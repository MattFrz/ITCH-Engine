"""Phase 5: post-trade viewer - a static-file reader, nothing more.

Reads exactly the three files the backtester wrote to output/ (trades.csv,
metrics.json, snapshots.parquet), plus the validation report if present.
It never imports the engine, never touches the event loop, and holds no
state. Run with:

    streamlit run viewer/app.py
"""

from __future__ import annotations

import html
import sys
from pathlib import Path

import streamlit as st

REPO_ROOT = Path(__file__).resolve().parents[1]
VALIDATION_TXT = REPO_ROOT / "validation" / "results" / "validation_run.txt"
sys.path.insert(0, str(REPO_ROOT / "viewer"))

import derive  # noqa: E402
from charts import (  # noqa: E402
    equity_curve,
    fill_rate_by_queue,
    latency_histogram,
    markout,
    queue_explorer,
    signal_decay,
    top_of_book,
    trade_context,
)

OUTPUT_DIR = derive.OUTPUT_DIR

# Shown to the viewer instead of the absolute path. On a deployed instance the
# absolute form is a server filesystem path (/mount/src/...), which tells a
# visitor about the host and nothing useful about the data.
try:
    DATA_DIR_LABEL = f"{OUTPUT_DIR.relative_to(REPO_ROOT).as_posix()}/"
except ValueError:
    DATA_DIR_LABEL = OUTPUT_DIR.name + "/"

st.set_page_config(
    page_title="ITCH-Engine | post-trade",
    page_icon=":chart_with_downwards_trend:",
    layout="wide",
)

