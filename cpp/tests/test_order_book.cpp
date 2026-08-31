// Plain-assert test harness: no framework dependency, exits nonzero on the
// first failure with file/line, which is all CTest needs.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "itch_engine/order_book.hpp"

namespace {

int checks_run = 0;

void check(bool cond, const char* expr, const char* file, int line) {
    ++checks_run;
    if (!cond) {
        std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expr);
        std::exit(1);
    }
}

#define CHECK(cond) check((cond), #cond, __FILE__, __LINE__)

using namespace itch;

constexpr Price P(double usd) {
    return static_cast<Price>(usd * static_cast<double>(PRICE_SCALE));
}

Event add(OrderId id, Side side, Price price, Qty qty) {
    return Event{0, id, EventType::Add, side, price, qty};
}
Event cancel(OrderId id, Qty qty = 0) {
    return Event{0, id, EventType::Cancel, Side::Bid, 0, qty};
}
Event modify(OrderId id, Price price, Qty qty) {
    return Event{0, id, EventType::Modify, Side::Bid, price, qty};
}
Event execute(OrderId id, Qty qty) {
    return Event{0, id, EventType::Execute, Side::Bid, 0, qty};
}

void test_add_and_best() {
    OrderBook book;
    book.apply(add(1, Side::Bid, P(100.00), 100));
    book.apply(add(2, Side::Bid, P(100.01), 50));
    book.apply(add(3, Side::Ask, P(100.03), 75));
    book.apply(add(4, Side::Ask, P(100.02), 25));

    BestQuote q = book.best();
    CHECK(q.has_bid && q.has_ask);
    CHECK(q.bid_price == P(100.01));
    CHECK(q.bid_qty == 50);
    CHECK(q.ask_price == P(100.02));
    CHECK(q.ask_qty == 25);
    CHECK(book.open_order_count() == 4);
    CHECK(book.mid_price().has_value());
    // mid = (100.01 + 100.02) / 2
    CHECK(std::abs(*book.mid_price() - 100.015) < 1e-9);
}

void test_full_cancel_removes_level() {
    OrderBook book;
    book.apply(add(1, Side::Bid, P(99.50), 10));
    CHECK(book.bid_level_count() == 1);
    book.apply(cancel(1));
    CHECK(book.bid_level_count() == 0);
    CHECK(book.open_order_count() == 0);
    CHECK(!book.best().has_bid);
}

void test_partial_cancel_keeps_priority() {
    OrderBook book;
    book.apply(add(1, Side::Ask, P(50.00), 100));
    book.apply(add(2, Side::Ask, P(50.00), 200));
    // Partial-cancel order 2 down; it must stay behind order 1.
    book.apply(cancel(2, 150));
    CHECK(book.ask_qty_at(P(50.00)) == 150);
    CHECK(*book.order_qty(2) == 50);
    CHECK(*book.queue_ahead(2) == 100);
    CHECK(*book.queue_ahead(1) == 0);
}

void test_cancel_in_middle_of_queue() {
    OrderBook book;
    for (OrderId id = 1; id <= 5; ++id) {
        book.apply(add(id, Side::Bid, P(10.00), 10 * static_cast<Qty>(id)));
    }
    // Cancel order 3 (middle). Orders 4 and 5 each move up by order 3's qty.
    CHECK(*book.queue_ahead(4) == 10 + 20 + 30);
    book.apply(cancel(3));
    CHECK(*book.queue_ahead(4) == 10 + 20);
    CHECK(*book.queue_ahead(5) == 10 + 20 + 40);
    CHECK(book.bid_qty_at(P(10.00)) == 10 + 20 + 40 + 50);
}

