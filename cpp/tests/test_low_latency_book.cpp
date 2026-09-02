// LowLatencyOrderBook semantics, tested standalone.
//
// test_differential.cpp proves this book agrees with the validated research
// book. These tests prove the semantics are the intended ones in the first
// place, so a shared misunderstanding cannot pass both.

#include <vector>

#include "itch_engine/book/low_latency_book.hpp"
#include "test_support.hpp"

using namespace itch;
using namespace itch::book;

namespace {

constexpr Price P(double usd) {
    return static_cast<Price>(usd * static_cast<double>(PRICE_SCALE));
}

MarketEvent add(OrderId id, Side side, Price price, std::uint32_t qty) {
    MarketEvent e;
    e.type = EventType::Add;
    e.order_id = id;
    e.side = side;
    e.price = price;
    e.quantity = qty;
    return e;
}
MarketEvent cancel(OrderId id, std::uint32_t qty = 0) {
    MarketEvent e;
    e.type = EventType::Cancel;
    e.order_id = id;
    e.quantity = qty;
    return e;
}
MarketEvent del(OrderId id) {
    MarketEvent e;
    e.type = EventType::Delete;
    e.order_id = id;
    return e;
}
MarketEvent execute(OrderId id, std::uint32_t qty) {
    MarketEvent e;
    e.type = EventType::Execute;
    e.order_id = id;
    e.quantity = qty;
    return e;
}
MarketEvent replace(OrderId id, OrderId new_id, Price price, std::uint32_t qty) {
    MarketEvent e;
    e.type = EventType::Replace;
    e.order_id = id;
    e.new_order_id = new_id;
    e.price = price;
    e.quantity = qty;
    return e;
}
MarketEvent modify(OrderId id, Price price, std::uint32_t qty) {
    MarketEvent e;
    e.type = EventType::Modify;
    e.order_id = id;
    e.price = price;
    e.quantity = qty;
    return e;
}

void check_queue(const LowLatencyOrderBook& b, Side side, Price price,
                 const std::vector<OrderId>& want) {
    const std::vector<OrderId> got = b.queue_at(side, price);
    CHECK_EQ(got.size(), want.size());
    for (std::size_t i = 0; i < want.size(); ++i) CHECK_EQ(got[i], want[i]);
}

void test_best_and_depth() {
    LowLatencyOrderBook b;
    b.apply(add(1, Side::Bid, P(100.00), 100));
    b.apply(add(2, Side::Bid, P(100.01), 50));
    b.apply(add(3, Side::Ask, P(100.03), 75));
    b.apply(add(4, Side::Ask, P(100.02), 25));

    BestQuote q = b.best();
    CHECK(q.has_bid && q.has_ask);
    CHECK_EQ(q.bid_price, P(100.01));
    CHECK_EQ(q.bid_qty, 50);
    CHECK_EQ(q.ask_price, P(100.02));
    CHECK_EQ(q.ask_qty, 25);
    CHECK_EQ(b.open_order_count(), 4);
    CHECK_EQ(b.bid_level_count(), 2);
    CHECK_EQ(b.ask_level_count(), 2);
    CHECK(b.mid_price().has_value());
    CHECK(*b.mid_price() > 100.0149 && *b.mid_price() < 100.0151);

    const auto bids = b.depth(Side::Bid, 5);
    CHECK_EQ(bids.size(), 2);
    CHECK_EQ(bids[0].price, P(100.01));
    CHECK_EQ(bids[1].price, P(100.00));
    const auto asks = b.depth(Side::Ask, 5);
    CHECK_EQ(asks[0].price, P(100.02));
    CHECK_EQ(asks[1].price, P(100.03));
}

// The exact sequence named in the specification for this work.
void test_fifo_scenario() {
    LowLatencyOrderBook b;
    const Price px = P(50.00);
    b.apply(add(1, Side::Bid, px, 100));  // A
    b.apply(add(2, Side::Bid, px, 200));  // B
    b.apply(add(3, Side::Bid, px, 300));  // C
    check_queue(b, Side::Bid, px, {1, 2, 3});
    CHECK_EQ(b.bid_qty_at(px), 600);
    CHECK_EQ(*b.queue_ahead(1), 0);
    CHECK_EQ(*b.queue_ahead(2), 100);
    CHECK_EQ(*b.queue_ahead(3), 300);

    b.apply(execute(1, 100));  // A fully executed
    check_queue(b, Side::Bid, px, {2, 3});
    CHECK(!b.order_qty(1).has_value());
    CHECK_EQ(*b.queue_ahead(2), 0);
    CHECK_EQ(*b.queue_ahead(3), 200);
    CHECK_EQ(b.bid_qty_at(px), 500);

    b.apply(cancel(2, 50));  // B partially cancelled: keeps its position
    check_queue(b, Side::Bid, px, {2, 3});
    CHECK_EQ(*b.order_qty(2), 150);
    CHECK_EQ(*b.queue_ahead(3), 150);
    CHECK_EQ(b.bid_qty_at(px), 450);

    b.apply(replace(3, 4, px, 300));  // C replaced: new reference, back of queue
    check_queue(b, Side::Bid, px, {2, 4});
    CHECK(!b.order_qty(3).has_value());
    CHECK_EQ(*b.order_qty(4), 300);
    CHECK_EQ(*b.queue_ahead(4), 150);

    b.apply(del(4));  // C deleted
    check_queue(b, Side::Bid, px, {2});
    CHECK_EQ(b.bid_qty_at(px), 150);
    CHECK_EQ(b.open_order_count(), 1);

    b.apply(del(2));
    CHECK_EQ(b.bid_level_count(), 0);
    CHECK(!b.best().has_bid);
    CHECK_EQ(b.open_order_count(), 0);
    CHECK_EQ(b.levels_in_use(), 0);
    CHECK_EQ(b.orders_in_use(), 0);
}

void test_partial_cancel_keeps_priority_and_full_removes() {
    LowLatencyOrderBook b;
    b.apply(add(1, Side::Ask, P(50.00), 100));
    b.apply(add(2, Side::Ask, P(50.00), 200));
    b.apply(cancel(2, 150));
    CHECK_EQ(b.ask_qty_at(P(50.00)), 150);
    CHECK_EQ(*b.order_qty(2), 50);
    CHECK_EQ(*b.queue_ahead(2), 100);

    // A cancel of at least everything remaining removes the order outright.
    b.apply(cancel(2, 9999));
    CHECK(!b.order_qty(2).has_value());
    CHECK_EQ(b.ask_qty_at(P(50.00)), 100);

    // Cancel with quantity 0 is a full cancel.
    b.apply(cancel(1, 0));
    CHECK_EQ(b.ask_level_count(), 0);
}

void test_cancel_in_middle_of_queue() {
    LowLatencyOrderBook b;
    for (OrderId id = 1; id <= 5; ++id) {
        b.apply(add(id, Side::Bid, P(10.00), static_cast<std::uint32_t>(10 * id)));
    }
    CHECK_EQ(*b.queue_ahead(4), 10 + 20 + 30);
    b.apply(del(3));
    CHECK_EQ(*b.queue_ahead(4), 10 + 20);
    CHECK_EQ(*b.queue_ahead(5), 10 + 20 + 40);
    CHECK_EQ(b.bid_qty_at(P(10.00)), 10 + 20 + 40 + 50);
    check_queue(b, Side::Bid, P(10.00), {1, 2, 4, 5});
}

void test_modify_semantics() {
    LowLatencyOrderBook b;
    b.apply(add(1, Side::Ask, P(30.00), 100));
    b.apply(add(2, Side::Ask, P(30.00), 100));

    b.apply(modify(1, P(30.00), 60));  // size-down at same price keeps the front
    CHECK_EQ(*b.queue_ahead(1), 0);
    CHECK_EQ(*b.order_qty(1), 60);
    CHECK_EQ(b.ask_qty_at(P(30.00)), 160);

    b.apply(modify(1, P(30.00), 120));  // size-up loses priority
    CHECK_EQ(*b.queue_ahead(1), 100);
    CHECK_EQ(*b.queue_ahead(2), 0);
    check_queue(b, Side::Ask, P(30.00), {2, 1});

    b.apply(modify(1, P(29.99), 120));  // reprice moves levels
    CHECK_EQ(b.ask_qty_at(P(30.00)), 100);
    CHECK_EQ(b.ask_qty_at(P(29.99)), 120);
    CHECK_EQ(b.best().ask_price, P(29.99));

    b.apply(modify(1, P(29.99), 0));  // modify to zero removes it
    CHECK(!b.order_qty(1).has_value());
    CHECK_EQ(b.ask_level_count(), 1);
}

void test_execute_semantics() {
    LowLatencyOrderBook b;
    b.apply(add(1, Side::Bid, P(20.00), 30));
    b.apply(add(2, Side::Bid, P(20.00), 40));
    b.apply(execute(1, 30));
    CHECK(!b.order_qty(1).has_value());
    CHECK_EQ(*b.queue_ahead(2), 0);
    b.apply(execute(2, 15));
    CHECK_EQ(*b.order_qty(2), 25);
    CHECK_EQ(b.bid_qty_at(P(20.00)), 25);

    // An execution for more than is resting fills only what is there.
    b.apply(execute(2, 1000));
    CHECK(!b.order_qty(2).has_value());
    CHECK_EQ(b.bid_level_count(), 0);

    // An execution for zero shares is a no-op, not a delete.
    b.apply(add(3, Side::Bid, P(20.00), 10));
    b.apply(execute(3, 0));
    CHECK_EQ(*b.order_qty(3), 10);
}

void test_anomalies_are_tolerated_and_counted() {
    LowLatencyOrderBook b;
    b.apply(cancel(42));
    b.apply(execute(99, 10));
    CHECK_EQ(b.unknown_order_events(), 2);
    CHECK_EQ(b.events_processed(), 2);

    b.apply(add(7, Side::Bid, P(1.00), 5));
    b.apply(add(7, Side::Bid, P(1.00), 5));  // duplicate reference
    CHECK_EQ(b.unknown_order_events(), 3);
    CHECK_EQ(b.bid_qty_at(P(1.00)), 5);

    // Zero-quantity add is dropped without being counted as an anomaly.
    b.apply(add(8, Side::Bid, P(1.00), 0));
    CHECK_EQ(b.unknown_order_events(), 3);
    CHECK_EQ(b.open_order_count(), 1);

    // A replace onto an already-live reference is an anomaly; the original is
    // still removed, because that is what the wire said happened.
    b.apply(add(9, Side::Bid, P(1.00), 5));
    b.apply(replace(9, 7, P(1.00), 5));
    CHECK_EQ(b.unknown_order_events(), 4);
    CHECK(!b.order_qty(9).has_value());

    // Non-book messages do not touch any counter.
    MarketEvent trade;
    trade.type = EventType::Trade;
    trade.quantity = 100;
    const std::uint64_t before = b.events_processed();
    b.apply(trade);
    CHECK_EQ(b.events_processed(), before);
}

void test_clear_wipes_and_reuses() {
    LowLatencyOrderBook b;
    b.apply(add(1, Side::Bid, P(9.99), 10));
    b.apply(add(2, Side::Ask, P(10.01), 20));
    b.apply(add(3, Side::Bid, P(9.98), 30));
    CHECK_EQ(b.open_order_count(), 3);

    MarketEvent clear;
    clear.type = EventType::Clear;
    b.apply(clear);

    CHECK_EQ(b.clears_applied(), 1);
    CHECK_EQ(b.open_order_count(), 0);
    CHECK_EQ(b.bid_level_count(), 0);
    CHECK_EQ(b.ask_level_count(), 0);
    CHECK_EQ(b.orders_in_use(), 0);
    CHECK_EQ(b.levels_in_use(), 0);
    CHECK(!b.best().has_bid && !b.best().has_ask);
    CHECK_EQ(b.unknown_order_events(), 0);

    b.apply(add(4, Side::Bid, P(9.95), 5));
    CHECK_EQ(b.open_order_count(), 1);
    CHECK_EQ(b.best().bid_price, P(9.95));
}

// The tick grid cannot address every price a real feed carries. These prices
// come straight out of the validation session.
void test_offgrid_and_far_prices() {
    LowLatencyOrderBook b;
    const Price sub_penny = 100'000;              // $0.0001
    const Price off_grid = P(0.3120);             // on the 1/100c grid, not the penny grid
    const Price far = 199'999LL * PRICE_SCALE;    // $199,999.00, past the window
    const Price normal = P(190.00);

    b.apply(add(1, Side::Bid, normal, 100));
    b.apply(add(2, Side::Bid, sub_penny, 200));
    b.apply(add(3, Side::Bid, off_grid, 300));
    b.apply(add(4, Side::Bid, far, 400));

    CHECK(!b.degraded());
    CHECK_EQ(b.metrics().capacity_rejections, 0);
    CHECK(b.metrics().peak_offgrid >= 2);  // sub-penny and far are both off grid
    CHECK_EQ(b.bid_level_count(), 4);

    // The far price is the best bid and must win against the grid.
    CHECK_EQ(b.best().bid_price, far);

    // Depth must merge the grid and the overflow in strict price order.
    const auto d = b.depth(Side::Bid, 10);
    CHECK_EQ(d.size(), 4);
    CHECK_EQ(d[0].price, far);
    CHECK_EQ(d[1].price, normal);
    CHECK_EQ(d[2].price, off_grid);
    CHECK_EQ(d[3].price, sub_penny);
    CHECK_EQ(d[0].qty, 400);
    CHECK_EQ(d[3].qty, 200);

    // And the same on the ask side, where "best" is the lowest.
    LowLatencyOrderBook a;
    a.apply(add(1, Side::Ask, normal, 100));
    a.apply(add(2, Side::Ask, sub_penny, 200));
    a.apply(add(3, Side::Ask, far, 400));
    CHECK_EQ(a.best().ask_price, sub_penny);
    const auto ad = a.depth(Side::Ask, 10);
    CHECK_EQ(ad[0].price, sub_penny);
    CHECK_EQ(ad[1].price, normal);
    CHECK_EQ(ad[2].price, far);

    // Off-grid levels are released like any other.
    b.apply(del(4));
    CHECK_EQ(b.best().bid_price, normal);
    b.apply(del(2));
    b.apply(del(3));
    CHECK_EQ(b.bid_level_count(), 1);
    CHECK_EQ(b.metrics().capacity_rejections, 0);
}

// Deep books must still find the best price in constant work, and the bitmap
// walk must be exact across level-0/1/2 word boundaries.
void test_many_levels_and_bitmap_boundaries() {
    LowLatencyOrderBook b;
    // Spread across enough ticks to cross several bitmap words in both
    // directions (64 ticks per word, 4096 per level-1 word).
    for (int i = 0; i < 5000; ++i) {
        b.apply(add(static_cast<OrderId>(i + 1), Side::Bid,
                    P(1.00) + static_cast<Price>(i) * 10'000'000LL, 10));
    }
    CHECK_EQ(b.bid_level_count(), 5000);
    CHECK_EQ(b.best().bid_price, P(1.00) + 4999LL * 10'000'000LL);

    const auto d = b.depth(Side::Bid, 5000);
    CHECK_EQ(d.size(), 5000);
    for (std::size_t i = 1; i < d.size(); ++i) {
        CHECK(d[i].price < d[i - 1].price);  // strictly descending
    }

    // Remove the top 4000 and confirm best tracks down correctly each time.
    for (int i = 4999; i >= 1000; --i) {
        b.apply(del(static_cast<OrderId>(i + 1)));
        CHECK_EQ(b.best().bid_price, P(1.00) + static_cast<Price>(i - 1) * 10'000'000LL);
    }
    CHECK_EQ(b.bid_level_count(), 1000);
}

// Capacity is finite and must fail loudly rather than corrupting state.
void test_capacity_exhaustion_fails_safe() {
    BookConfig cfg;
    cfg.max_orders = 64;
    cfg.index_capacity = 256;
    cfg.max_levels = 8;
    LowLatencyOrderBook b(cfg);

    for (OrderId i = 1; i <= 64; ++i) {
        b.apply(add(i, Side::Bid, P(10.00), 1));
    }
    CHECK_EQ(b.open_order_count(), 64);
    CHECK(!b.degraded());

    b.apply(add(65, Side::Bid, P(10.00), 1));
    CHECK(b.degraded());
    CHECK_EQ(b.metrics().capacity_rejections, 1);
    // The rejected order must not be half-inserted.
    CHECK_EQ(b.open_order_count(), 64);
    CHECK(!b.order_qty(65).has_value());
    CHECK_EQ(b.bid_qty_at(P(10.00)), 64);

    // Freeing space lets it work again; the degraded flag stays set because
    // events were lost and the book can no longer claim to be complete.
    b.apply(del(1));
    b.apply(add(66, Side::Bid, P(10.00), 5));
    CHECK_EQ(*b.order_qty(66), 5);
    CHECK(b.degraded());
}

void test_level_capacity_exhaustion() {
    BookConfig cfg;
    cfg.max_orders = 1024;
    cfg.index_capacity = 4096;
    cfg.max_levels = 4;
    LowLatencyOrderBook b(cfg);
    for (int i = 0; i < 4; ++i) {
        b.apply(add(static_cast<OrderId>(i + 1), Side::Bid,
                    P(10.00) + static_cast<Price>(i) * 10'000'000LL, 1));
    }
    CHECK(!b.degraded());
    b.apply(add(99, Side::Bid, P(20.00), 1));
    CHECK(b.degraded());
    CHECK_EQ(b.bid_level_count(), 4);
    CHECK(!b.order_qty(99).has_value());
}

void test_reset_restores_a_clean_book() {
    LowLatencyOrderBook b;
    b.apply(add(1, Side::Bid, P(10.00), 100));
    BookConfig cfg;
    cfg.max_orders = 1024;
    cfg.index_capacity = 4096;
    b.reset(cfg);
    CHECK_EQ(b.open_order_count(), 0);
    CHECK_EQ(b.events_processed(), 0);
    CHECK(!b.degraded());
    CHECK_EQ(b.bid_level_count(), 0);
    b.apply(add(1, Side::Bid, P(10.00), 100));
    CHECK_EQ(b.bid_qty_at(P(10.00)), 100);
}

}  // namespace

int main() {
    test_best_and_depth();
    test_fifo_scenario();
    test_partial_cancel_keeps_priority_and_full_removes();
    test_cancel_in_middle_of_queue();
    test_modify_semantics();
    test_execute_semantics();
    test_anomalies_are_tolerated_and_counted();
    test_clear_wipes_and_reuses();
    test_offgrid_and_far_prices();
    test_many_levels_and_bitmap_boundaries();
    test_capacity_exhaustion_fails_safe();
    test_level_capacity_exhaustion();
    test_reset_restores_a_clean_book();
    itch_test::pass("low latency book");
    return 0;
}
