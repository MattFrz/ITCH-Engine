"""Shared pytest setup.

Puts `python/` on the path so the tests import the package the same way the
scripts do, and skips the binding tests cleanly when the C++ module has not
been built yet (a fresh clone has no .pyd).
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "python"))


@pytest.fixture(scope="session")
def cpp():
    """The compiled engine, or a skip if it has not been built."""
    try:
        import itch_engine_cpp
    except ImportError:
        pytest.skip("itch_engine_cpp not built - run cmake --build build")
    return itch_engine_cpp
