# Failure Modes

The two ways a backtest lies, demonstrated with this engine's own numbers on
**real Nasdaq data**: XNAS.ITCH MBO, AAPL, 2026-07-30, 14,469,899 messages
(49.4% cancels, 49.2% adds, 1.4% executions - nearly every add is later
cancelled, which is why the book's O(1) cancel path matters). Regenerate with
`py -3.9 scripts/run_backtest.py --day 2026-07-30` (Windows) or
`python scripts/run_backtest.py --day 2026-07-30` (Linux/macOS).

## 1. Ignoring queue position

A naive backtest assumes: *the price touched my limit, therefore I traded.*
What actually happens: a passive order joins the **back** of a FIFO queue at
its price level and fills only after every share ahead of it trades or
cancels. Measured across 1,783 passive orders on the real day:

| Shares ahead at join | Orders | Any fill | Full fill |
|---|---|---|---|
| 0-100 | 689 | 45% | 16% |
| 100-250 | 444 | 53% | 21% |
| 250-500 | 230 | 57% | 30% |
| 500-1k | 187 | 43% | 30% |
| 1k-2.5k | 164 | 34% | 26% |
| 2.5k-5k | 41 | 24% | 20% |

Overall passive fill rate: **~47%**, vs the naive backtest's implicit 100%.
But the raw fill rate understates the problem, because real fills are
**adversely selected**. Notice the table is not cleanly monotone: on real
data, which orders fill is driven less by where you joined the queue than by
*what the market did next*. The queue reaches you precisely when the market
trades through your level - i.e. when the price is moving against you. The
fills you don't get are the good ones; a queue that never reaches you is a
price that went your way immediately.

The result on this day: **naive backtest +$293; realistic engine -$2,311.**
The naive backtest didn't just overstate the edge, it had the sign wrong.
The realistic loss works out to roughly 2 cents per share across 3,672
fills and 116,884 shares traded, with no single episode dominating (the
worst one-second equity step is under $100) - the steady toll a slow,
naive signal pays for trading against faster flow. No "slippage haircut"
constant recovers this, because the loss is conditional on the signal
itself: you get filled most reliably exactly when you are most wrong.
Position discipline is enforced structurally (the strategy sees its
in-flight orders and never stacks entries or exits); max position held all
day was exactly the intended 100 shares.

## 2. Trading on data you haven't received yet

The OFI signal's information coefficient by horizon on the real day
(from `metrics.json`):

| Horizon | 100ms | 250ms | 500ms | 1s | 2s | 5s | 10s | 20s |
|---|---|---|---|---|---|---|---|---|
| IC | .018 | .020 | .031 | .044 | .067 | .042 | .026 | -.028 |

The signal peaks near 2s and is *negative* by 20s - hold too long and the
edge reverses. A vectorized backtest implicitly acts at the moment of the
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
profit. The only defense is an engine whose book state is *proven* correct -
validation: **0 drift across 1,000 sampled timestamps (8,000 assertions)
against an independent reference implementation, on 14.5M real messages** -
and whose fill logic is at least pessimistic about queues and latency.
