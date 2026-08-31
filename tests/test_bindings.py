"""The C++ book as seen through pybind11.

The C++ suite covers the book's internals; this covers the binding surface
the Python layer actually calls, so a signature change cannot pass CI while
silently breaking the backtest.
"""

from __future__ import annotations

import numpy as np

ADD, CANCEL, MODIFY, EXECUTE, CLEAR = 0, 1, 2, 3, 4
BID, ASK = 0, 1


def px(cpp, usd):
    return int(usd * cpp.PRICE_SCALE)


def test_best_quote_and_mid(cpp):
    b = cpp.OrderBook()
    b.apply_event(1, 1, ADD, BID, px(cpp, 10.00), 100)
    b.apply_event(2, 2, ADD, ASK, px(cpp, 10.02), 100)
    q = b.best()
    assert q.has_bid and q.has_ask
    assert q.bid_price == px(cpp, 10.00)
    assert abs(b.mid_price() - 10.01) < 1e-9


def test_mid_is_none_on_a_one_sided_book(cpp):
    b = cpp.OrderBook()
    b.apply_event(1, 1, ADD, BID, px(cpp, 10.00), 100)
    assert b.mid_price() is None


def test_queue_ahead_reflects_fifo_order(cpp):
    b = cpp.OrderBook()
    b.apply_event(1, 1, ADD, BID, px(cpp, 10.00), 100)
    b.apply_event(2, 2, ADD, BID, px(cpp, 10.00), 250)
    assert b.queue_ahead(1) == 0
    assert b.queue_ahead(2) == 100


def test_queue_ahead_is_none_for_a_dead_order(cpp):
    b = cpp.OrderBook()
    assert b.queue_ahead(999) is None


def test_batch_and_scalar_paths_agree(cpp):
    n = 500
    rng = np.random.default_rng(0)
    ts = np.arange(n, dtype=np.int64)
    oid = np.arange(1, n + 1, dtype=np.uint64)
    etype = np.zeros(n, dtype=np.uint8)
    side = rng.integers(0, 2, n).astype(np.uint8)
    price = (px(cpp, 10.0) + rng.integers(-5, 5, n) * 10_000_000).astype(np.int64)
    qty = rng.integers(1, 500, n).astype(np.int64)

    scalar = cpp.OrderBook()
    for i in range(n):
        scalar.apply_event(int(ts[i]), int(oid[i]), int(etype[i]),
                           int(side[i]), int(price[i]), int(qty[i]))
    batch = cpp.OrderBook()
    batch.apply_batch(ts, oid, etype, side, price, qty)

    assert scalar.open_order_count == batch.open_order_count
    assert scalar.best().bid_price == batch.best().bid_price
    assert scalar.best().ask_price == batch.best().ask_price
    assert scalar.events_processed == batch.events_processed


def test_depth_is_ordered_best_first(cpp):
    b = cpp.OrderBook()
    for i, p in enumerate([9.97, 9.99, 9.98]):
        b.apply_event(i, i + 1, ADD, BID, px(cpp, p), 10)
    d = b.depth(BID, 3)
    assert [lv.price for lv in d] == [px(cpp, 9.99), px(cpp, 9.98), px(cpp, 9.97)]


def test_unknown_orders_are_counted_not_fatal(cpp):
    b = cpp.OrderBook()
    b.apply_event(1, 404, CANCEL, BID, px(cpp, 10.0), 10)
    assert b.unknown_order_events == 1
    assert b.open_order_count == 0


def test_clear_wipes_the_book(cpp):
    b = cpp.OrderBook()
    b.apply_event(1, 1, ADD, BID, px(cpp, 10.00), 100)
    b.apply_event(2, 2, ADD, ASK, px(cpp, 10.02), 100)
    b.apply_event(3, 0, CLEAR, BID, 0, 0)
    assert b.open_order_count == 0
    assert b.clears_applied == 1
    assert b.unknown_order_events == 0   # Clear is not an anomaly
