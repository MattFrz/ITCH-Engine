# Pulling new data from Databento

Exact commands for Windows (PowerShell or Command Prompt), from the repo
root (`ITCH-Engine` folder). Every command here uses `py -3.9` because
that's the Python this project's C++ module is built against.

## One-time setup

1. Get an API key from your Databento dashboard (databento.com, under
   API Keys). It starts with `db-`.
2. Install the client library (only needed once):

```
py -3.9 -m pip install databento
```

3. Set the key for your current terminal session.

PowerShell:

```
$env:DATABENTO_API_KEY = "db-YOUR-KEY-HERE"
```

Command Prompt (cmd.exe):

```
set DATABENTO_API_KEY=db-YOUR-KEY-HERE
```

To set it permanently so every new terminal has it (either shell):

```
setx DATABENTO_API_KEY "db-YOUR-KEY-HERE"
```

Note: `setx` only affects terminals opened *after* you run it. Never put
the key in any file inside the repo.

## Pulling a day

Data is historical and published next-day: you can pull yesterday's
session or older, not today's live session. Use a real trading day
(weekday, not a market holiday).

```
py -3.9 scripts\pull_day.py --symbol AAPL --day 2026-08-03
```

This prints the record count and the exact dollar cost first and asks
`proceed with this pull? [y/N]` before spending any credits. One full day
of MBO for a liquid large-cap is roughly $1 and 10-15 million records.
Add `--yes` to skip the prompt. Add `--force` to re-download a day you
already have.

Already-pulled days are cached in `data\raw\symbol=AAPL\date=2026-08-03\`
and are never re-downloaded (credits are never spent twice) and never
deleted by pulling other days.

## Running the pipeline on the new day

```
py -3.9 validation\validate_book.py --symbol AAPL --day 2026-08-03
```

```
py -3.9 scripts\run_backtest.py --symbol AAPL --day 2026-08-03
```

```
py -3.9 scripts\export_charts.py
```

```
py -3.9 -m streamlit run viewer\app.py
```

The backtest overwrites `output\` (what the dashboard shows), but the
previous run's results are automatically archived to `output_archive\`
first, so nothing is lost. Add `--rth-only` to the backtest command to
restrict trading to regular hours (09:30-16:00 ET).

## Checking disk usage and cleaning up

```
py -3.9 scripts\cleanup_data.py
```

Lists every cached day and its size. Deletes nothing without `--delete`:

```
py -3.9 scripts\cleanup_data.py --keep-latest 1 --delete
```

```
py -3.9 scripts\cleanup_data.py --symbol AAPL --day 2026-07-01 --delete
```

```
py -3.9 scripts\cleanup_data.py --archives --keep-latest 3 --delete
```

## Without an API key

Everything still runs: if `DATABENTO_API_KEY` is not set, the same
commands generate a seeded synthetic day with an identical schema instead
of pulling real data (free, instant). This is intentional and fine - the
whole pipeline works without a paid data source. The dashboard labels
which mode produced the results it is showing, and every cached day
carries a `source.txt` marker recording how it was produced.

## Troubleshooting: "I wanted real data but got synthetic"

This happens when the key was not visible to the script at pull time.
Things to check, in order:

1. **The key was never set in this terminal.** `set` only lasts for the
   current window; `setx` makes it permanent but only affects terminals
   opened afterward. Check what the current terminal sees:

```
echo %DATABENTO_API_KEY%
```

   If that prints `%DATABENTO_API_KEY%` back at you, the variable is not
   set in this terminal.

2. **`set` needs an equals sign, no spaces.** This is a classic cmd trap:
   `set NAME value` does nothing useful - it must be `set NAME=value`:

```
set DATABENTO_API_KEY=db-YOUR-KEY-HERE
```

3. **A synthetic day is already cached for that date.** Pulls are cached,
   so once a synthetic day exists for a date, plain re-pulls will say
   "already cached" and keep the synthetic data. `pull_day.py` detects
   this case (key set, cached day synthetic) and tells you to re-run with
   `--force`, which replaces the synthetic day with the real pull:

```
py -3.9 scripts\pull_day.py --symbol AAPL --day 2026-08-03 --force
```

To see at a glance which cached days are real and which are synthetic:

```
py -3.9 scripts\cleanup_data.py
```

The listing has a `source` column: `databento` means real,
`synthetic` means generated.

How to know a pull is really hitting Databento: it prints the record
count and the exact dollar cost and asks `proceed with this pull? [y/N]`
before downloading. The synthetic path never asks anything.