# --------------------------------------------------------------------- css
# Palette is kept in lockstep with viewer/charts/_style.py and
# .streamlit/config.toml so the chrome and the figures read as one system.
#
# Surfaces use a nested enclosure - an outer shell carrying a hairline and
# a large radius, an inner core with its own inset top highlight and a
# concentric smaller radius - rather than the flat 1px grey box. Shadows are
# tinted toward the background hue instead of black, and every transition
# runs on one custom easing curve. Deliberately NOT borrowed from the
# marketing-site playbook: landing-page section padding (this is a dense
# analytics tool - its job is letting someone compare numbers without
# scrolling), scroll-entry reveals, and an asymmetric bento layout, which
# fights a table of figures that has to stay aligned to be read.
st.markdown(
    """
    <style>
      @import url('https://fonts.googleapis.com/css2?family=Geist:wght@400;500;600;700&family=Geist+Mono:wght@400;500;600&display=swap');

      :root {
        --bg:      #080b12;
        --panel:   #0f141d;
        --core:    #131924;
        --raised:  #18202c;
        --hair:    rgba(255,255,255,.055);
        --hair-lit:rgba(255,255,255,.10);
        --text:    #e8ebf1;
        --muted:   #808b9c;
        --faint:   #5a6472;
        --accent:  #3fbfd4;
        --warn:    #e2725b;
        --amber:   #d9a441;
        --green:   #5fb98a;
        --sans: Geist, 'Segoe UI', system-ui, -apple-system, sans-serif;
        --mono: 'Geist Mono', 'JetBrains Mono', 'Cascadia Code', Consolas, monospace;
        --ease: cubic-bezier(.32,.72,0,1);
        --lift: 0 1px 2px rgba(6,10,18,.5), 0 12px 32px -16px rgba(6,10,18,.9);
        --inset: inset 0 1px 0 rgba(255,255,255,.045);
      }

      .stApp { background: var(--bg); }
      html, body, [class*="css"] { font-family: var(--sans); }
      .block-container { padding-top: 3.6rem; padding-bottom: 3rem; max-width: 1460px; }
      code, pre { font-family: var(--mono); font-variant-numeric: tabular-nums; }

      /* Film grain on a fixed, non-interactive layer so it never repaints
         with scroll. Breaks the flatness of large dark fields. */
      .stApp::after {
        content: ""; position: fixed; inset: 0; pointer-events: none; z-index: 3;
        opacity: .022; mix-blend-mode: overlay;
        background-image: url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='140' height='140'%3E%3Cfilter id='n'%3E%3CfeTurbulence type='fractalNoise' baseFrequency='.85' numOctaves='3'/%3E%3C/filter%3E%3Crect width='140' height='140' filter='url(%23n)'/%3E%3C/svg%3E");
      }
      /* One restrained ambient wash behind the masthead - not a mesh of
         glowing orbs. */
      .stApp::before {
        content: ""; position: fixed; top: -220px; left: 12%; width: 900px;
        height: 520px; pointer-events: none; z-index: 0; opacity: .5;
        background: radial-gradient(ellipse at center,
                    rgba(63,191,212,.10), rgba(63,191,212,0) 68%);
      }

      /* ---- nested enclosure ---- */
      .ie-shell { background: var(--panel); border: 1px solid var(--hair);
                  border-radius: 18px; padding: 6px; box-shadow: var(--lift); }
      .ie-core  { background: var(--core); border-radius: 12px;
                  box-shadow: var(--inset); padding: 22px 26px; }

      /* ---- masthead ---- */
      .ie-eyebrow { font-family: var(--mono); font-size: 10px; font-weight: 500;
                    letter-spacing: .22em; text-transform: uppercase;
                    color: var(--faint); margin-bottom: 14px; }
      .ie-mast { display: flex; align-items: baseline; gap: 15px;
                 flex-wrap: wrap; margin-bottom: 12px; }
      .ie-mast h1 { font-size: 37px; font-weight: 600; letter-spacing: -.038em;
                    margin: 0; line-height: 1; color: var(--text); }
      .ie-mast .ie-sub { font-family: var(--mono); font-size: 12px;
                         color: var(--muted); }
      .ie-rule { height: 1px; margin: 16px 0 20px;
                 background: linear-gradient(90deg, var(--hair-lit),
                             rgba(255,255,255,0) 62%); }

      /* ---- badges ---- */
      .ie-badge { display: inline-block; padding: 4px 10px; margin: 0 6px 6px 0;
                  border-radius: 999px; font-family: var(--mono); font-size: 10.5px;
                  font-weight: 500; letter-spacing: .08em; text-transform: uppercase;
                  background: var(--panel); border: 1px solid var(--hair);
                  color: var(--muted); box-shadow: var(--inset); }
      .ie-badge.pass   { color: var(--green); border-color: rgba(95,185,138,.28); }
      .ie-badge.warn   { color: var(--amber); border-color: rgba(217,164,65,.28); }
      .ie-badge.accent { color: var(--accent); border-color: rgba(63,191,212,.28); }

      /* ---- hero ---- */
      .ie-hero { display: grid; grid-template-columns: 1fr auto 1fr;
                 align-items: center; gap: 10px; }
      .ie-hero .lab { font-family: var(--mono); font-size: 10.5px;
                      letter-spacing: .16em; text-transform: uppercase;
                      color: var(--muted); margin-bottom: 10px; }
      .ie-hero .val { font-family: var(--mono); font-size: 42px; font-weight: 600;
                      letter-spacing: -.035em; line-height: 1; color: var(--text);
                      font-variant-numeric: tabular-nums; }
      .ie-hero .note { font-size: 12px; color: var(--muted); margin-top: 11px;
                       line-height: 1.5; max-width: 33ch; }
      /* Hue marks *which model* produced a number, on the label and edge
         rule only. Values stay neutral: colouring a positive figure red and
         a negative one cyan reads backwards. Sign colour is spent once, on
         the gap - the number the page exists to show. */
      .ie-hero .naive { border-left: 2px solid var(--warn); padding-left: 18px; }
      .ie-hero .naive .lab { color: var(--warn); }
      .ie-hero .realistic { text-align: right; padding-right: 18px;
                            border-right: 2px solid var(--accent); }
      .ie-hero .realistic .lab { color: var(--accent); }
      .ie-hero .realistic .note { margin-left: auto; }
      .ie-gap { text-align: center; padding: 4px 30px;
                border-left: 1px solid var(--hair);
                border-right: 1px solid var(--hair); }
      .ie-gap .lab { white-space: nowrap; }
      /* Same size as the two figures it sits between. This was smaller on
         the theory that the gap is derived and the model outputs are the
         primary numbers, which had it backwards: the gap is the result the
         project exists to report. Sign colour distinguishes it instead. */
      .ie-gap .val { font-family: var(--mono); font-size: 42px; font-weight: 600;
                     letter-spacing: -.035em; line-height: 1;
                     font-variant-numeric: tabular-nums; }

      /* ---- stat cards ----
         Hand-rolled rather than st.metric: st.metric sizes its value for
         short strings and ellipsises anything longer ("587 / 2,0..."), and
         stamps a direction arrow on any delta string including descriptive
         ones. Owning the markup also drops a dependency on Streamlit's
         internal data-testid names. */
      /* auto-fit rather than a fixed column count, so a row of cards wraps
         on its own instead of being squeezed into unreadable slivers. */
      .ie-cards { display: grid; gap: 14px; margin: 16px 0 6px;
                  grid-template-columns: repeat(auto-fit, minmax(186px, 1fr)); }
      .ie-card { background: var(--panel); border: 1px solid var(--hair);
                 border-radius: 14px; padding: 5px;
                 transition: border-color .45s var(--ease),
                             transform .45s var(--ease); }
      .ie-card:hover { border-color: var(--hair-lit); transform: translateY(-1px); }
      .ie-card > div { background: var(--core); border-radius: 10px;
                       padding: 14px 16px; box-shadow: var(--inset); }
      .ie-card .l { color: var(--muted); font-size: 12.5px; }
      /* Clamped rather than fixed: once the row auto-fits to narrower
         columns, a fixed 26px pushed longer values ("587 / 2,061") past the
         card's padding. nowrap stays so a figure never breaks mid-number. */
      .ie-card .v { font-family: var(--mono); font-variant-numeric: tabular-nums;
                    font-size: clamp(18px, 1.9vw, 26px); font-weight: 500;
                    letter-spacing: -.025em; margin-top: 6px;
                    color: var(--text); white-space: nowrap; }
      .ie-card .d { color: var(--faint); font-family: var(--mono);
                    font-size: 11px; margin-top: 5px; }

      /* ---- charts get the same enclosure ---- */
      [data-testid="stPlotlyChart"], .stPlotlyChart {
          background: var(--panel); border: 1px solid var(--hair);
          border-radius: 16px; padding: 6px; box-shadow: var(--lift);
          margin-bottom: 16px;
          transition: border-color .45s var(--ease);
      }
      [data-testid="stPlotlyChart"]:hover, .stPlotlyChart:hover {
          border-color: var(--hair-lit); }
      [data-testid="stPlotlyChart"] > div, .stPlotlyChart > div {
          background: var(--core); border-radius: 11px; box-shadow: var(--inset);
          overflow: hidden; }

      /* ---- tabs ---- */
      .stTabs [data-baseweb="tab-list"] { gap: 2px; border-bottom: 1px solid var(--hair);
                                          margin-bottom: 20px; }
      .stTabs [data-baseweb="tab"] { font-family: var(--mono); font-size: 11.5px;
                                     letter-spacing: .1em; text-transform: uppercase;
                                     color: var(--faint); padding: 12px 16px;
                                     transition: color .45s var(--ease); }
      .stTabs [data-baseweb="tab"]:hover { color: var(--muted); }
      .stTabs [aria-selected="true"] { color: var(--accent); }

      /* ---- filter bar ---- */
      .ie-filters { font-family: var(--mono); font-size: 10px; font-weight: 500;
                    letter-spacing: .22em; text-transform: uppercase;
                    color: var(--faint); margin: 22px 0 2px; }
      div[data-baseweb="select"] > div, .stMultiSelect div[data-baseweb="select"] > div {
          background: var(--core) !important; border-color: var(--hair) !important;
          border-radius: 9px !important; font-size: 13px; }

      /* ---- empty state ---- */
      .ie-empty h2 { font-size: 21px; font-weight: 600; letter-spacing: -.025em;
                     margin: 0 0 10px; color: var(--text); }
      .ie-empty p  { color: var(--muted); font-size: 13.5px; margin: 0 0 4px;
                     max-width: 68ch; line-height: 1.65; }
      .ie-step { font-family: var(--mono); font-size: 12.5px; color: var(--muted);
                 padding: 9px 0; border-top: 1px solid var(--hair); }
      .ie-step b { color: var(--text); font-weight: 500; }
      .ie-step .n { color: var(--accent); margin-right: 12px; }

      /* ---- drilldown ---- */
      .ie-dd { display: grid; grid-template-columns: repeat(auto-fit, minmax(120px, 1fr));
               gap: 22px; }
      .ie-dd .k { font-family: var(--mono); font-size: 10px; letter-spacing: .16em;
                  text-transform: uppercase; color: var(--faint); margin-bottom: 6px; }
      .ie-dd .v { font-family: var(--mono); font-size: 17px; color: var(--text);
                  font-variant-numeric: tabular-nums; }

      /* ---- footer ---- */
      .ie-foot { border-top: 1px solid var(--hair); margin-top: 40px;
                 padding-top: 18px; display: flex; justify-content: space-between;
                 gap: 18px; flex-wrap: wrap; }
      .ie-foot .src { font-family: var(--mono); font-size: 10.5px;
                      color: var(--faint); line-height: 1.85; }
      .ie-foot .cr  { font-family: var(--mono); font-size: 10.5px;
                      color: var(--faint); text-align: right; line-height: 1.85;
                      white-space: nowrap; }
      .ie-foot a { color: var(--muted); text-decoration: none;
                   border-bottom: 1px solid var(--hair);
                   transition: color .45s var(--ease); }
      .ie-foot a:hover { color: var(--accent); }

      /* ---- keyboard focus ----
         Visible focus is an accessibility requirement, not a style choice.
         :focus-visible so it appears for keyboard users without ringing
         every mouse click. */
      a:focus-visible, button:focus-visible, [role="tab"]:focus-visible,
      div[data-baseweb="select"]:focus-within {
          outline: 2px solid var(--accent); outline-offset: 2px;
          border-radius: 6px; }

      /* ---- narrow viewports ----
         The hero is the page's headline and its third column carries the
         realistic PnL - the number the whole project exists to show. At a
         fixed 1fr/auto/1fr it was pushed off-screen entirely below ~860px,
         so it collapses to a stack and the dividing rules turn horizontal. */
      @media (max-width: 860px) {
        .block-container { padding-left: 1rem; padding-right: 1rem; }
        .ie-mast h1 { font-size: 28px; }
        .ie-hero { grid-template-columns: 1fr; gap: 22px; }
        .ie-hero .val { font-size: 34px; }
        .ie-hero .note { max-width: none; }
        .ie-hero .naive { padding-left: 14px; }
        .ie-hero .realistic { text-align: left; padding-right: 0;
                              padding-left: 14px; border-right: none;
                              border-left: 2px solid var(--accent); }
        .ie-hero .realistic .note { margin-left: 0; }
        .ie-gap { text-align: left; padding: 18px 0;
                  border-left: none; border-right: none;
                  border-top: 1px solid var(--hair);
                  border-bottom: 1px solid var(--hair); }
        .ie-core { padding: 18px 18px; }
      }
      @media (max-width: 860px) {
        /* Plotly's modebar is pinned top-right and collides with the chart
           title once the panel is narrow. Zoom/pan is a desktop affordance
           anyway - touch users pinch. */
        .modebar { display: none !important; }
      }
      @media (max-width: 560px) {
        .ie-mast h1 { font-size: 24px; }
        .ie-foot { flex-direction: column; }
        .ie-foot .cr { text-align: left; white-space: normal; }
      }
    </style>
    """,
    unsafe_allow_html=True,
)

