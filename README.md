# Backtesting Engine

An event-driven backtesting engine for HFT / market-making research, in modern **C++23**.
It replays historical L2 order-book snapshots and trade prints, simulates limit-order
placement/cancellation with **queue-position-aware** execution, and reports PnL, inventory,
and turnover.

> **Status: strategies implemented.** Data pipeline, order book, queue-aware execution
> simulator, PnL/inventory/turnover metrics, and four strategies — a constant-spread
> quoter (pipeline stand-in), the paper-faithful **Avellaneda–Stoikov (2008)** and
> **A-S + the Stoikov (2018) micro-price**, and a deployable **online A-S** (rolling
> horizon + online σ/k + inventory cap) that holds inventory near flat — all
> unit-tested (32 tests) and validated end-to-end on the sample data (~23M events
> replay in <1 s). For the faithful strategies σ and k are calibrated offline in a
> single pass and held constant; the online variant re-estimates them continuously;
> the micro-price is fitted from a finite-state Markov chain. See
> **[docs/STRATEGY.md](docs/STRATEGY.md)** for the model descriptions, performance
> results, and improvement roadmap.

## Prerequisites

- `clang-21` + `libc++-21` (`libc++-21-dev`)
- CMake ≥ 3.28 (developed on 4.3) and Ninja
- C++23, classic headers (no modules)

## Build & test

```bash
make build     # configure + build (fetches Catch2 + nlohmann/json on first run)
make test      # run the unit/integration suite (ctest)
```

Or drive CMake directly:

```bash
cmake --preset release
cmake --build --preset release
ctest --preset release
```

A `debug` preset adds AddressSanitizer/UBSan.

## Quick start

The sample CSVs live in `market_data/` (git-ignored). Convert them once to packed binary,
then run a backtest:

```bash
make convert                  # CSV -> market_data/*.bin (one-time)
make run                      # backtest with configs/default.json
# or: make run CONFIG=configs/your.json
```

`make run` writes a summary to `report.csv` and prints, e.g.:

```
events=22901679 book=1036690 trades=21864989 fills=764349 | inv=-88008 turnover=5.43e6 fees=543 equity_pnl=-1525.9 | 0.92s (24.84M ev/s) -> report.csv
```

### Bundled sample (no large data needed)

A tiny real-format sample ships in the repo (`market_data/*_sample.csv`), so the project is
runnable out of the box:

```bash
make sample     # converts the sample CSVs and backtests configs/sample.json
```

For a real run, drop the full `lob.csv` / `trades.csv` into `market_data/` (git-ignored) and
use `configs/default.json`.

## Configuration (`configs/default.json`)

