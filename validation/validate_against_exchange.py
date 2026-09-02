"""Validate the reconstructed book against the venue-derived aggregated book.

This is the check the other validation scripts cannot do.

`validate_book.py` proves the C++ book and an independently written Python book
agree exactly, and `validate_low_latency.py` proves the low-latency book agrees
with both. All of that is *agreement*: the implementations were written from
the same reading of the MBO semantics, so a shared misinterpretation would
agree perfectly across every one of them and still be wrong.

Databento's `mbp-10` is the aggregated book Databento reconstructs from the
same XNAS.ITCH feed, independently of this repository. Comparing against it
tests the interpretation, not just the implementation:

    events.parquet ---> itch_engine_cpp book ---\\
                                                 >--- compared level by level
    XNAS.ITCH mbp-10 (Databento's own book) ----/

At every point in the session where the venue book changed, for the top ten
levels on each side:

    price          bid_px_NN / ask_px_NN
    aggregate size bid_sz_NN / ask_sz_NN
    order count    bid_ct_NN / ask_ct_NN

Alignment
---------
On `sequence`, the feed's own message counter, which both schemas carry.

`ts_event` is NOT a safe join key, and assuming it was cost real time here: an
mbp-10 row can carry a *different* timestamp from the MBO record that produced
it, so "apply every MBO event with ts <= T" compares the engine's book against
a venue snapshot taken part-way through that timestamp. 98% of mbp-10
timestamps share a nanosecond with a book-changing MBO record, so the error is
not rare - it showed up as a ~0.2% false mismatch rate that looked like a book
bug and was not.

Aligning on `sequence` needs one more thing: the row's `action`.

A single sequence can produce several mbp-10 rows, and they do not all
describe the same instant. The venue's own action labels say which is which -
and note that mbp-10 contains no 'F' at all, because in the aggregated book an
execution IS a removal:

    action 'T'   a trade report. The book shown is the state BEFORE this
                 sequence's change, since the trade record precedes the
                 removal it reports. Compare against `sequence <  N`.
                 (mbp-10 has exactly 301,947 of these on the reference day -
                 precisely the MBO 'T' count.)

    action 'A'   an add, and
    action 'C'   a removal (a cancel OR an execution). The book shown is the
                 state AFTER this sequence's change. Compare against
                 `sequence <= N`.

Getting this wrong is not a small error. Aligning on `ts_event` scored 99.79%,
"last row per sequence" scored 97.68% and "first row per sequence" scored
66.71% - all of them false mismatches produced by comparing against a snapshot
from the wrong side of a message, and all of them easy to mistake for a book
bug. Only the action-aware rule is actually asking the question the check is
supposed to ask.

Usage:
    python validation/validate_against_exchange.py --symbol AAPL --day 2026-07-30
    python validation/validate_against_exchange.py --rth-only --levels 5
    python validation/validate_against_exchange.py --max-checkpoints 200000
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np
import pandas as pd

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "python"))

import itch_engine_cpp as cpp  # noqa: E402

from itch_engine.ingest.databento_client import data_source, fetch_day  # noqa: E402
from itch_engine.ingest.normalize import load_events, normalize_day  # noqa: E402
from itch_engine.ingest.reference_snapshots import (  # noqa: E402
    DEFAULT_SCHEMA,
    load_reference_snapshots,
)

RESULTS_DIR = REPO_ROOT / "validation" / "results"
UNDEF_PRICE = np.iinfo(np.int64).max
NS_PER_DAY = 86_400_000_000_000

# Field layout of one level in the snapshot_series result.
BID_PX, BID_SZ, BID_CT, ASK_PX, ASK_SZ, ASK_CT = range(6)
FIELD_NAMES = ["bid price", "bid size", "bid count", "ask price", "ask size", "ask count"]


def et_hhmmss(ts_ns: int) -> str:
    return pd.Timestamp(ts_ns, unit="ns", tz="UTC").tz_convert("US/Eastern").strftime("%H:%M:%S")


def reference_matrix(ref: pd.DataFrame, levels: int) -> np.ndarray:
    """[n_rows, levels, 6] from the mbp-10 columns, with absent levels as zeros.

    Databento spells an absent level as price INT64_MAX with size and count 0;
    this engine spells it as a level that is simply not in `depth()`. Both are
    normalized to (0, 0, 0) so the comparison is like for like.
    """
    out = np.zeros((len(ref), levels, 6), dtype=np.int64)
    for lvl in range(levels):
        bpx = ref[f"bid_px_{lvl:02d}"].to_numpy(np.int64)
        apx = ref[f"ask_px_{lvl:02d}"].to_numpy(np.int64)
        bid_absent = bpx == UNDEF_PRICE
        ask_absent = apx == UNDEF_PRICE
        out[:, lvl, BID_PX] = np.where(bid_absent, 0, bpx)
        out[:, lvl, BID_SZ] = np.where(bid_absent, 0, ref[f"bid_sz_{lvl:02d}"].to_numpy(np.int64))
        out[:, lvl, BID_CT] = np.where(bid_absent, 0, ref[f"bid_ct_{lvl:02d}"].to_numpy(np.int64))
        out[:, lvl, ASK_PX] = np.where(ask_absent, 0, apx)
        out[:, lvl, ASK_SZ] = np.where(ask_absent, 0, ref[f"ask_sz_{lvl:02d}"].to_numpy(np.int64))
        out[:, lvl, ASK_CT] = np.where(ask_absent, 0, ref[f"ask_ct_{lvl:02d}"].to_numpy(np.int64))
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--symbol", default="AAPL")
    ap.add_argument("--day", default="2026-07-30")
    ap.add_argument("--schema", default=DEFAULT_SCHEMA)
    ap.add_argument("--levels", type=int, default=10)
    ap.add_argument("--max-checkpoints", type=int, default=0, help="0 = every one")
    ap.add_argument("--chunk", type=int, default=250_000)
    ap.add_argument("--rth-only", action="store_true", help="09:30-16:00 ET only")
    ap.add_argument("--show", type=int, default=8, help="mismatches to print in detail")
    args = ap.parse_args()

    if args.levels < 1 or args.levels > 10:
        raise SystemExit("--levels must be 1..10")

    # --- inputs ------------------------------------------------------------
    raw = fetch_day(args.symbol, args.day)
    source = data_source(args.symbol, args.day)
    if source != "databento":
        raise SystemExit(
            f"the cached day for {args.symbol} {args.day} is '{source}'. This check "
            "compares against the venue's own book, so it is only meaningful on real "
            "data - a synthetic day has no exchange truth to compare to."
        )
    normalize_day(raw, args.symbol, args.day)
    events = load_events(args.symbol, args.day)
    ref = load_reference_snapshots(args.symbol, args.day, args.schema)

    ts = np.ascontiguousarray(events["ts"].to_numpy(np.int64))
    order_id = np.ascontiguousarray(events["order_id"].to_numpy(np.uint64))
    etype = np.ascontiguousarray(events["type"].to_numpy(np.uint8))
    side = np.ascontiguousarray(events["side"].to_numpy(np.uint8))
    price = np.ascontiguousarray(events["price"].to_numpy(np.int64))
    qty = np.ascontiguousarray(events["qty"].to_numpy(np.int64))
    n_events = len(ts)

    if "sequence" not in events.columns:
        raise SystemExit(
            "the normalized day has no `sequence` column, so it cannot be aligned "
            "exactly against the venue book. Older cached pulls predate the column. "
            "Re-pull and re-normalize:\n"
            "  python scripts/pull_day.py --symbol {s} --day {d} --force\n"
            "(`sequence` is kept by the current databento_client; see its docstring "
            "for why ts_event is not a safe join key.)".format(s=args.symbol, d=args.day)
        )

    if "action" not in ref.columns:
        raise SystemExit(
            "the cached reference snapshots have no `action` column, which is "
            "needed to tell a pre-change trade report from a post-change book "
            "row. Rebuild them from the cached download (this spends nothing):\n"
            "  python -c \"import sys; sys.path.insert(0,'python'); "
            "from itch_engine.ingest.reference_snapshots import "
            "fetch_reference_snapshots as f; "
            "f('{s}','{d}', force=True, assume_yes=True)\"".format(
                s=args.symbol, d=args.day
            )
        )

    # Split by action (see the module docstring): trade reports describe the
    # book BEFORE their sequence, book rows describe it AFTER. Keep the first
    # trade report and the last book row at each sequence, which are the two
    # unambiguous instants.
    ref = ref.sort_values(["sequence"], kind="stable")
    is_trade = ref["action"].to_numpy() == "T"
    trades = ref.loc[is_trade].drop_duplicates("sequence", keep="first")
    books = ref.loc[~is_trade].drop_duplicates("sequence", keep="last")
    trades = trades.assign(_before=True)
    books = books.assign(_before=False)
    ref = pd.concat([trades, books], ignore_index=True)

    if args.rth_only:
        day0 = (int(ts.min()) // NS_PER_DAY) * NS_PER_DAY
        # 09:30-16:00 ET on this date, in UTC ns.
        open_ns = pd.Timestamp(f"{args.day} 09:30", tz="US/Eastern").tz_convert("UTC").value
        close_ns = pd.Timestamp(f"{args.day} 16:00", tz="US/Eastern").tz_convert("UTC").value
        del day0
        keep = (ref["ts_event"] >= open_ns) & (ref["ts_event"] <= close_ns)
        ref = ref.loc[keep].reset_index(drop=True)

    if args.max_checkpoints and len(ref) > args.max_checkpoints:
        idx = np.linspace(0, len(ref) - 1, args.max_checkpoints).astype(np.int64)
        ref = ref.iloc[idx].reset_index(drop=True)

    cp_ts = ref["ts_event"].to_numpy(np.int64)
    cp_seq = ref["sequence"].to_numpy(np.uint64)
    n_cp = len(cp_ts)
    if n_cp == 0:
        raise SystemExit("no checkpoints in the selected window")

    # Number of MBO events to have applied at each checkpoint, by sequence.
    # The events are ts-sorted; sequence is monotonic with ts on this feed, but
    # sort explicitly rather than assume it.
    seq = events["sequence"].to_numpy(np.uint64)
    if not np.all(np.diff(seq.astype(np.int64)) >= 0):
        order = np.argsort(seq, kind="stable")
        seq = seq[order]
        ts = np.ascontiguousarray(ts[order])
        order_id = np.ascontiguousarray(order_id[order])
        etype = np.ascontiguousarray(etype[order])
        side = np.ascontiguousarray(side[order])
        price = np.ascontiguousarray(price[order])
        qty = np.ascontiguousarray(qty[order])
    before = ref["_before"].to_numpy()
    cuts = np.where(
        before,
        np.searchsorted(seq, cp_seq, side="left"),   # state before this sequence
        np.searchsorted(seq, cp_seq, side="right"),  # state after this sequence
    ).astype(np.int64)

    # The replay walks forward once, so checkpoints must be ordered by how many
    # events precede them, not by sequence.
    order = np.argsort(cuts, kind="stable")
    cuts = cuts[order]
    ref = ref.iloc[order].reset_index(drop=True)
    cp_ts = cp_ts[order]

    print(f"symbol={args.symbol} day={args.day} schema={args.schema} source={source}")
    print(f"MBO events            : {n_events:,}")
    print(f"reference rows         : {len(ref):,} distinct timestamps"
          f"{' (RTH only)' if args.rth_only else ''}")
    print(f"levels compared        : {args.levels} per side "
          f"(price, aggregate size, order count)")
    print(f"MBO window             : {et_hhmmss(int(ts.min()))} .. {et_hhmmss(int(ts.max()))} ET")
    print(f"reference window       : {et_hhmmss(int(cp_ts.min()))} .. "
          f"{et_hhmmss(int(cp_ts.max()))} ET")
    print()

    theirs_all = reference_matrix(ref, args.levels)  # built AFTER the reorder

    # --- replay and compare, in chunks -------------------------------------
    book = cpp.OrderBook()
    applied = 0
    # [levels, 6] mismatch counts, plus per-level any-field mismatch.
    field_mismatch = np.zeros((args.levels, 6), dtype=np.int64)
    level_mismatch = np.zeros(args.levels, dtype=np.int64)
    row_mismatch = np.zeros(n_cp, dtype=bool)
    examples: list[tuple] = []

    for start in range(0, n_cp, args.chunk):
        stop = min(start + args.chunk, n_cp)
        chunk_cuts = cuts[start:stop]
        end_event = int(chunk_cuts[-1])
        # Feed only the slice this chunk needs; the book keeps its state across
        # calls, so cuts are relative to the slice.
        rel = (chunk_cuts - applied).astype(np.int64)
        ours = book.snapshot_series(
            ts[applied:end_event],
            order_id[applied:end_event],
            etype[applied:end_event],
            side[applied:end_event],
            price[applied:end_event],
            qty[applied:end_event],
            rel,
            args.levels,
        )
        applied = end_event

        theirs = theirs_all[start:stop]
        diff = ours != theirs  # [chunk, levels, 6]
        field_mismatch += diff.sum(axis=0)
        level_mismatch += diff.any(axis=2).sum(axis=0)
        row_mismatch[start:stop] = diff.any(axis=(1, 2))

        if len(examples) < args.show:
            bad = np.flatnonzero(diff.any(axis=(1, 2)))
            for b in bad[: args.show - len(examples)]:
                examples.append((start + int(b), ours[b].copy(), theirs[b].copy()))

    # --- report ------------------------------------------------------------
    total_fields = n_cp * args.levels * 6
    total_field_mismatch = int(field_mismatch.sum())
    matching_rows = int(n_cp - row_mismatch.sum())

    lines: list[str] = []

    def emit(s: str = "") -> None:
        lines.append(s)
        print(s)

    emit(f"symbol={args.symbol} day={args.day} schema={args.schema}")
    emit(f"events replayed: {n_events:,}")
    emit(f"checkpoints: {n_cp:,} (every timestamp at which the venue book changed)")
    emit(f"levels compared: {args.levels} per side, 3 fields each")
    emit(f"field comparisons: {total_fields:,}")
    emit("")
    emit(f"checkpoints fully matching all {args.levels} levels: "
         f"{matching_rows:,} / {n_cp:,} ({100.0 * matching_rows / n_cp:.4f}%)")
    emit(f"field mismatches: {total_field_mismatch:,} / {total_fields:,} "
         f"({100.0 * total_field_mismatch / total_fields:.6f}%)")
    emit("")

    emit("per level (checkpoints where that level differed in any field):")
    for lvl in range(args.levels):
        emit(f"  level {lvl:02d}: {int(level_mismatch[lvl]):>10,} "
             f"({100.0 * level_mismatch[lvl] / n_cp:7.4f}%)")
    emit("")

    emit("per field (across all levels):")
    for f in range(6):
        c = int(field_mismatch[:, f].sum())
        emit(f"  {FIELD_NAMES[f]:<11}: {c:>10,} "
             f"({100.0 * c / (n_cp * args.levels):7.4f}%)")
    emit("")

    # Time profile: does divergence concentrate near the start (a warm-up
    # artefact of starting from an empty book) or persist all day?
    emit("mismatch rate by hour (ET):")
    hours = (
        pd.Series(pd.to_datetime(cp_ts, unit="ns", utc=True))
        .dt.tz_convert("US/Eastern")
        .dt.hour.to_numpy()
    )
    for h in np.unique(hours):
        m = hours == h
        cnt = int(row_mismatch[m].sum())
        tot = int(m.sum())
        emit(f"  {h:02d}:00  {cnt:>9,} / {tot:>9,}  ({100.0 * cnt / tot:7.4f}%)")
    emit("")

    if examples:
        emit(f"first {len(examples)} differing checkpoints:")
        for cp_idx, ours_row, theirs_row in examples:
            emit(f"  checkpoint {cp_idx:,} at {et_hhmmss(int(cp_ts[cp_idx]))} ET "
                 f"(ts={cp_ts[cp_idx]})")
            for lvl in range(args.levels):
                if (ours_row[lvl] != theirs_row[lvl]).any():
                    o, t = ours_row[lvl], theirs_row[lvl]
                    emit(f"    L{lvl:02d} ours   bid {o[BID_PX]/1e9:>10.4f} x {o[BID_SZ]:>7} "
                         f"({o[BID_CT]:>4})  ask {o[ASK_PX]/1e9:>10.4f} x {o[ASK_SZ]:>7} "
                         f"({o[ASK_CT]:>4})")
                    emit(f"    L{lvl:02d} venue  bid {t[BID_PX]/1e9:>10.4f} x {t[BID_SZ]:>7} "
                         f"({t[BID_CT]:>4})  ask {t[ASK_PX]/1e9:>10.4f} x {t[ASK_SZ]:>7} "
                         f"({t[ASK_CT]:>4})")
        emit("")

    ok = total_field_mismatch == 0
    if ok:
        emit("RESULT: PASS - the reconstruction matches the venue's own aggregated "
             "book exactly, at every level and every checkpoint")
    else:
        emit(f"RESULT: MISMATCH - {total_field_mismatch:,} field differences. See the "
             "per-level, per-field and hourly breakdowns above; a divergence "
             "concentrated at the start of the session is a warm-up artefact of "
             "reconstructing from an empty book, while one spread evenly is a "
             "semantic difference worth chasing.")

    RESULTS_DIR.mkdir(parents=True, exist_ok=True)
    (RESULTS_DIR / "exchange_validation.txt").write_text("\n".join(lines) + "\n")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