void test_execute_fifo() {
    OrderBook book;
    book.apply(add(1, Side::Bid, P(20.00), 30));
    book.apply(add(2, Side::Bid, P(20.00), 40));
    // Feed reports execution against order 1 (front) for 30, then partial
    // against order 2.
    book.apply(execute(1, 30));
    CHECK(!book.order_qty(1).has_value());
    CHECK(*book.queue_ahead(2) == 0);
    book.apply(execute(2, 15));
    CHECK(*book.order_qty(2) == 25);
    CHECK(book.bid_qty_at(P(20.00)) == 25);
}

void test_modify_semantics() {
    OrderBook book;
    book.apply(add(1, Side::Ask, P(30.00), 100));
    book.apply(add(2, Side::Ask, P(30.00), 100));

    // Size-down at same price: order 1 keeps the front of the queue.
    book.apply(modify(1, P(30.00), 60));
    CHECK(*book.queue_ahead(1) == 0);
    CHECK(*book.order_qty(1) == 60);

    // Size-up at same price: order 1 loses priority, goes behind order 2.
    book.apply(modify(1, P(30.00), 120));
    CHECK(*book.queue_ahead(1) == 100);
    CHECK(*book.queue_ahead(2) == 0);

    // Price change: moves levels, old level survives with order 2 only.
    book.apply(modify(1, P(29.99), 120));
    CHECK(book.ask_qty_at(P(30.00)) == 100);
    CHECK(book.ask_qty_at(P(29.99)) == 120);
    BestQuote q = book.best();
    CHECK(q.ask_price == P(29.99));
}

void test_unknown_orders_tolerated() {
    OrderBook book;
    book.apply(cancel(42));
    book.apply(execute(99, 10));
    CHECK(book.unknown_order_events() == 2);
    CHECK(book.events_processed() == 2);
    // Duplicate add of a live id is also an anomaly, not a crash.
    book.apply(add(7, Side::Bid, P(1.00), 5));
    book.apply(add(7, Side::Bid, P(1.00), 5));
    CHECK(book.unknown_order_events() == 3);
    CHECK(book.bid_qty_at(P(1.00)) == 5);
}

void test_depth() {
    OrderBook book;
    book.apply(add(1, Side::Bid, P(9.99), 10));
    book.apply(add(2, Side::Bid, P(9.98), 20));
    book.apply(add(3, Side::Bid, P(9.97), 30));
    auto d = book.depth(Side::Bid, 2);
    CHECK(d.size() == 2);
    CHECK(d[0].price == P(9.99) && d[0].qty == 10 && d[0].order_count == 1);
    CHECK(d[1].price == P(9.98) && d[1].qty == 20);
}

// A venue Clear wipes every resting order on both sides. Without this the
// book carries pre-halt orders for the rest of the session.
void test_clear_wipes_book() {
    OrderBook book;
    book.apply(add(1, Side::Bid, P(9.99), 10));
    book.apply(add(2, Side::Ask, P(10.01), 20));
    book.apply(add(3, Side::Bid, P(9.98), 30));
    CHECK(book.open_order_count() == 3);

    Event ev{};
    ev.type = EventType::Clear;
    book.apply(ev);

    CHECK(book.clears_applied() == 1);
    CHECK(book.open_order_count() == 0);
    CHECK(book.bid_level_count() == 0);
    CHECK(book.ask_level_count() == 0);
    CHECK(!book.best().has_bid && !book.best().has_ask);
    // A Clear is not an unknown-order anomaly.
    CHECK(book.unknown_order_events() == 0);

    // The book is reusable afterwards: post-resume orders rest normally.
    book.apply(add(4, Side::Bid, P(9.95), 5));
    CHECK(book.open_order_count() == 1);
    CHECK(book.best().bid_price == P(9.95));
}

}  // namespace

int main() {
    test_add_and_best();
    test_full_cancel_removes_level();
    test_partial_cancel_keeps_priority();
    test_cancel_in_middle_of_queue();
    test_execute_fifo();
    test_modify_semantics();
    test_unknown_orders_tolerated();
    test_depth();
    test_clear_wipes_book();
    std::printf("PASS: all order book tests (%d checks)\n", checks_run);
    return 0;
}
