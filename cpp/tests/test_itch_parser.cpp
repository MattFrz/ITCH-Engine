// Nasdaq TotalView-ITCH 5.0 parser tests.
//
// Two halves: hand-built messages checked field by field against the published
// layout (so the offsets are verified against the spec, not against the
// encoder), and an encoder/parser round trip over a generated event stream (so
// the replay simulator's capture is known to reproduce its input).

#include <cstring>
#include <vector>

#include "itch_engine/feed/itch_encoder.hpp"
#include "itch_engine/feed/itch_parser.hpp"
#include "test_support.hpp"

using namespace itch;
using namespace itch::feed;

namespace {

// Builds the 11-byte common header directly from the specification.
void put_header(std::uint8_t* b, char type, std::uint16_t locate, std::uint64_t ts_ns) {
    b[0] = static_cast<std::uint8_t>(type);
    store_be16(b + 1, locate);
    store_be16(b + 3, 0);
    store_be48(b + 5, ts_ns);
}

void test_add_order() {
    std::uint8_t msg[36] = {};
    put_header(msg, 'A', 7, 34200000000123ull);
    store_be64(msg + 11, 0x0102030405060708ull);
    msg[19] = 'B';
    store_be32(msg + 20, 250);
    std::memcpy(msg + 24, "AAPL    ", 8);
    store_be32(msg + 32, 1912500);  // $191.25 in 1/10000 USD

    ItchParser p;
    MarketEvent ev;
    CHECK(p.parse(msg, sizeof(msg), ev) == ParseResult::Ok);
    CHECK(ev.type == EventType::Add);
    CHECK(ev.side == Side::Bid);
    CHECK_EQ(ev.stock_locate, 7);
    CHECK_EQ(ev.order_id, 0x0102030405060708ull);
    CHECK_EQ(ev.quantity, 250);
    CHECK_EQ(ev.timestamp, 34200000000123ll);
    // 1912500 * 1e-4 USD = $191.25 -> 191.25e9 internal units.
    CHECK_EQ(ev.price, 191'250'000'000ll);

    msg[19] = 'S';
    CHECK(p.parse(msg, sizeof(msg), ev) == ParseResult::Ok);
    CHECK(ev.side == Side::Ask);
}

void test_add_order_with_mpid_is_the_same_event() {
    std::uint8_t msg[40] = {};
    put_header(msg, 'F', 1, 1000);
    store_be64(msg + 11, 42);
    msg[19] = 'B';
    store_be32(msg + 20, 100);
    std::memcpy(msg + 24, "AAPL    ", 8);
    store_be32(msg + 32, 10000);  // $1.0000
    std::memcpy(msg + 36, "MMQQ", 4);

    ItchParser p;
    MarketEvent ev;
    CHECK(p.parse(msg, sizeof(msg), ev) == ParseResult::Ok);
    CHECK(ev.type == EventType::Add);
    CHECK_EQ(ev.order_id, 42);
    CHECK_EQ(ev.price, PRICE_SCALE);  // exactly $1.00
}

void test_executed_cancel_delete_replace() {
    ItchParser p;
    MarketEvent ev;

    std::uint8_t exec[31] = {};
    put_header(exec, 'E', 1, 5);
    store_be64(exec + 11, 99);
    store_be32(exec + 19, 40);
    store_be64(exec + 23, 777);
    CHECK(p.parse(exec, sizeof(exec), ev) == ParseResult::Ok);
    CHECK(ev.type == EventType::Execute);
    CHECK_EQ(ev.order_id, 99);
    CHECK_EQ(ev.quantity, 40);
    // The execution message carries no book price: the book fills the resting
    // order at the price it already rests at.
    CHECK_EQ(ev.price, 0);

    // 'C' is an execution at a price different from the display price. It must
    // still not hand that price to the book.
    std::uint8_t execp[36] = {};
    put_header(execp, 'C', 1, 6);
    store_be64(execp + 11, 99);
    store_be32(execp + 19, 10);
    store_be64(execp + 23, 778);
    execp[31] = 'Y';
    store_be32(execp + 32, 1234567);
    CHECK(p.parse(execp, sizeof(execp), ev) == ParseResult::Ok);
    CHECK(ev.type == EventType::Execute);
    CHECK_EQ(ev.quantity, 10);
    CHECK_EQ(ev.price, 0);

    std::uint8_t cancel[23] = {};
    put_header(cancel, 'X', 1, 7);
    store_be64(cancel + 11, 99);
    store_be32(cancel + 19, 25);
    CHECK(p.parse(cancel, sizeof(cancel), ev) == ParseResult::Ok);
    CHECK(ev.type == EventType::Cancel);
    CHECK_EQ(ev.quantity, 25);

    std::uint8_t del[19] = {};
    put_header(del, 'D', 1, 8);
    store_be64(del + 11, 99);
    CHECK(p.parse(del, sizeof(del), ev) == ParseResult::Ok);
    CHECK(ev.type == EventType::Delete);
    CHECK_EQ(ev.order_id, 99);
    CHECK_EQ(ev.quantity, 0);

    std::uint8_t rep[35] = {};
    put_header(rep, 'U', 1, 9);
    store_be64(rep + 11, 99);
    store_be64(rep + 19, 100);
    store_be32(rep + 27, 500);
    store_be32(rep + 31, 2000000);  // $200.0000
    CHECK(p.parse(rep, sizeof(rep), ev) == ParseResult::Ok);
    CHECK(ev.type == EventType::Replace);
    CHECK_EQ(ev.order_id, 99);
    CHECK_EQ(ev.new_order_id, 100);
    CHECK_EQ(ev.quantity, 500);
    CHECK_EQ(ev.price, 200LL * PRICE_SCALE);
    // Side is not on the wire for a replace; the book inherits it.
    CHECK(ev.side == Side::Unknown);
}

void test_trades_and_system_events() {
    ItchParser p;
    MarketEvent ev;

    std::uint8_t trade[44] = {};
    put_header(trade, 'P', 3, 11);
    store_be64(trade + 11, 5);
    trade[19] = 'B';
    store_be32(trade + 20, 300);
    std::memcpy(trade + 24, "AAPL    ", 8);
    store_be32(trade + 32, 1500000);
    store_be64(trade + 36, 8888);
    CHECK(p.parse(trade, sizeof(trade), ev) == ParseResult::Ok);
    CHECK(ev.type == EventType::Trade);
    CHECK_EQ(ev.quantity, 300);
    CHECK_EQ(ev.price, 150LL * PRICE_SCALE);
    CHECK(!mutates_book(ev.type));  // a print, not a book change

    std::uint8_t cross[40] = {};
    put_header(cross, 'Q', 3, 12);
    store_be64(cross + 11, 1000000);
    std::memcpy(cross + 19, "AAPL    ", 8);
    store_be32(cross + 27, 1500000);
    store_be64(cross + 31, 9999);
    cross[39] = 'O';
    CHECK(p.parse(cross, sizeof(cross), ev) == ParseResult::Ok);
    CHECK(ev.type == EventType::Trade);
    CHECK_EQ(ev.quantity, 1000000);

    // 'S' with code 'O' is the one point where the book is known empty, which
    // is what the historical path spells "Clear".
    std::uint8_t sys[12] = {};
    put_header(sys, 'S', 0, 13);
    sys[11] = 'O';
    CHECK(p.parse(sys, sizeof(sys), ev) == ParseResult::Ok);
    CHECK(ev.type == EventType::Clear);

    sys[11] = 'Q';  // start of market hours
    CHECK(p.parse(sys, sizeof(sys), ev) == ParseResult::Ok);
    CHECK(ev.type == EventType::SystemEvent);
    CHECK_EQ(ev.quantity, static_cast<std::uint32_t>('Q'));

    ParserConfig cfg;
    cfg.start_of_messages_is_clear = false;
    p.set_config(cfg);
    sys[11] = 'O';
    CHECK(p.parse(sys, sizeof(sys), ev) == ParseResult::Ok);
    CHECK(ev.type == EventType::SystemEvent);
}

void test_bounds_and_unknown_types() {
    ItchParser p;
    MarketEvent ev;

    // Shorter than the common header.
    std::uint8_t tiny[5] = {'A', 0, 0, 0, 0};
    CHECK(p.parse(tiny, sizeof(tiny), ev) == ParseResult::BadLength);

    // Right type, wrong length: the spec fixes every message size, so a
    // mismatch is a corrupt stream, not something to decode optimistically.
    std::uint8_t shortadd[30] = {};
    put_header(shortadd, 'A', 1, 1);
    CHECK(p.parse(shortadd, sizeof(shortadd), ev) == ParseResult::BadLength);

    std::uint8_t longadd[37] = {};
    put_header(longadd, 'A', 1, 1);
    CHECK(p.parse(longadd, sizeof(longadd), ev) == ParseResult::BadLength);

    std::uint8_t unknown[20] = {};
    put_header(unknown, '\x7f', 1, 1);
    CHECK(p.parse(unknown, sizeof(unknown), ev) == ParseResult::UnknownType);

    // Reference data is understood and deliberately produces no event.
    std::uint8_t dir[39] = {};
    put_header(dir, 'R', 1, 1);
    CHECK(p.parse(dir, sizeof(dir), ev) == ParseResult::Ignored);

    CHECK_EQ(p.stats().bad_length, 3);
    CHECK_EQ(p.stats().unknown_type, 1);
    CHECK_EQ(p.stats().ignored, 1);
}

void test_session_epoch_and_locate_filter() {
    ParserConfig cfg;
    cfg.session_epoch_ns = 1'700'000'000'000'000'000LL;
    cfg.stock_locate_filter = 7;
    ItchParser p(cfg);
    MarketEvent ev;

    std::uint8_t msg[36] = {};
    put_header(msg, 'A', 7, 12345);
    store_be64(msg + 11, 1);
    msg[19] = 'B';
    store_be32(msg + 20, 10);
    store_be32(msg + 32, 10000);
    CHECK(p.parse(msg, sizeof(msg), ev) == ParseResult::Ok);
    CHECK_EQ(ev.timestamp, 1'700'000'000'000'000'000LL + 12345);

    put_header(msg, 'A', 8, 12345);  // a different symbol
    CHECK(p.parse(msg, sizeof(msg), ev) == ParseResult::Ignored);

    // System events are never filtered out: they are session-wide.
    std::uint8_t sys[12] = {};
    put_header(sys, 'S', 0, 1);
    sys[11] = 'O';
    CHECK(p.parse(sys, sizeof(sys), ev) == ParseResult::Ok);
}

// Encode a generated stream and parse it back. Adds, executes and replaces
// must survive byte for byte; cancels and modifies legitimately change shape
// (a Databento-style modify becomes 'X' or 'U' on the wire) and are checked in
// test_pipeline.cpp by book state instead.
void test_encoder_round_trip_for_direct_types() {
    EncoderConfig ecfg;
    ecfg.session_epoch_ns = 1'700'000'000'000'000'000LL;
    ecfg.stock_locate = 4;
    ItchEncoder enc(ecfg);

    ParserConfig pcfg;
    pcfg.session_epoch_ns = ecfg.session_epoch_ns;
    ItchParser parser(pcfg);

    MarketEvent add;
    add.timestamp = ecfg.session_epoch_ns + 999;
    add.order_id = 123456789;
    add.price = 191'230'000'000LL;
    add.quantity = 700;
    add.side = Side::Ask;
    add.type = EventType::Add;

    MarketEvent decoded;
    int messages = 0;
    CHECK(enc.encode(add, [&](const std::uint8_t* m, std::uint8_t len) {
        ++messages;
        CHECK(parser.parse(m, len, decoded) == ParseResult::Ok);
    }));
    CHECK_EQ(messages, 1);
    CHECK(decoded.type == EventType::Add);
    CHECK_EQ(decoded.order_id, add.order_id);
    CHECK_EQ(decoded.price, add.price);
    CHECK_EQ(decoded.quantity, add.quantity);
    CHECK(decoded.side == Side::Ask);
    CHECK_EQ(decoded.timestamp, add.timestamp);
    CHECK_EQ(decoded.stock_locate, 4);

    MarketEvent exec = add;
    exec.type = EventType::Execute;
    exec.quantity = 100;
    CHECK(enc.encode(exec, [&](const std::uint8_t* m, std::uint8_t len) {
        CHECK(parser.parse(m, len, decoded) == ParseResult::Ok);
    }));
    CHECK(decoded.type == EventType::Execute);
    CHECK_EQ(decoded.quantity, 100);

    // A price that is not a whole number of ITCH ticks cannot be represented;
    // the encoder reports that rather than rounding it away.
    MarketEvent bad = add;
    bad.order_id = 5;
    bad.price = 191'230'000'001LL;
    bool emitted_anything = false;
    CHECK(!enc.encode(bad, [&](const std::uint8_t*, std::uint8_t) { emitted_anything = true; }));
    CHECK(!emitted_anything);
    CHECK_EQ(enc.stats().price_not_representable, 1);
}

}  // namespace

int main() {
    test_add_order();
    test_add_order_with_mpid_is_the_same_event();
    test_executed_cancel_delete_replace();
    test_trades_and_system_events();
    test_bounds_and_unknown_types();
    test_session_epoch_and_locate_filter();
    test_encoder_round_trip_for_direct_types();
    itch_test::pass("itch parser");
    return 0;
}
