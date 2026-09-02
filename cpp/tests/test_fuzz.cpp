// Fuzz the network-facing decoders with hostile input.
//
// MoldUDP64Decoder and ItchParser are the only code in this project that reads
// bytes it did not produce. On a live feed those bytes arrive from a socket, so
// every length, count and offset in them is attacker-influenced in the threat
// model that matters: a malformed or truncated datagram must be rejected, not
// read past.
//
// This throws random and adversarial input at both, plus at the whole
// decode -> parse -> book chain, and asserts only that nothing reads out of
// bounds and no invariant breaks. Build it with -DITCH_ENABLE_ASAN=ON to make
// an out-of-bounds read an immediate failure rather than a silent one.
//
// The bounds argument it is checking:
//   * decode() rejects anything shorter than a 20-byte header, then validates
//     EVERY message block against the datagram length BEFORE delivering any of
//     them, so a truncated packet is dropped whole rather than half-applied.
//   * parse() requires the message length to equal the length the ITCH
//     specification fixes for that message type, so every field read afterwards
//     is provably inside the buffer.

#include <cstdint>
#include <cstring>
#include <vector>

#include "itch_engine/book/low_latency_book.hpp"
#include "itch_engine/feed/itch_encoder.hpp"
#include "itch_engine/feed/itch_parser.hpp"
#include "itch_engine/feed/moldudp64.hpp"
#include "test_support.hpp"

using namespace itch;
using namespace itch::feed;
using itch_test::Rng;

