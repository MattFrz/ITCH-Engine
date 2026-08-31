"""Fill model: the queue-position logic the project's thesis rests on."""

from __future__ import annotations

from itch_engine.backtest.fill_model import (
    SIDE_ASK,
    SIDE_BID,
    FillModel,
    SimOrder,
)


class FakeBook:
    """Minimal stand-in exposing only what the fill model asks of a book."""

    def __init__(self, bid_qty=0, ask_qty=0, depth=None):
        self._bid_qty = bid_qty
        self._ask_qty = ask_qty
        self._depth = depth or {}

    def bid_qty_at(self, price):
        return self._bid_qty

    def ask_qty_at(self, price):
        return self._ask_qty

    def depth(self, side, n):
        return self._depth.get(side, [])[:n]


class Level:
    def __init__(self, price, qty):
        self.price = price
        self.qty = qty
        self.order_count = 1


def passive(qty=100, side=SIDE_BID, price=1000):
    return SimOrder(ref=1, side=side, price=price, qty=qty,
                    decision_ts=0, arrival_ts=0)


def test_joining_records_the_full_level_as_queue_ahead():
    fm, o = FillModel(), passive()
    fm.activate(o, FakeBook(bid_qty=500))
    assert o.queue_ahead == 500
    assert o.queue_ahead_at_join == 500
    assert o.status == "live"


def test_execution_smaller_than_the_queue_does_not_fill_us():
    fm, o = FillModel(), passive()
    fm.activate(o, FakeBook(bid_qty=500))
    fm.on_execute(o, 1, SIDE_BID, 1000, 300)
    assert o.queue_ahead == 200
    assert fm.fills == []


def test_execution_spilling_past_the_queue_fills_us():
    fm, o = FillModel(), passive(qty=100)
    fm.activate(o, FakeBook(bid_qty=500))
    fm.on_execute(o, 1, SIDE_BID, 1000, 560)   # 500 burns queue, 60 spills
    assert o.queue_ahead == 0
    assert len(fm.fills) == 1
    assert fm.fills[0].qty == 60
    assert fm.fills[0].aggressive is False
    assert fm.fills[0].queue_ahead_at_join == 500


def test_fill_never_exceeds_order_size():
    fm, o = FillModel(), passive(qty=100)
    fm.activate(o, FakeBook(bid_qty=0))
    fm.on_execute(o, 1, SIDE_BID, 1000, 10_000)
    assert sum(f.qty for f in fm.fills) == 100
    assert o.status == "filled"


def test_events_at_other_levels_or_sides_are_ignored():
    fm, o = FillModel(), passive()
    fm.activate(o, FakeBook(bid_qty=500))
    fm.on_execute(o, 1, SIDE_ASK, 1000, 900)    # wrong side
    fm.on_execute(o, 1, SIDE_BID, 1001, 900)    # wrong price
    assert o.queue_ahead == 500
    assert fm.fills == []


def test_cancel_uses_the_expected_fraction_ahead():
    fm, o = FillModel(), passive()
    fm.activate(o, FakeBook(bid_qty=500))
    # We are behind 500 of a 1000-share level, so half the cancelled volume
    # is expected to have been ahead of us.
    fm.on_cancel(o, SIDE_BID, 1000, ev_qty=200, level_qty_before=1000)
    assert o.queue_ahead == 400


def test_queue_ahead_never_goes_negative():
    fm, o = FillModel(), passive()
    fm.activate(o, FakeBook(bid_qty=100))
    fm.on_cancel(o, SIDE_BID, 1000, ev_qty=10_000, level_qty_before=100)
    assert o.queue_ahead == 0


def test_marketable_order_walks_real_depth():
    fm = FillModel()
    o = SimOrder(ref=1, side=SIDE_BID, price=1002, qty=250,
                 decision_ts=0, arrival_ts=0, aggressive=True)
    book = FakeBook(depth={SIDE_ASK: [Level(1000, 100), Level(1001, 100),
                                      Level(1002, 100)]})
    fm.activate(o, book)
    assert [f.price for f in fm.fills] == [1000, 1001, 1002]
    assert sum(f.qty for f in fm.fills) == 250
    assert all(f.aggressive for f in fm.fills)
    assert o.status == "filled"


def test_marketable_order_stops_at_its_limit_price():
    fm = FillModel()
    o = SimOrder(ref=1, side=SIDE_BID, price=1000, qty=250,
                 decision_ts=0, arrival_ts=0, aggressive=True)
    book = FakeBook(depth={SIDE_ASK: [Level(1000, 100), Level(1001, 500)]})
    fm.activate(o, book)
    assert sum(f.qty for f in fm.fills) == 100     # will not pay 1001
    assert o.status == "cancelled"                  # remainder does not rest


def test_cancelled_order_stops_accruing_fills():
    fm, o = FillModel(), passive()
    fm.activate(o, FakeBook(bid_qty=0))
    fm.cancel(o)
    fm.on_execute(o, 1, SIDE_BID, 1000, 500)
    assert fm.fills == []
    assert o.status == "cancelled"
