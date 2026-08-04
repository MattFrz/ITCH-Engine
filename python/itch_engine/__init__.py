"""ITCH-Engine: order book reconstruction and latency-realistic backtesting.

The performance-critical order book lives in the compiled `itch_engine_cpp`
module (built from cpp/ via pybind11). This package is the orchestration
layer: ingestion, the event-driven backtest loop, the test-client signal,
and output reporting.
"""

__version__ = "0.1.0"

# Internal normalized event schema (shared contract with the C++ side).
EVENT_ADD = 0
EVENT_CANCEL = 1
EVENT_MODIFY = 2
EVENT_EXECUTE = 3

SIDE_BID = 0
SIDE_ASK = 1

PRICE_SCALE = 1_000_000_000  # fixed-point: 1e-9 USD per unit (Databento MBO)
