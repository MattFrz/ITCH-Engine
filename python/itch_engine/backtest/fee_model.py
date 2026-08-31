"""Exchange fee schedule: maker rebates and taker fees, per share.

Without this the backtest prices every fill at `price * qty` and nothing
else, which quietly flatters the result. This strategy posts to enter and
crosses to exit, so in reality every round trip earns a maker rebate on one
leg and pays a taker fee on the other. Omitting both is not neutral: the
taker fee is larger than the maker rebate on every major US equity venue,
so a fee-free backtest is systematically optimistic about exactly the kind
of high-turnover, small-edge strategy this project is testing.

Defaults are Nasdaq's standard displayed-order schedule for equities priced
at or above $1.00, expressed in dollars per share:

* taker: $0.0030 per share charged for removing displayed liquidity
* maker: $0.0020 per share credited for adding displayed liquidity

These are list rates. Real desks negotiate tiers, and a firm doing serious
volume pays materially less, so treat this as the pessimistic end. The
point is not to nail one venue's pricing to the tenth of a mil, it is that
the sign and rough magnitude of transaction costs belong in the PnL.

Set `FeeModel.free()` to reproduce the older cost-free numbers.
"""

from __future__ import annotations

from dataclasses import dataclass

# Nasdaq standard rates, dollars per share, for stocks >= $1.00.
NASDAQ_TAKER_FEE = 0.0030
NASDAQ_MAKER_REBATE = 0.0020


@dataclass(frozen=True)
class FeeModel:
    """Per-share exchange economics.

    Sign convention: `cost_for` returns a dollar amount to be *subtracted*
    from cash. A maker rebate is therefore a negative cost.
    """

    taker_fee_per_share: float = NASDAQ_TAKER_FEE
    maker_rebate_per_share: float = NASDAQ_MAKER_REBATE

    @classmethod
    def free(cls) -> "FeeModel":
        """No fees and no rebates - the cost-free baseline."""
        return cls(taker_fee_per_share=0.0, maker_rebate_per_share=0.0)

    def cost_for(self, qty: int, aggressive: bool) -> float:
        """Dollar cost of one fill. Negative means a credit."""
        if aggressive:
            return abs(qty) * self.taker_fee_per_share
        return -abs(qty) * self.maker_rebate_per_share

    def describe(self) -> dict:
        return {
            "taker_fee_per_share": self.taker_fee_per_share,
            "maker_rebate_per_share": self.maker_rebate_per_share,
        }