COPYRIGHT_HOLDER = "MattFrz"
COPYRIGHT_YEAR = "2026"
REPO_URL = "https://github.com/MattFrz/ITCH-Engine"


# ------------------------------------------------------------------ helpers
def _esc(v: object) -> str:
    """Escapes a value before it goes into raw HTML.

    The panels below render with unsafe_allow_html, and some of what they
    interpolate originates outside this file - `symbol` and `day` come from
    a CLI argument by way of metrics.json. Nobody is attacking their own
    dashboard, but a viewer that renders whatever a data file says is a bad
    habit to ship in a public repo.
    """
    return html.escape(str(v), quote=True)


def _shell(body: str) -> str:
    return f"<div class='ie-shell'><div class='ie-core'>{body}</div></div>"


def _masthead(sub: str) -> None:
    st.markdown(
        "<div class='ie-eyebrow'>post-trade analysis</div>"
        f"<div class='ie-mast'><h1>ITCH-Engine</h1>"
        f"<span class='ie-sub'>{sub}</span></div><div class='ie-rule'></div>",
        unsafe_allow_html=True,
    )


def _usd(v: float) -> str:
    """+$180 / -$64 - sign outside the symbol, as money is actually written.

    A plain f"${v:+,.0f}" yields "$+180", which nobody writes.
    """
    return f"{'-' if v < 0 else '+'}${abs(v):,.0f}"


