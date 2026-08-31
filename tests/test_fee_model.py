"""Fee model: sign convention is the thing worth pinning down."""

from __future__ import annotations

from itch_engine.backtest.fee_model import (
    NASDAQ_MAKER_REBATE,
    NASDAQ_TAKER_FEE,
    FeeModel,
)


def test_taker_is_a_cost_and_maker_is_a_credit():
    f = FeeModel()
    # Positive = subtracted from cash. Negative = credited.
    assert f.cost_for(100, aggressive=True) > 0
    assert f.cost_for(100, aggressive=False) < 0


def test_rates_match_the_published_schedule():
    f = FeeModel()
    assert f.cost_for(100, aggressive=True) == 100 * NASDAQ_TAKER_FEE
    assert f.cost_for(100, aggressive=False) == -100 * NASDAQ_MAKER_REBATE


def test_taker_fee_exceeds_maker_rebate():
    # The whole reason fees matter to this strategy: it posts to enter and
    # crosses to exit, so a round trip is a net cost, not a net credit.
    f = FeeModel()
    round_trip = f.cost_for(100, aggressive=False) + f.cost_for(100, aggressive=True)
    assert round_trip > 0


def test_quantity_sign_does_not_flip_the_cost():
    f = FeeModel()
    assert f.cost_for(-100, aggressive=True) == f.cost_for(100, aggressive=True)


def test_free_model_is_the_costless_baseline():
    f = FeeModel.free()
    assert f.cost_for(100, aggressive=True) == 0.0
    assert f.cost_for(100, aggressive=False) == 0.0


def test_describe_round_trips_the_rates():
    d = FeeModel().describe()
    assert d["taker_fee_per_share"] == NASDAQ_TAKER_FEE
    assert d["maker_rebate_per_share"] == NASDAQ_MAKER_REBATE
