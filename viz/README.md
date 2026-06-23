# bt-viz

Plots for the [cmf backtesting engine](../README.md): **price**, **PnL/equity**,
**inventory**, and **volume** from the time-series CSV the engine writes.

## How it fits together

The C++ engine records a per-row time series during a backtest (see
`bt::TimeSeriesRecorder`). Enable it by setting `series_csv` (and optionally
`series_interval_ms`) in the run config — the bundled `configs/{default,sample,as_online}.json`
already do. After a run you get, e.g., `reports/series_sample.csv` with:

```
ts,mid,inventory,equity,realized_cash,turnover,fees,fills,bucket_qty,bucket_fills
```

`bt-viz` reads that file and paints the panels. C++ stays fast and deterministic;
Python only plots.

## Setup

```bash
cd viz
poetry install
```

## Use

```bash
# from the viz/ directory
poetry run bt-viz ../reports/series_sample.csv               # -> ../reports/series_sample.png
poetry run bt-viz ../reports/series_as_online.csv -o run.png --show   # also open a window
poetry run bt-viz ../reports/series_as_online.csv \
    --trades ../market_data/trades.csv                       # overlay raw market volume
poetry run bt-viz ../reports/series_as_online.csv --max-points 5000   # finer detail (slower)

# live: re-render whenever the engine rewrites the CSV (the "watcher service")
poetry run bt-viz ../reports/series_sample.csv --watch
```

From the repo root:

```bash
make plot     # backtest the sample and render reports/series_sample.png
make plots    # render reports/series_<strategy>.png for every series CSV
```

`make plots` expects the per-strategy CSVs — run `make experiments` first (it writes
`reports/series_{fixed,as,microprice_as,as_online}.csv`). A full run is ~500k rows;
`bt-viz` buckets it to `--max-points` (default 3000) for fast, legible figures —
state columns keep their last value per bucket, volume/fill counts are summed.

## Panels

| panel | source column(s) | meaning |
|--|--|--|
| price | `mid` | marked mid price over time |
| PnL | `equity`, `realized_cash` | mark-to-market PnL from a flat start; realized cash |
| inventory | `inventory` | signed position (base units), 0-line marked |
| volume | `bucket_qty`, `turnover` | strategy volume per sample (bars) + cumulative turnover (line); optional market volume with `--trades` |