def _cards(items: list[tuple[str, str, str, str]]) -> None:
    """Renders a row of stat cards: (label, value, sub, tooltip)."""
    cells = "".join(
        f"<div class='ie-card' title='{tip}'><div><div class='l'>{label}</div>"
        f"<div class='v'>{value}</div><div class='d'>{sub or '&nbsp;'}</div>"
        f"</div></div>"
        for label, value, sub, tip in items
    )
    # No inline grid-template-columns: an inline style would beat the
    # responsive rules in the stylesheet and stop the row from wrapping.
    st.markdown(f"<div class='ie-cards'>{cells}</div>", unsafe_allow_html=True)


def _nothing(title: str, body: str) -> None:
    """A composed empty state, rather than a default info box."""
    st.markdown(
        _shell(f"<div class='ie-empty'><h2>{title}</h2><p>{body}</p></div>"),
        unsafe_allow_html=True,
    )


def _footer(note: str) -> None:
    st.markdown(
        "<div class='ie-foot'>"
        f"<div class='src'>{note}</div>"
        f"<div class='cr'>&copy; {COPYRIGHT_YEAR} {COPYRIGHT_HOLDER}. "
        "All rights reserved.<br>"
        f"<a href='{REPO_URL}' target='_blank' rel='noopener'>"
        "github.com/MattFrz/ITCH-Engine</a></div>"
        "</div>",
        unsafe_allow_html=True,
    )


