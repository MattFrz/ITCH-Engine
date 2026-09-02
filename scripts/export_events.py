"""Export a normalized day to the binary event file the C++ tools read.

This is the one place the historical (Python) path hands data to the
low-latency (C++) path, and it is deliberately a file rather than a call:
nothing in the low-latency pipeline links against Python or pybind11.

    data/processed/symbol=X/date=Y/events.parquet
            |  this script
            v
    data/capture/X_Y.evbin          (MarketEvent records, verbatim)
            |  build/cpp/itch_make_capture
            v
    data/capture/X_Y.itchcap        (real ITCH messages in real MoldUDP64
                                     datagrams)
            |  build/cpp/itch_replay  /  bench_pipeline
            v
    MoldUDP64 -> ITCH parser -> MarketEvent -> LowLatencyOrderBook

The .evbin layout mirrors `struct MarketEvent` (cpp/include/itch_engine/
market_event.hpp) byte for byte - 40 bytes, no padding. It is a local
artifact, so it is little-endian host order by design; the format that has to
be portable is the ITCH one the next step produces.

Usage:
    python scripts/export_events.py --symbol AAPL --day 2026-07-01
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

import numpy as np

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "python"))

from itch_engine.ingest.databento_client import fetch_day  # noqa: E402
from itch_engine.ingest.normalize import load_events, normalize_day  # noqa: E402

CAPTURE_DIR = REPO_ROOT / "data" / "capture"

MAGIC = b"ITCHEVB1"
HEADER_LEN = 64
VERSION = 1

# Must stay identical to struct MarketEvent. The itemsize assert below is the
# guard: if the C++ layout ever changes, this fails loudly instead of writing
# a file the tools would misread.
EVENT_DTYPE = np.dtype(
    [
        ("timestamp", "<i8"),
        ("order_id", "<u8"),
        ("new_order_id", "<u8"),
        ("price", "<i8"),
        ("quantity", "<u4"),
        ("stock_locate", "<u2"),
        ("type", "u1"),
        ("side", "u1"),
    ]
)
assert EVENT_DTYPE.itemsize == 40, EVENT_DTYPE.itemsize

NS_PER_DAY = 86_400_000_000_000


def export(symbol: str, day: str, out_path: Path, stock_locate: int = 1) -> dict:
    raw = fetch_day(symbol, day)
    normalize_day(raw, symbol, day)
    events = load_events(symbol, day)

    ts = events["ts"].to_numpy(np.int64)
    if len(ts) == 0:
        raise SystemExit(f"{symbol} {day}: no events")

    # ITCH carries 48-bit nanoseconds since midnight, so the absolute epoch has
    # to travel out of band. Midnight UTC of the first event's day is the
    # reference both the encoder and the parser use.
    session_epoch_ns = int(ts.min() // NS_PER_DAY) * NS_PER_DAY
    span_ns = int(ts.max()) - session_epoch_ns
    if span_ns >= NS_PER_DAY:
        raise SystemExit(
            f"{symbol} {day}: events span {span_ns / NS_PER_DAY:.2f} days; ITCH's "
            "48-bit ns-since-midnight timestamp cannot represent that. Split the "
            "day before exporting."
        )

    qty = events["qty"].to_numpy(np.int64)
    if (qty < 0).any():
        qty = np.clip(qty, 0, None)
    if (qty > np.iinfo(np.uint32).max).any():
        raise SystemExit("quantity exceeds the 32-bit ITCH share field")

    out = np.zeros(len(ts), dtype=EVENT_DTYPE)
    out["timestamp"] = ts
    out["order_id"] = events["order_id"].to_numpy(np.uint64)
    out["new_order_id"] = 0
    out["price"] = events["price"].to_numpy(np.int64)
    out["quantity"] = qty.astype(np.uint32)
    out["stock_locate"] = stock_locate
    out["type"] = events["type"].to_numpy(np.uint8)
    out["side"] = events["side"].to_numpy(np.uint8)

    out_path.parent.mkdir(parents=True, exist_ok=True)
    header = bytearray(HEADER_LEN)
    header[0:8] = MAGIC
    struct.pack_into("<II", header, 8, VERSION, EVENT_DTYPE.itemsize)
    struct.pack_into("<q", header, 16, session_epoch_ns)
    struct.pack_into("<Q", header, 24, len(out))
    sym = symbol.encode("ascii")[:8].ljust(8, b" ")
    header[32:40] = sym
    struct.pack_into("<H", header, 40, stock_locate)

    with open(out_path, "wb") as f:
        f.write(bytes(header))
        out.tofile(f)

    type_counts = {int(k): int(v) for k, v in zip(*np.unique(out["type"], return_counts=True))}
    return {
        "events": int(len(out)),
        "session_epoch_ns": session_epoch_ns,
        "first_ts": int(ts.min()),
        "last_ts": int(ts.max()),
        "bytes": out_path.stat().st_size,
        "type_counts": type_counts,
    }


TYPE_NAMES = {0: "Add", 1: "Cancel", 2: "Modify", 3: "Execute", 4: "Clear"}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--symbol", default="AAPL")
    ap.add_argument("--day", default="2026-07-01")
    ap.add_argument("--out", default=None, help="output .evbin path")
    ap.add_argument("--locate", type=int, default=1, help="ITCH stock locate code")
    args = ap.parse_args()

    out_path = (
        Path(args.out)
        if args.out
        else CAPTURE_DIR / f"{args.symbol}_{args.day}.evbin"
    )
    info = export(args.symbol, args.day, out_path, args.locate)

    print(f"wrote {out_path}")
    print(f"  events           : {info['events']:,}")
    print(f"  bytes            : {info['bytes']:,}")
    print(f"  session epoch ns : {info['session_epoch_ns']}")
    print(f"  first / last ts  : {info['first_ts']} .. {info['last_ts']}")
    for t, n in sorted(info["type_counts"].items()):
        print(f"    {TYPE_NAMES.get(t, t):8s}: {n:,}")
    print()
    print("next:")
    print(
        f"  build/cpp/itch_make_capture --input {out_path} "
        f"--symbol {args.symbol} --locate {args.locate} "
        f"--output {out_path.with_suffix('.itchcap')}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
