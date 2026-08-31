"""Normalization: the contract between the raw feed and the C++ book."""

from __future__ import annotations

import pandas as pd

from itch_engine import (
    EVENT_ADD,
    EVENT_CANCEL,
    EVENT_CLEAR,
    EVENT_EXECUTE,
    EVENT_MODIFY,
)
import pytest

from itch_engine.ingest import normalize as normalize_mod
from itch_engine.ingest.normalize import ACTION_MAP, SIDE_MAP, normalize_day


@pytest.fixture(autouse=True)
def isolated_output(tmp_path, monkeypatch):
    """Keep normalize_day's writes out of the real data/processed tree.

    normalize_day resolves its output from the module-level PROCESSED_DIR,
    so passing a tmp_path input alone is not enough: without this the suite
    silently creates symbol=TEST partitions in the developer's data
    directory.
    """
    monkeypatch.setattr(normalize_mod, "PROCESSED_DIR", tmp_path / "processed")


def raw_frame(rows):
    return pd.DataFrame(
        rows, columns=["ts_event", "order_id", "action", "side", "price", "size"]
    )


def test_action_map_covers_the_book_mutating_actions():
    assert ACTION_MAP["A"] == EVENT_ADD
    assert ACTION_MAP["C"] == EVENT_CANCEL
    assert ACTION_MAP["M"] == EVENT_MODIFY
    assert ACTION_MAP["F"] == EVENT_EXECUTE
    # Clear mutates the book and must not be silently dropped.
    assert ACTION_MAP["R"] == EVENT_CLEAR


def test_non_book_actions_stay_unmapped():
    for a in ("T", "N"):
        assert a not in ACTION_MAP


def test_sides_map_to_the_cpp_enum():
    assert SIDE_MAP == {"B": 0, "A": 1}


def test_clear_survives_the_side_filter(tmp_path):
    # Venues emit Clear with side 'N'. The side check that the per-order
    # actions need must not discard it.
    raw = raw_frame([
        (1, 1, "A", "B", 100, 10),
        (2, 0, "R", "N", 0, 0),
        (3, 2, "A", "A", 101, 10),
    ])
    raw_path = tmp_path / "raw.parquet"
    raw.to_parquet(raw_path, index=False)

    out = normalize_day(raw_path, "TEST", "2026-01-02", force=True)
    events = pd.read_parquet(out)
    assert list(events["type"]) == [EVENT_ADD, EVENT_CLEAR, EVENT_ADD]


def test_unknown_actions_are_dropped(tmp_path):
    raw = raw_frame([
        (1, 1, "A", "B", 100, 10),
        (2, 2, "T", "B", 100, 10),   # trade, no book impact
        (3, 3, "Z", "B", 100, 10),   # not a real action
    ])
    raw_path = tmp_path / "raw.parquet"
    raw.to_parquet(raw_path, index=False)

    events = pd.read_parquet(normalize_day(raw_path, "TEST", "2026-01-03", force=True))
    assert len(events) == 1
    assert events["type"].iloc[0] == EVENT_ADD


def test_output_is_sorted_by_timestamp(tmp_path):
    raw = raw_frame([
        (30, 1, "A", "B", 100, 10),
        (10, 2, "A", "B", 100, 10),
        (20, 3, "A", "B", 100, 10),
    ])
    raw_path = tmp_path / "raw.parquet"
    raw.to_parquet(raw_path, index=False)

    events = pd.read_parquet(normalize_day(raw_path, "TEST", "2026-01-04", force=True))
    assert list(events["ts"]) == [10, 20, 30]


def test_dtypes_match_the_cpp_contract(tmp_path):
    raw = raw_frame([(1, 1, "A", "B", 100, 10)])
    raw_path = tmp_path / "raw.parquet"
    raw.to_parquet(raw_path, index=False)

    events = pd.read_parquet(normalize_day(raw_path, "TEST", "2026-01-05", force=True))
    assert events["ts"].dtype == "int64"
    assert events["order_id"].dtype == "uint64"
    assert events["type"].dtype == "uint8"
    assert events["side"].dtype == "uint8"
    assert events["price"].dtype == "int64"
    assert events["qty"].dtype == "int64"