# ---------------------------------------------------------------- load files
if not (OUTPUT_DIR / "metrics.json").exists():
    # A composed "nothing here yet" view rather than a bare error: the viewer
    # is a static-file reader, so the fix is always the same three commands.
    _masthead("no run loaded")
    steps = [
        ("1", "cmake -S . -B build -DCMAKE_BUILD_TYPE=Release "
              "-DPYBIND11_FINDPYTHON=ON &amp;&amp; cmake --build build",
         "build the C++ book and the pybind11 module"),
        ("2", "python validation/validate_book.py",
         "ingests the day on first run, then proves the book is correct"),
        ("3", "python scripts/run_backtest.py",
         "writes the three files this page reads"),
    ]
    rows = "".join(
        f"<div class='ie-step'><span class='n'>{n}</span><b>{cmd}</b><br>"
        f"<span style='margin-left:28px'>{note}</span></div>"
        for n, cmd, note in steps
    )
    st.markdown(
        _shell(
            "<div class='ie-empty'><h2>No backtest output to read</h2>"
            "<p>This page renders <code>trades.csv</code>, "
            "<code>metrics.json</code> and <code>snapshots.parquet</code> from "
            f"<code>{_esc(DATA_DIR_LABEL)}</code>. Nothing has been written there yet. "
            "Run the pipeline in order - each step feeds the next:</p>"
            f"<div style='margin-top:18px'>{rows}</div>"
            "<p style='margin-top:18px'>Without <code>DATABENTO_API_KEY</code> "
            "set, ingestion generates a seeded synthetic day with the same "
            "schema, so every step still runs end to end.</p></div>"
        ),
        unsafe_allow_html=True,
    )
    _footer("Waiting on output/.")
    st.stop()

