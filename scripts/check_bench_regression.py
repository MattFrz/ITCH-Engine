"""Run the pipeline benchmark and fail on a regression.

Performance-regression protection that is safe to run in CI. Shared runners
have unpredictable and often very slow CPUs, so the thresholds are split into
two kinds:

  HARD  properties of the code, not of the machine. Zero allocations per
        message is a hard invariant: if it breaks, something on the hot path
        started calling the allocator and no amount of CI noise explains it.

  SOFT  timings. These are floors low enough that a shared runner passes them
        comfortably, so they catch a change that makes the pipeline
        *categorically* slower (an accidental O(n), a copy per message, a
        rehash) rather than a few percent of drift. A real number for a real
        machine comes from running the benchmark on one, pinned, and comparing
        against docs/low_latency_architecture.md.

Usage:
    python scripts/check_bench_regression.py --bench build/cpp/bench_pipeline
    python scripts/check_bench_regression.py --bench ... --baseline out.json
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

# SOFT floors. Deliberately generous: about 5x below what the reference machine
# measures, which a slow shared runner still clears.
DEFAULT_LIMITS = {
    "end_to_end_ns": 250.0,  # per message, complete pipeline
    "book_ns": 200.0,        # per message, book apply
    "parse_ns": 100.0,       # per message, ITCH decode
    "mold_ns": 60.0,         # per message, MoldUDP64
    "p99_ns": 2000.0,        # per message, parse + book
    "p999_ns": 20000.0,
}

# HARD invariant.
MAX_ALLOCS_PER_MSG = 0.0


def run_bench(bench: str, events: int, cpu: int | None) -> dict:
    # Resolve explicitly: a relative path with forward slashes does not
    # survive CreateProcess on Windows.
    exe = Path(bench)
    if not exe.exists() and exe.with_suffix(".exe").exists():
        exe = exe.with_suffix(".exe")
    if not exe.exists():
        raise SystemExit(f"benchmark not found: {bench}")
    cmd = [str(exe.resolve()), "--events", str(events), "--repeats", "2", "--json"]
    if cpu is not None:
        cmd += ["--cpu", str(cpu)]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        print(proc.stdout)
        print(proc.stderr, file=sys.stderr)
        raise SystemExit(f"{bench} exited {proc.returncode}")
    print(proc.stdout)
    match = re.search(r"^JSON (\{.*\})$", proc.stdout, re.MULTILINE)
    if not match:
        raise SystemExit("benchmark produced no JSON summary line")
    return json.loads(match.group(1))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--bench", required=True, help="path to bench_pipeline")
    ap.add_argument("--events", type=int, default=400_000)
    ap.add_argument("--cpu", type=int, default=None)
    ap.add_argument("--out", default=None, help="write the measured JSON here")
    ap.add_argument(
        "--baseline",
        default=None,
        help="compare against a previous --out file and report the delta "
        "(reported, never failed on - CI machines are not comparable)",
    )
    args = ap.parse_args()

    result = run_bench(args.bench, args.events, args.cpu)

    print("\n--- regression check ---")
    failures = []

    allocs = result.get("allocs_per_msg", 0.0)
    status = "ok" if allocs <= MAX_ALLOCS_PER_MSG else "FAIL"
    print(f"  [HARD] allocations/message  {allocs:.6f}  (limit {MAX_ALLOCS_PER_MSG})  {status}")
    if allocs > MAX_ALLOCS_PER_MSG:
        failures.append(
            f"the hot path allocated {allocs:.6f} times per message; it must never allocate"
        )

    for key, limit in DEFAULT_LIMITS.items():
        value = result.get(key)
        if value is None:
            continue
        ok = value <= limit
        print(f"  [soft] {key:<16} {value:9.2f} ns  (limit {limit:8.1f})  {'ok' if ok else 'FAIL'}")
        if not ok:
            failures.append(f"{key} = {value:.2f} ns exceeds the {limit:.1f} ns floor")

    if args.baseline:
        base_path = Path(args.baseline)
        if base_path.exists():
            base = json.loads(base_path.read_text())
            print("\n  delta vs baseline (informational - different machines are not comparable):")
            for key in DEFAULT_LIMITS:
                if key in base and key in result and base[key]:
                    delta = (result[key] - base[key]) / base[key] * 100.0
                    print(f"    {key:<16} {base[key]:8.2f} -> {result[key]:8.2f} ns  {delta:+6.1f}%")
        else:
            print(f"\n  baseline {base_path} not found; skipping comparison")

    if args.out:
        Path(args.out).parent.mkdir(parents=True, exist_ok=True)
        Path(args.out).write_text(json.dumps(result, indent=2) + "\n")
        print(f"\n  wrote {args.out}")

    if failures:
        print("\nREGRESSION:")
        for f in failures:
            print(f"  - {f}")
        return 1
    print("\nPASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
