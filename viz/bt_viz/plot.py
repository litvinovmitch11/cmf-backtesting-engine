"""Load the engine's CSVs and render the backtest panels.

The engine writes a per-row time series (see ``bt::TimeSeriesRecorder``) with
columns::

    ts,mid,inventory,equity,realized_cash,turnover,fees,fills,bucket_qty,bucket_fills

``ts`` is microseconds since the Unix epoch. ``mid`` is the marked mid price,
``equity`` is mark-to-market PnL from a flat start, ``bucket_qty`` is the
strategy volume traded since the previous row. Everything plotted here comes
from that one file; an optional raw trades CSV adds market volume for context.
"""

from __future__ import annotations

import math
from pathlib import Path

import matplotlib
import pandas as pd

# Default to the non-interactive backend so saving a PNG never needs a display.
matplotlib.use("Agg", force=False)

import matplotlib.dates as mdates  # noqa: E402
import matplotlib.pyplot as plt  # noqa: E402

SERIES_COLUMNS = [
    "ts",
    "mid",
    "inventory",
    "equity",
    "realized_cash",
    "turnover",
    "fees",
    "fills",
    "bucket_qty",
    "bucket_fills",
]


def load_series(path: str | Path) -> pd.DataFrame:
    """Read a recorder CSV into a frame with a parsed ``time`` column."""
    df = pd.read_csv(path)
    missing = [c for c in SERIES_COLUMNS if c not in df.columns]
    if missing:
        raise ValueError(f"{path}: missing columns {missing}; is this an engine series CSV?")
    df["time"] = pd.to_datetime(df["ts"], unit="us")
    return df


def load_trades(path: str | Path) -> pd.DataFrame:
    """Read the raw market trades CSV (``,local_timestamp,side,price,amount``)."""
    df = pd.read_csv(path)
    df = df.rename(columns={"local_timestamp": "ts"})
    df["time"] = pd.to_datetime(df["ts"], unit="us")
    return df