missing = [f for f in ("trades.csv", "snapshots.parquet")
           if not (OUTPUT_DIR / f).exists()]
if missing:
    # metrics.json alone is not a complete run - fail with the specific file
    # rather than letting pandas raise a bare FileNotFoundError mid-page.
    _masthead("incomplete run")
    _nothing(
        "Run output is incomplete",
        f"<code>metrics.json</code> is present but "
        f"<code>{'</code>, <code>'.join(missing)}</code> "
        f"{'is' if len(missing) == 1 else 'are'} missing from "
        f"<code>{_esc(DATA_DIR_LABEL)}</code>. Re-run "
        "<code>python scripts/run_backtest.py</code> to write the full set.",
    )
    _footer("Incomplete output/.")
    st.stop()

metrics = derive.load_metrics()
summary = metrics["summary"]
params = metrics.get("params", {})
validation_pass = VALIDATION_TXT.exists() and "PASS" in VALIDATION_TXT.read_text()

# ------------------------------------------------------------------- header
_masthead(
    f"{_esc(params.get('symbol', '?'))} &middot; {_esc(params.get('day', '?'))} &middot; "
    f"{summary['events_processed']:,} events"
)
badges = [
    "<span class='ie-badge pass'>book validated &middot; 0 drift</span>"
    if validation_pass else
    "<span class='ie-badge warn'>validation not run</span>",
    "<span class='ie-badge warn'>synthetic data</span>"
    if params.get("synthetic_data") else
    "<span class='ie-badge accent'>XNAS.ITCH MBO</span>",
    f"<span class='ie-badge'>{_esc(params.get('data_latency_ms', '?'))}ms data "
    f"/ {_esc(params.get('order_latency_ms', '?'))}ms order</span>",
]
if derive.IS_DEMO:
    # Say so plainly: this is the committed snapshot, not a run that just
    # happened on this machine.
    badges.append("<span class='ie-badge warn'>bundled demo run</span>")
st.markdown("".join(badges), unsafe_allow_html=True)

# ----------------------------------------------------- hero: the whole point
naive = summary["naive_pnl_usd"]
real = summary["realistic_pnl_usd"]
gap = real - naive
gap_color = "var(--green)" if gap >= 0 else "var(--warn)"
st.markdown(
    _shell(
        "<div class='ie-hero'>"
        "<div class='naive'><div class='lab'>naive backtest</div>"
        f"<div class='val'>{_usd(naive)}</div>"
        "<div class='note'>fills at mid, zero latency, no queue - the "
        "assumption most backtests ship with</div></div>"
        "<div class='ie-gap'><div class='lab'>fill-realism gap</div>"
        f"<div class='val' style='color:{gap_color}'>{_usd(gap)}</div></div>"
        "<div class='realistic'><div class='lab'>realistic engine</div>"
        f"<div class='val'>{_usd(real)}</div>"
        "<div class='note'>same signal and clock, through FIFO queue "
        "position, the two-book latency model and exchange fees</div></div>"
        "</div>"
    ),
    unsafe_allow_html=True,
)

fr = summary.get("passive_fill_rate")
_cards([
    ("Passive fill rate", f"{fr:.0%}" if fr is not None else "n/a",
     "naive assumes 100%",
     "Share of simulated passive orders that received any fill"),
    ("Boundary p50",
     f"{metrics['latency_histogram'].get('p50_us', float('nan')):.1f} us",
     "target &lt; 20 us", "Python/C++ crossing per apply_event call"),
    ("Mean queue ahead",
     f"{summary.get('mean_queue_ahead_at_join', 0):,.0f}", "shares, at join",
     "Average depth resting ahead when a passive order joined its level"),
    ("Exchange fees", _usd(-summary.get("fees_paid_usd", 0.0)),
     "net of maker rebates",
     "Taker fees charged minus maker rebates earned, at Nasdaq list rates"),
    ("Fills / orders",
     f"{summary['fills']:,} / {summary['orders_submitted']:,}", "",
     "Simulated fills against orders submitted by the strategy"),
])

