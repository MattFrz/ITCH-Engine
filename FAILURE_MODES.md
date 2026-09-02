# Failure Modes

The two ways a backtest lies, demonstrated with this engine's own numbers on
**real Nasdaq data**: XNAS.ITCH MBO, AAPL, 2026-07-30, 14,270,119 messages
(49.4% cancels, 49.2% adds, 1.4% executions - nearly every add is later
cancelled, which is why the book's O(1) cancel path matters). Regenerate
with a Databento key configured:

    python scripts/run_backtest.py --symbol AAPL --day 2026-07-30

Without a key the same command runs against a seeded synthetic day, so the
pipeline still works end to end; the magnitudes below are specific to this
real session.

## 1. Ignoring queue position

A naive backtest assumes: *the price touched my limit, therefore I traded.*
What actually happens: a passive order joins the **back** of a FIFO queue at
its price level and fills only after every share ahead of it trades or
cancels. Measured across 1,801 passive orders on the real day:

| Shares ahead at join | Orders | Any fill | Full fill |
|---|---|---|---|
| 0-100 | 657 | 38.8% | 14.6% |
| 100-250 | 497 | 54.5% | 22.9% |
| 250-500 | 247 | 58.3% | 32.0% |
| 500-1k | 174 | 43.7% | 32.8% |
| 1k-2.5k | 166 | 36.7% | 28.3% |
| 2.5k-5k | 40 | 25.0% | 20.0% |
| >5k | 20 | 30.0% | 30.0% |

Overall passive fill rate: **45.7%**, against the naive backtest's implicit
100%.

The table is deliberately not cleanly monotone, and that is the interesting
part. On real data, which orders fill is driven less by where you joined the
queue than by *what the market did next*. The queue reaches you precisely
when the market trades through your level, which is when the price is moving
against you. The fills you miss are disproportionately the good ones: a
queue that never reaches you is often a price that went your way
immediately. That is **adverse selection**, and no fill-rate number alone
captures it.

The result: **naive backtest +$368.50; realistic engine -$2,679.01**, across
3,601 fills and 116,814 shares. The naive backtest did not merely overstate
the edge, it had the sign wrong. The swing decomposes as:

| | |
|---|---|
| naive backtest | **+$368.50** |
| realistic, before fees | -$2,617.78 |
| exchange fees, net of maker rebates | -$61.23 |
| **realistic, all-in** | **-$2,679.01** |

Fees are only $61 of the $3,048 swing. The rest is execution: roughly 2
cents per share of adverse selection, with no single episode dominating. No
constant "slippage haircut" recovers it, because the loss is conditional on
the signal itself - you get filled most reliably exactly when you are most
wrong. Position discipline is structural (the strategy sees its in-flight
orders and never stacks entries or exits), and the output confirms maximum
absolute position held all day was exactly the intended 100 shares.

## 2. Trading on data you haven't received yet

The OFI signal's information coefficient by horizon (from `metrics.json`):

| Horizon | 100ms | 250ms | 500ms | 1s | 2s | 5s | 10s | 20s |
|---|---|---|---|---|---|---|---|---|
| IC | .018 | .022 | .028 | .040 | .066 | .059 | .031 | -.019 |

The signal peaks near 2s and is *negative* by 20s: hold too long and the
edge reverses. Note how little of it exists at 100 ms, which is where a
latency-handicapped strategy is forced to operate.

A vectorized backtest implicitly acts at the moment of the
event with the information of the event, harvesting this curve at its peak
with zero delay. A real strategy sees the book `data_latency` late (2 ms
here), decides on a 100 ms clock, and its order lands `order_latency` later
(3 ms +/- jitter). For a decaying signal the round trip shifts you rightward
along this curve; the move you predicted starts happening *while your order
is in flight*, and you capture only what remains.

Delayed data compounds with failure mode #1: by the time your passive order
arrives, everyone with the same signal and less latency is already queued
ahead of you. Your queue position at join is itself a function of your
latency - which is why this engine models both together (two order books,
one fed late; orders that travel) instead of bolting constants onto a
vectorized backtest.

## The general lesson

Both failure modes push PnL the same direction (up in the backtest, down in
production) and neither shows up as a bug: the naive backtest runs clean,
plots a rising equity curve, and is wrong by more than its entire stated
profit. The defense is an engine whose book state is verified rather than
assumed - **0 drift across 1,000 sampled timestamps (8,000 assertions)
against an independently written reference implementation** - and whose
fill logic is at least pessimistic about queues, latency and fees.

That verification has a boundary worth stating: it proves the two
implementations agree, not that either matches the exchange's own published
book. Comparing against `mbp-1` snapshots is the open item, and it is
tracked in the README's Limitations section rather than glossed over here.