def downsample(series: pd.DataFrame, max_points: int) -> pd.DataFrame:
    """Bucket a long series down to ~max_points rows for fast, legible plots.

    State/level columns (price, equity, inventory, turnover, ...) take the last
    value in each bucket; flow columns (per-row volume and fill counts) are
    summed, so totals are preserved. A full run can be ~half a million rows;
    plotting every one is slow and turns the volume bars into a solid block.
    """
    n = len(series)
    if max_points <= 0 or n <= max_points:
        return series
    step = math.ceil(n / max_points)
    g = series.groupby(series.index // step)
    out = pd.DataFrame(
        {
            "ts": g["ts"].last(),
            "time": g["time"].last(),
            "mid": g["mid"].last(),
            "inventory": g["inventory"].last(),
            "equity": g["equity"].last(),
            "realized_cash": g["realized_cash"].last(),
            "turnover": g["turnover"].last(),
            "fees": g["fees"].last(),
            "fills": g["fills"].last(),
            "bucket_qty": g["bucket_qty"].sum(),
            "bucket_fills": g["bucket_fills"].sum(),
        }
    ).reset_index(drop=True)
    return out


def _market_volume(trades: pd.DataFrame, freq: str) -> pd.DataFrame:
    """Resample raw trade prints into signed buy/sell volume bars."""
    t = trades.set_index("time")
    buys = t.loc[t["side"] == "buy", "amount"].resample(freq).sum()
    sells = t.loc[t["side"] == "sell", "amount"].resample(freq).sum()
    out = pd.DataFrame({"buy": buys, "sell": sells}).fillna(0.0)
    return out


def render(
    series: pd.DataFrame,
    out: str | Path | None = None,
    trades: pd.DataFrame | None = None,
    title: str = "Backtest",
    max_points: int = 3000,
) -> plt.Figure:
    """Draw the price / PnL / inventory / volume panels and (optionally) save."""
    series = downsample(series, max_points)
    fig, axes = plt.subplots(
        4, 1, figsize=(13, 11), sharex=True, gridspec_kw={"height_ratios": [3, 3, 2, 2]}
    )
    ax_price, ax_pnl, ax_inv, ax_vol = axes
    t = series["time"].reset_index(drop=True)

    # --- Price (mid) -------------------------------------------------------
    ax_price.plot(t, series["mid"], color="#1f77b4", lw=1.0, label="mid")
    ax_price.set_ylabel("price")
    ax_price.set_title(title, loc="left", fontsize=12, fontweight="bold")
    ax_price.legend(loc="upper left", fontsize=8)
    ax_price.grid(True, alpha=0.3)

    # --- PnL (equity) + realized cash -------------------------------------
    ax_pnl.plot(t, series["equity"], color="#2ca02c", lw=1.2, label="equity (MtM PnL)")
    ax_pnl.plot(t, series["realized_cash"], color="#9467bd", lw=0.8, alpha=0.7, label="realized cash")
    ax_pnl.axhline(0.0, color="grey", lw=0.6)
    ax_pnl.fill_between(
        t, 0.0, series["equity"], where=series["equity"] >= 0, color="#2ca02c", alpha=0.12
    )
    ax_pnl.fill_between(
        t, 0.0, series["equity"], where=series["equity"] < 0, color="#d62728", alpha=0.12
    )
    ax_pnl.set_ylabel("PnL")
    ax_pnl.legend(loc="upper left", fontsize=8)
    ax_pnl.grid(True, alpha=0.3)

    # --- Inventory ---------------------------------------------------------
    ax_inv.plot(t, series["inventory"], color="#ff7f0e", lw=1.0)
    ax_inv.axhline(0.0, color="grey", lw=0.6)
    ax_inv.fill_between(t, 0.0, series["inventory"], color="#ff7f0e", alpha=0.15)
    ax_inv.set_ylabel("inventory")
    ax_inv.grid(True, alpha=0.3)

    # --- Volume: strategy traded volume per sample + cumulative turnover ---
    # Market volume (when overlaid) is orders of magnitude larger than the
    # strategy's fills, so it gets its own offset right axis rather than burying
    # the bars on a shared scale.
    width = _bar_width(t)
    ax_vol.bar(
        t, series["bucket_qty"], width=width, color="#1f77b4", alpha=0.7, label="strategy fills"
    )
    ax_vol.set_ylabel("strategy vol", color="#1f77b4")
    ax_vol.tick_params(axis="y", labelcolor="#1f77b4")
    ax_vol.grid(True, alpha=0.3)

    extra_handles: list = []
    ax_turn = ax_vol.twinx()
    ax_turn.plot(t, series["turnover"], color="#8c564b", lw=1.0, label="cum. turnover")
    ax_turn.set_ylabel("cum. turnover", color="#8c564b")
    ax_turn.tick_params(axis="y", labelcolor="#8c564b")
    extra_handles += ax_turn.get_legend_handles_labels()[0]

    if trades is not None and not trades.empty:
        mkt = _market_volume(trades, _freq_from(t))
        ax_mkt = ax_vol.twinx()
        ax_mkt.spines["right"].set_position(("outward", 52))
        ax_mkt.plot(
            mkt.index, mkt["buy"] + mkt["sell"], color="grey", lw=1.0, alpha=0.6,
            label="market volume",
        )
        ax_mkt.set_ylabel("market vol", color="grey")
        ax_mkt.tick_params(axis="y", labelcolor="grey")
        extra_handles += ax_mkt.get_legend_handles_labels()[0]

    handles = ax_vol.get_legend_handles_labels()[0] + extra_handles
    ax_vol.legend(handles, [h.get_label() for h in handles], loc="upper left", fontsize=8)

    # Adapt the tick labels to the span: times for an intraday run, dates for a
    # multi-day one (a full backtest can cover several days).
    locator = mdates.AutoDateLocator()
    ax_vol.xaxis.set_major_locator(locator)
    ax_vol.xaxis.set_major_formatter(mdates.ConciseDateFormatter(locator))
    fig.autofmt_xdate()
    fig.tight_layout()

    if out is not None:
        fig.savefig(out, dpi=120)
    return fig


def _span_seconds(t: pd.Series) -> float:
    if len(t) < 2:
        return 1.0
    return max((t.iloc[-1] - t.iloc[0]).total_seconds(), 1.0)


def _bar_width(t: pd.Series) -> float:
    """Bar width in matplotlib date units (days), ~one sampling step wide."""
    step = _span_seconds(t) / max(len(t) - 1, 1)
    return 0.8 * step / 86400.0


def _freq_from(t: pd.Series) -> str:
    """A pandas resample frequency roughly matching the series sampling step."""
    step = max(int(round(_span_seconds(t) / max(len(t) - 1, 1))), 1)
    return f"{step}s"