# ------------------------------------------------------------------ filters
with st.spinner("computing mark-outs against the reconstructed book"):
    trades_all = derive.enriched_trades()

st.markdown("<div class='ie-filters'>filters</div>", unsafe_allow_html=True)
f1, f2, f3, f4 = st.columns([1.4, 1, 1, 1.6])
sessions_present = sorted(trades_all["session"].unique()) if not trades_all.empty else []
with f1:
    sel_session = st.multiselect("session", sessions_present, default=[],
                                 placeholder="all sessions")
with f2:
    sel_side = st.multiselect("side", ["buy", "sell"], default=[],
                              placeholder="both")
with f3:
    sel_liq = st.multiselect("liquidity", ["maker", "taker"], default=[],
                             placeholder="both")
with f4:
    if not trades_all.empty:
        # Floor the lower bound and ceil the upper one. Truncating both to
        # whole seconds pushed the last fill (…:53.020) past a …:53 bound
        # and quietly dropped it from an unfiltered view.
        lo_t = trades_all["et"].min().floor("s").time()
        hi_t = trades_all["et"].max().ceil("s").time()
        sel_time = st.slider("time of day (ET)", min_value=lo_t, max_value=hi_t,
                             value=(lo_t, hi_t), step=None, format="HH:mm")
    else:
        sel_time = None

trades = derive.filter_trades(trades_all, sel_session, sel_side, sel_liq, sel_time)
filtered = len(trades) != len(trades_all)

# --------------------------------------------------------------------- tabs
tab_overview, tab_exec, tab_blotter, tab_engine, tab_data = st.tabs(
    ["overview", "execution quality", "blotter", "engine", "raw outputs"]
)

with tab_overview:
    st.plotly_chart(equity_curve.figure(metrics), width='stretch')
    with st.spinner("reading snapshots.parquet"):
        _snaps = derive.load_snapshots()
    st.plotly_chart(top_of_book.figure(_snaps), width="stretch")
    st.caption(
        "Whole-day series straight from metrics.json and snapshots.parquet - "
        "the filters above apply to fill-level analysis, not to these two."
    )

with tab_exec:
    if trades.empty:
        _nothing("No fills in this slice",
                 "The current filter combination selects none of the "
                 f"{len(trades_all):,} simulated fills. Widen the time range "
                 "or clear a filter above.")
    else:
        mk = derive.markout_summary(trades)
        st.plotly_chart(markout.figure(mk), width='stretch')
        st.plotly_chart(queue_explorer.figure(derive.queue_buckets(trades)),
                        width='stretch')
        c1, c2 = st.columns(2)
        with c1:
            st.plotly_chart(fill_rate_by_queue.figure(metrics), width='stretch')
        with c2:
            st.plotly_chart(signal_decay.figure(metrics), width='stretch')
        st.caption(
            "Top two respect the filters; the bottom two are whole-day "
            "aggregates written by the backtester. Mark-outs are signed so "
            "that below zero always means the market moved against the "
            "fill, and are measured against the last two-sided quote at or "
            "before the horizon - so fills inside the final minute are "
            "marked at the close rather than a full horizon out."
        )