namespace {

std::uint64_t delivered = 0;
std::uint64_t parsed_ok = 0;
std::uint64_t applied = 0;

// Random bytes of random length, including lengths around every boundary the
// decoder cares about.
void test_random_datagrams() {
    Rng rng(0xF0FDU);
    MoldUDP64Decoder dec;
    dec.set_auto_resync(true);
    ItchParser parser;
    MarketEvent ev;
    std::vector<std::uint8_t> buf;

    for (int iter = 0; iter < 200000; ++iter) {
        // Bias lengths toward the interesting region: 0, just under and just
        // over the 20-byte header, and typical datagram sizes.
        std::size_t len;
        switch (rng.below(4)) {
            case 0: len = rng.below(24); break;                 // around the header
            case 1: len = 20 + rng.below(8); break;             // header + a stub
            case 2: len = rng.below(200); break;
            default: len = rng.below(1500); break;
        }
        buf.assign(len, 0);
        for (std::size_t i = 0; i < len; ++i) {
            buf[i] = static_cast<std::uint8_t>(rng.below(256));
        }
        // The decoder must survive any of this without reading past `len`.
        dec.decode(buf.data(), len,
                   [&](std::uint64_t, const std::uint8_t* msg, std::uint16_t mlen) {
                       ++delivered;
                       // Every delivered block must lie inside the datagram.
                       CHECK(msg >= buf.data());
                       CHECK(msg + mlen <= buf.data() + len);
                       if (parser.parse(msg, mlen, ev) == ParseResult::Ok) ++parsed_ok;
                   });
    }
    CHECK(dec.stats().packets == 200000);
}

// Well-formed headers with adversarial message counts and block lengths: the
// case where a length field lies about how much data follows.
void test_lying_length_fields() {
    Rng rng(0xBADCAFE);
    MoldUDP64Decoder dec;
    dec.set_auto_resync(true);
    std::vector<std::uint8_t> buf;

    for (int iter = 0; iter < 100000; ++iter) {
        const std::size_t len = 20 + rng.below(300);
        buf.assign(len, 0);
        std::memset(buf.data(), 'S', kMoldSessionLen);
        store_be64(buf.data() + 10, rng.next() % 1000);
        // A count that is usually far larger than the bytes present.
        store_be16(buf.data() + 18, static_cast<std::uint16_t>(rng.below(2000)));
        // Block length fields that claim more than the datagram holds.
        for (std::size_t off = 20; off + 2 <= len; off += 2) {
            store_be16(buf.data() + off, static_cast<std::uint16_t>(rng.below(70000)));
        }
        dec.decode(buf.data(), len,
                   [&](std::uint64_t, const std::uint8_t* msg, std::uint16_t mlen) {
                       ++delivered;
                       CHECK(msg + mlen <= buf.data() + len);
                   });
    }
    // Almost all of these must be rejected as malformed, and none may have
    // delivered a block that runs off the end (asserted above).
    CHECK(dec.stats().malformed_packets > 90000);
}

// Every ITCH message type at every length from 0 to 64, with random payloads:
// the parser must never accept a length the specification does not fix.
void test_parser_length_matrix() {
    Rng rng(0x5EED);
    ItchParser parser;
    MarketEvent ev;
    std::uint8_t buf[128];

    const char types[] = {'S','R','H','Y','L','V','W','K','J','h','A','F','E',
                          'C','X','D','U','P','Q','B','I','N','\0','~','\xff'};
    for (char t : types) {
        for (std::size_t len = 0; len <= 64; ++len) {
            for (int rep = 0; rep < 40; ++rep) {
                for (std::size_t i = 0; i < sizeof(buf); ++i) {
                    buf[i] = static_cast<std::uint8_t>(rng.below(256));
                }
                buf[0] = static_cast<std::uint8_t>(t);
                const ParseResult r =
                    parser.parse(buf, static_cast<std::uint16_t>(len), ev);
                if (r == ParseResult::Ok || r == ParseResult::Ignored) {
                    // Accepted only at exactly the specified length.
                    CHECK(len == message_length(static_cast<std::uint8_t>(t)));
                }
            }
        }
    }
}

// The whole chain, including the book: random bytes must never corrupt book
// invariants or trip a capacity failure that was not reported.
void test_chain_into_the_book() {
    Rng rng(0xC0FFEE);
    book::BookConfig cfg;
    cfg.max_orders = 1u << 14;
    cfg.index_capacity = 1u << 15;
    cfg.max_levels = 1u << 12;
    cfg.tick_count = 1u << 14;
    book::LowLatencyOrderBook bk(cfg);

    MoldUDP64Decoder dec;
    dec.set_auto_resync(true);
    ItchParser parser;
    MarketEvent ev;
    std::vector<std::uint8_t> buf;

    for (int iter = 0; iter < 100000; ++iter) {
        const std::size_t len = rng.below(600);
        buf.assign(len, 0);
        for (std::size_t i = 0; i < len; ++i) {
            buf[i] = static_cast<std::uint8_t>(rng.below(256));
        }
        dec.decode(buf.data(), len,
                   [&](std::uint64_t, const std::uint8_t* msg, std::uint16_t mlen) {
                       if (parser.parse(msg, mlen, ev) != ParseResult::Ok) return;
                       bk.apply(ev);
                       ++applied;
                   });
        // Book invariants must hold no matter what was fed in.
        const BestQuote q = bk.best();
        if (q.has_bid) CHECK(q.bid_qty >= 0);
        if (q.has_ask) CHECK(q.ask_qty >= 0);
        CHECK(bk.open_order_count() <= cfg.max_orders);
        CHECK(bk.bid_level_count() + bk.ask_level_count() <= cfg.max_levels);
    }
    // Capacity exhaustion is allowed here (random adds are unbounded); what is
    // NOT allowed is exhausting it without saying so.
    if (bk.metrics().capacity_rejections > 0) CHECK(bk.degraded());
}

// Mutation fuzzing: start from a STRUCTURALLY VALID packet built by the
// encoder, then corrupt it. This is the case that matters. Purely random bytes
// almost never form a packet whose block lengths add up, so they are rejected
// at the first check and never reach the parser at all - which proves very
// little. Corrupting a valid packet drives real messages deep into the decoder
// and the book with hostile field values.
void test_mutated_valid_packets() {
    Rng rng(0x1234ABCD);
    book::BookConfig cfg;
    cfg.max_orders = 1u << 14;
    cfg.index_capacity = 1u << 15;
    cfg.max_levels = 1u << 12;
    cfg.tick_count = 1u << 14;
    book::LowLatencyOrderBook bk(cfg);

    MoldUDP64Decoder dec;
    dec.set_auto_resync(true);
    ItchParser parser;
    MarketEvent ev;

    // A pool of real events to encode from.
    itch_test::GeneratorConfig gcfg;
    gcfg.event_count = 20000;
    const auto events = itch_test::generate_events(gcfg, 99);

    EncoderConfig ecfg;
    ecfg.session_epoch_ns = events.front().timestamp / 86'400'000'000'000LL *
                            86'400'000'000'000LL;
    ItchEncoder enc(ecfg);
    MoldUDP64Packer packer("FUZZCAP", 1, 1400);

    std::vector<std::uint8_t> pkt;
    std::size_t ev_idx = 0;
    std::uint64_t mutated_delivered = 0;

    for (int iter = 0; iter < 150000; ++iter) {
        // Build one valid datagram.
        pkt.clear();
        int packed = 0;
        while (packed < 8 && ev_idx < events.size()) {
            enc.encode(events[ev_idx++], [&](const std::uint8_t* m, std::uint8_t mlen) {
                if (packer.would_overflow(mlen)) {
                    const std::uint8_t* out = nullptr;
                    const std::size_t n = packer.flush(&out);
                    if (n > 0) pkt.assign(out, out + n);
                }
                packer.append(m, mlen);
                ++packed;
            });
            if (!pkt.empty()) break;
        }
        if (pkt.empty()) {
            const std::uint8_t* out = nullptr;
            const std::size_t n = packer.flush(&out);
            if (n == 0) { ev_idx = 0; enc.reset(); continue; }
            pkt.assign(out, out + n);
        }
        if (ev_idx >= events.size()) { ev_idx = 0; enc.reset(); }

        // Corrupt it: flip bytes, and sometimes truncate.
        const std::uint32_t flips = rng.below(6);
        for (std::uint32_t f = 0; f < flips && !pkt.empty(); ++f) {
            pkt[rng.below(static_cast<std::uint32_t>(pkt.size()))] =
                static_cast<std::uint8_t>(rng.below(256));
        }
        if (rng.below(4) == 0 && pkt.size() > 1) {
            pkt.resize(rng.below(static_cast<std::uint32_t>(pkt.size())));
        }

        dec.decode(pkt.data(), pkt.size(),
                   [&](std::uint64_t, const std::uint8_t* msg, std::uint16_t mlen) {
                       ++mutated_delivered;
                       // Bounds: the block must lie inside the datagram.
                       CHECK(msg >= pkt.data());
                       CHECK(msg + mlen <= pkt.data() + pkt.size());
                       if (parser.parse(msg, mlen, ev) != ParseResult::Ok) return;
                       bk.apply(ev);
                       ++applied;
                   });

        CHECK(bk.open_order_count() <= cfg.max_orders);
        CHECK(bk.bid_level_count() + bk.ask_level_count() <= cfg.max_levels);
    }

    delivered += mutated_delivered;
    // This is the assertion that makes the whole test meaningful: corrupted
    // but structurally plausible packets really did reach the parser in bulk.
    CHECK(mutated_delivered > 100000);
    if (bk.metrics().capacity_rejections > 0) CHECK(bk.degraded());
    std::printf("  mutation fuzz: %llu blocks reached the parser from corrupted packets\n",
                static_cast<unsigned long long>(mutated_delivered));
}

}  // namespace

int main() {
    test_random_datagrams();
    test_lying_length_fields();
    test_parser_length_matrix();
    test_chain_into_the_book();
    test_mutated_valid_packets();
    std::printf("  fuzz: %llu blocks delivered, %llu parsed, %llu applied to the book\n",
                static_cast<unsigned long long>(delivered),
                static_cast<unsigned long long>(parsed_ok),
                static_cast<unsigned long long>(applied));
    itch_test::pass("fuzz (malformed network input)");
    return 0;
}