| key | meaning |
|--|--|
| `lob_bin` / `trades_bin` | converted input paths |
| `report_csv` | summary output path |
| `series_csv` | per-row time-series for plotting (price/PnL/inventory/volume); empty ⇒ off |
| `series_interval_ms` | market-time sampling step for the series |
| `fee_bps` | fee charged on each fill's notional |
| `order_latency_us` | delay before a placed order joins the book |
| `feed_latency_us` | reserved (roadmap) |
| `half_spread` | quote offset from mid, in ticks |
| `order_qty` | size per quote |
| `max_inventory` | hard position cap |
| `queue_model` | `optimistic` or `proportional` |
| `strategy` | `fixed` (stand-in), `as` (faithful A-S), `microprice_as` (A-S + micro-price), or `as_online` (deployable A-S) |
| `as_gamma` | risk aversion (tune per instrument — see [STRATEGY.md](docs/STRATEGY.md#1-avellanedastoikov-2008)) |
| `as_horizon_s` | horizon `T` (seconds): single-shot for `as`/`microprice_as`, rolling session for `as_online` |
| `as_sigma` / `as_k` | pin volatility / arrival-decay instead of calibrating offline (≤0 ⇒ calibrate offline) |
| `as_vol_alpha` / `as_k_alpha` | EWMA weights for the `as_online` online σ/k estimators |
| `as_min_half_spread` | `as_online` half-spread floor, in ticks |
| `mp_imbalance_bins` / `mp_spread_bins` | micro-price Markov-chain state granularity |

## Strategies

Four strategies plug into one `Strategy` interface (selected via `strategy`):
`fixed` (constant-spread stand-in), `as` (paper-faithful Avellaneda–Stoikov 2008),
`microprice_as` (A-S centred on the Stoikov 2018 micro-price), and `as_online` (the
deployable A-S: rolling horizon + online σ/k + inventory cap, holding inventory near
flat). For the faithful strategies σ and k are calibrated offline in one pass and held
constant; `as_online` seeds from that calibration and then re-estimates them online.
The micro-price is fitted from a one-pass Markov chain over the LOB. Run them on the
full data and reproduce the report with:

```bash
make experiments     # -> reports/{fixed,as,microprice_as,as_online}.csv
make sweep           # risk-aversion (gamma) sweep -> reports/gamma_sweep.csv
```

`as_online` is the right strategy to evaluate as a market maker (controlled book);
`as`/`microprice_as` are the literal paper implementations. See
[docs/STRATEGY.md](docs/STRATEGY.md) for the performance comparison.

See **[docs/STRATEGY.md](docs/STRATEGY.md)** for the models, calibration,
performance tables, and roadmap. Pre-made configs: `configs/{fixed,as,microprice_as}.json`.

## Plots

When `series_csv` is set, the engine records a per-row time series
(`ts,mid,inventory,equity,realized_cash,turnover,fees,fills,bucket_qty,bucket_fills`)
sampled every `series_interval_ms` of market time — written by `bt::TimeSeriesRecorder`
alongside the summary. C++ stays fast and deterministic; a small **poetry** project in
[`viz/`](viz/) paints it with pandas + matplotlib:

```bash
make viz-install   # one-time: cd viz && poetry install
make plot          # backtest the sample and render reports/series_sample.png
```

`make plot` draws four stacked panels — **price** (mid), **PnL** (equity + realized cash),
**inventory**, and **volume** (per-sample strategy fills + cumulative turnover).

### One plot per strategy

`make experiments` writes a `reports/series_<strategy>.csv` for each of `fixed`, `as`,
`microprice_as`, and `as_online`. Render them all to `reports/series_<strategy>.png`:

```bash
make experiments    # backtest every strategy -> reports/series_*.csv
make plots          # render reports/series_<strategy>.png for each (needs viz-install)
```

A full run is ~half a million samples; `bt-viz` buckets the series down to ~3000 points
(`--max-points`) so each figure stays fast and legible. Render any single run directly,
optionally overlaying raw market volume, or live-watch the CSV:

```bash
cd viz
poetry run bt-viz ../reports/series_as_online.csv -o run.png --show
poetry run bt-viz ../reports/series_as_online.csv --trades ../market_data/trades.csv
poetry run bt-viz ../reports/series_as_online.csv --watch   # re-render on every change
```

See [`viz/README.md`](viz/README.md) for details.

## Project layout

```
include/bt/{core,data,book,exec,strategy,metrics,engine}/   public headers
src/bt/**                                                   definitions
apps/{convert_csv,backtest}.cpp                             executables
tests/                                                      Catch2 tests
configs/default.json                                        sample config
viz/                                                        poetry plotting project (bt-viz)
docs/ARCHITECTURE.md                                        design, market model, roadmap
CMakeLists.txt, CMakePresets.json, Makefile                 build
```

## Code style / linting

`make format` (clang-format) and `make tidy` (clang-tidy) use the LLVM-derived
`.clang-format` / `.clang-tidy`. `make tidy-fix` auto-applies fixes — e.g. to add braces if
you re-enable `readability-braces-around-statements`.

## Documentation

See **[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)** for the design, the market model and its
assumptions, the execution/queue model, determinism, and the roadmap.