with tab_blotter:
    if trades.empty:
        _nothing("No fills in this slice",
                 "The current filter combination selects none of the "
                 f"{len(trades_all):,} simulated fills. Widen the time range "
                 "or clear a filter above.")
    else:
        m1 = trades["markout_1s"].mean()
        m30 = trades["markout_30s"].mean()
        _cards([
            ("Fills shown", f"{len(trades):,}",
             f"of {len(trades_all):,} total" if filtered else "unfiltered", ""),
            ("Shares", f"{trades['qty'].sum():,.0f}", "", ""),
            ("Notional", f"${trades['notional'].sum():,.0f}", "", ""),
            ("Mean 1s mark-out", f"{m1:+.3f}c", "per share", ""),
            ("Mean 30s mark-out", f"{m30:+.3f}c", "per share", ""),
        ])
        st.write("")
        blotter = trades.assign(
            time=trades["et"].dt.strftime("%H:%M:%S.%f").str[:-3],
        )[["time", "side_label", "liquidity", "qty", "price_usd",
           "queue_ahead_at_join", "markout_1s", "markout_30s", "order_ref"]]
        sel = st.dataframe(
            blotter, width='stretch', height=340, hide_index=True,
            on_select="rerun", selection_mode="single-row", key="blotter",
            column_config={
                "time": st.column_config.TextColumn("time (ET)", width="small"),
                "side_label": st.column_config.TextColumn("side", width="small"),
                "liquidity": st.column_config.TextColumn("liq", width="small"),
                "qty": st.column_config.NumberColumn("qty", format="%d"),
                "price_usd": st.column_config.NumberColumn("price", format="$%.2f"),
                "queue_ahead_at_join": st.column_config.NumberColumn(
                    "queue ahead", format="%d"),
                "markout_1s": st.column_config.NumberColumn("1s c/sh", format="%+.3f"),
                "markout_30s": st.column_config.NumberColumn("30s c/sh", format="%+.3f"),
                "order_ref": st.column_config.NumberColumn("ref", format="%d"),
            },
        )
        rows = sel.selection.rows if sel and sel.selection else []
        if rows:
            tr = trades.iloc[rows[0]]
            fields = [
                ("time (ET)", tr["et"].strftime("%H:%M:%S.%f")[:-3]),
                ("side", _esc(tr["side_label"])),
                ("liquidity", _esc(tr["liquidity"])),
                ("qty", f"{int(tr['qty']):,}"),
                ("price", f"${tr['price_usd']:,.2f}"),
                ("queue ahead", f"{int(tr['queue_ahead_at_join']):,}"),
                ("1s mark-out", f"{tr['markout_1s']:+.3f}c"),
                ("30s mark-out", f"{tr['markout_30s']:+.3f}c"),
                ("order ref", f"{int(tr['order_ref'])}"),
            ]
            st.markdown(
                _shell(
                    "<div class='ie-dd'>" + "".join(
                        f"<div><div class='k'>{k}</div><div class='v'>{v}</div></div>"
                        for k, v in fields
                    ) + "</div>"
                ),
                unsafe_allow_html=True,
            )
            st.plotly_chart(
                trade_context.figure(derive.best_quotes(), tr), width='stretch')
        else:
            st.caption("Select a row to see the book around that fill.")

with tab_engine:
    st.plotly_chart(latency_histogram.figure(metrics), width='stretch')
    eps = summary.get("events_per_second")
    _cards([
        ("Events processed", f"{summary['events_processed']:,}", "",
         "MBO messages replayed through the C++ book"),
        ("Event-loop throughput", f"{eps:,.0f} ev/s" if eps else "n/a",
         "full loop, incl. Python bookkeeping",
         "The C++ book alone replays at >1M events/s (see profiling/)"),
        ("Unknown-order events", f"{summary['unknown_order_events']:,}",
         "counted, never guessed at",
         "Feed messages referencing orders resting from before the session"),
    ])
    if validation_pass:
        st.write("")
        st.code(VALIDATION_TXT.read_text().strip(), language=None)

with tab_data:
    st.markdown("**trades.csv** - one row per simulated fill")
    st.dataframe(derive.load_trades(), width='stretch', height=280)
    st.markdown("**snapshots.parquet** - per-second top-5 book state (first 500 rows)")
    st.dataframe(derive.load_snapshots().head(500), width='stretch', height=280)
    st.markdown("**metrics.json** - run summary")
    st.json({"summary": summary, "params": params})

_footer(
    f"Decoupled static-file reader over {_esc(DATA_DIR_LABEL)}.<br>"
    "Never imports the engine or the event loop. Sources: trades.csv "
    "&middot; metrics.json &middot; snapshots.parquet &middot; "
    "validation/results/validation_run.txt"
)
