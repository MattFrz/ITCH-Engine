#pragma once

// Shared test harness: plain asserts (no framework dependency, exits nonzero
// on the first failure with file/line, which is all CTest needs) plus a
// deterministic market-event generator.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "itch_engine/market_event.hpp"

namespace itch_test {

inline int checks_run = 0;

inline void check(bool cond, const char* expr, const char* file, int line) {
    ++checks_run;
    if (!cond) {
        std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expr);
        std::exit(1);
    }
}

inline void check_eq_i64(long long got, long long want, const char* expr, const char* file,
                         int line) {
    ++checks_run;
    if (got != want) {
        std::fprintf(stderr, "FAIL %s:%d: %s\n  got  %lld\n  want %lld\n", file, line, expr, got,
                     want);
        std::exit(1);
    }
}

#define CHECK(cond) ::itch_test::check((cond), #cond, __FILE__, __LINE__)
#define CHECK_EQ(got, want) \
    ::itch_test::check_eq_i64(static_cast<long long>(got), static_cast<long long>(want), \
                              #got " == " #want, __FILE__, __LINE__)

inline void pass(const char* name) {
    std::printf("PASS: %s (%d checks)\n", name, checks_run);
}

// splitmix64: small, fast, and identical on every platform, which matters more
// here than statistical quality. Every test stream is reproducible from a seed.
class Rng {
public:
    explicit Rng(std::uint64_t seed) : s_(seed) {}
    std::uint64_t next() {
        std::uint64_t z = (s_ += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    std::uint32_t below(std::uint32_t n) { return static_cast<std::uint32_t>(next() % n); }
    // Inclusive range.
    std::int64_t between(std::int64_t lo, std::int64_t hi) {
        return lo + static_cast<std::int64_t>(next() % static_cast<std::uint64_t>(hi - lo + 1));
    }

private:
    std::uint64_t s_;
};

struct GeneratorConfig {
    std::size_t event_count = 200000;
    itch::Price base_price = 190LL * itch::PRICE_SCALE;  // $190.00
    int levels = 40;                                     // ticks either side of base
    itch::Price tick = 10'000'000;                       // $0.01

    // Anomaly and edge-case injection. These are not noise: each one is a case
    // the two books have to agree on, and the real AAPL session contains all
    // of them.
    bool include_modify = true;
    bool include_replace = true;
    bool include_unknown_refs = true;   // cancel/execute for ids never added
    bool include_duplicate_adds = true;
    bool include_zero_quantities = true;
    bool include_offgrid_prices = true; // prices off the penny grid
    bool include_far_prices = true;     // prices outside the tick window
    bool include_clear = true;
    bool include_trades = true;         // non-book-affecting prints
};

// Generates a realistic-shaped MBO stream: adds dominate, cancels close
// behind, executions and modifies sparse - the message mix of a real ITCH day.
// It tracks its own live orders so cancels and executes mostly reference real
// ones, with a deliberate minority that do not.
inline std::vector<itch::MarketEvent> generate_events(const GeneratorConfig& cfg,
                                                      std::uint64_t seed) {
    using namespace itch;
    std::vector<MarketEvent> out;
    out.reserve(cfg.event_count);
    Rng rng(seed);

    struct Live {
        OrderId id;
        Price price;
        std::uint32_t qty;
        Side side;
    };
    std::vector<Live> live;
    live.reserve(cfg.event_count / 4);

    OrderId next_id = 1;
    Timestamp ts = 1'700'000'000'000'000'000LL;

    for (std::size_t i = 0; i < cfg.event_count; ++i) {
        ts += rng.between(1, 5000);
        MarketEvent ev;
        ev.timestamp = ts;
        ev.stock_locate = 1;

        const std::uint32_t roll = rng.below(1000);

        if (cfg.include_clear && i > 0 && i % 97777 == 0) {
            ev.type = EventType::Clear;
            ev.side = Side::Unknown;
            ev.order_id = 0;
            ev.quantity = 0;
            ev.price = 0;
            out.push_back(ev);
            live.clear();
            continue;
        }

        if (cfg.include_trades && roll >= 995) {
            ev.type = EventType::Trade;
            ev.order_id = next_id++;
            ev.side = rng.below(2) ? Side::Ask : Side::Bid;
            ev.quantity = static_cast<std::uint32_t>(rng.between(1, 500));
            ev.price = cfg.base_price;
            out.push_back(ev);
            continue;
        }

        // Adds until there is something to act on, then a realistic mix.
        const bool force_add = live.size() < 64;
        if (force_add || roll < 480) {
            ev.type = EventType::Add;
            ev.side = rng.below(2) ? Side::Ask : Side::Bid;
            ev.quantity = static_cast<std::uint32_t>(rng.between(1, 900));
            if (cfg.include_zero_quantities && roll == 479) ev.quantity = 0;

            const std::int64_t offset = rng.between(-cfg.levels, cfg.levels);
            ev.price = cfg.base_price + offset * cfg.tick;
            if (cfg.include_offgrid_prices && roll == 3) {
                // Off the penny grid but still on Nasdaq's 1/10000 USD grid,
                // which is exactly what a real sub-penny quote looks like.
                ev.price += cfg.tick / 10;
            }
            if (cfg.include_far_prices && roll == 4) {
                // Far outside the default tick window ($10,485.75).
                ev.price = 150'000LL * PRICE_SCALE + rng.between(0, 100) * cfg.tick;
            }
            if (cfg.include_far_prices && roll == 5) {
                ev.price = 100'000;  // $0.0001, the low end of the real feed
            }

            if (cfg.include_duplicate_adds && roll == 6 && !live.empty()) {
                ev.order_id = live[rng.below(static_cast<std::uint32_t>(live.size()))].id;
            } else {
                ev.order_id = next_id++;
                if (ev.quantity > 0) {
                    live.push_back(Live{ev.order_id, ev.price, ev.quantity, ev.side});
                }
            }
            out.push_back(ev);
            continue;
        }

        if (cfg.include_unknown_refs && roll >= 985) {
            // A reference that was never added: both books must tolerate and
            // count it identically.
            ev.type = rng.below(2) ? EventType::Cancel : EventType::Execute;
            ev.order_id = 900'000'000'000ull + rng.next() % 1000;
            ev.quantity = static_cast<std::uint32_t>(rng.between(0, 300));
            ev.side = Side::Unknown;
            ev.price = 0;
            out.push_back(ev);
            continue;
        }

        if (live.empty()) continue;
        const std::uint32_t pick = rng.below(static_cast<std::uint32_t>(live.size()));
        Live& target = live[pick];
        ev.order_id = target.id;
        ev.side = target.side;

        if (roll < 800) {
            // Cancel: full more often than partial, as on a real feed.
            const bool full = rng.below(100) < 70;
            ev.type = full ? (rng.below(2) ? EventType::Delete : EventType::Cancel)
                           : EventType::Cancel;
            ev.price = target.price;
            if (full) {
                ev.quantity = ev.type == EventType::Delete
                                  ? 0
                                  : (rng.below(2) ? 0 : target.qty);
                live[pick] = live.back();
                live.pop_back();
            } else {
                const std::uint32_t take =
                    static_cast<std::uint32_t>(rng.between(1, target.qty > 1 ? target.qty - 1 : 1));
                ev.quantity = take;
                if (take >= target.qty) {
                    live[pick] = live.back();
                    live.pop_back();
                } else {
                    target.qty -= take;
                }
            }
            out.push_back(ev);
            continue;
        }

        if (roll < 900) {
            ev.type = EventType::Execute;
            ev.price = 0;
            const bool full = rng.below(100) < 55;
            const std::uint32_t take =
                full ? target.qty
                     : static_cast<std::uint32_t>(rng.between(0, target.qty));
            ev.quantity = take;
            if (take >= target.qty && take > 0) {
                live[pick] = live.back();
                live.pop_back();
            } else if (take > 0) {
                target.qty -= take;
            }
            out.push_back(ev);
            continue;
        }

        if (cfg.include_replace && roll >= 950) {
            ev.type = EventType::Replace;
            ev.new_order_id = rng.below(10) == 0 ? target.id : next_id++;
            ev.quantity = static_cast<std::uint32_t>(rng.between(0, 900));
            const std::int64_t offset = rng.between(-cfg.levels, cfg.levels);
            ev.price = cfg.base_price + offset * cfg.tick;
            const Side side = target.side;
            live[pick] = live.back();
            live.pop_back();
            if (ev.quantity > 0) {
                live.push_back(Live{ev.new_order_id, ev.price, ev.quantity, side});
            }
            out.push_back(ev);
            continue;
        }

        if (cfg.include_modify) {
            ev.type = EventType::Modify;
            const bool same_price = rng.below(100) < 60;
            ev.price = same_price ? target.price
                                  : cfg.base_price + rng.between(-cfg.levels, cfg.levels) * cfg.tick;
            if (same_price) {
                // Mostly a size-down (keeps priority), sometimes a size-up.
                ev.quantity = rng.below(100) < 80
                                  ? static_cast<std::uint32_t>(rng.between(0, target.qty))
                                  : target.qty + static_cast<std::uint32_t>(rng.between(1, 200));
            } else {
                ev.quantity = static_cast<std::uint32_t>(rng.between(0, 900));
            }
            if (ev.quantity == 0) {
                live[pick] = live.back();
                live.pop_back();
            } else {
                target.price = ev.price;
                target.qty = ev.quantity;
            }
            out.push_back(ev);
            continue;
        }

        // Fall through: a plain full cancel.
        ev.type = EventType::Delete;
        ev.quantity = 0;
        ev.price = target.price;
        live[pick] = live.back();
        live.pop_back();
        out.push_back(ev);
    }

    return out;
}

}  // namespace itch_test
