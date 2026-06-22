# Backtesting Engine

An event-driven backtesting engine for HFT / market-making research, in modern **C++23**.
It replays historical L2 order-book snapshots and trade prints, simulates limit-order
placement/cancellation with **queue-position-aware** execution, and reports PnL, inventory,
and turnover.

> **Status: strategies implemented.** Data pipeline, order book, queue-aware execution
> simulator, PnL/inventory/turnover metrics, and three strategies — a constant-spread
> quoter (pipeline stand-in), **Avellaneda–Stoikov (2008)**, and **A-S + the Stoikov
> (2018) micro-price** — all unit-tested (29 tests) and validated end-to-end on the
> sample data (~23M events replay in <1 s). σ and k are calibrated offline in a single
> pass and held constant (paper-faithful); the micro-price is fitted from a
> finite-state Markov chain. See
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
| `fee_bps` | fee charged on each fill's notional |
| `order_latency_us` | delay before a placed order joins the book |
| `feed_latency_us` | reserved (roadmap) |
| `half_spread` | quote offset from mid, in ticks |
| `order_qty` | size per quote |
| `max_inventory` | hard position cap |
| `queue_model` | `optimistic` or `proportional` |
| `strategy` | `fixed` (stand-in), `as` (Avellaneda–Stoikov), or `microprice_as` |
| `as_gamma` | risk aversion (tune per instrument — see [STRATEGY.md](docs/STRATEGY.md#1-avellanedastoikov-2008)) |
| `as_horizon_s` | rolling session length `T` (seconds) for the `(T−t)` term |
| `as_sigma` / `as_k` | pin volatility / arrival-decay instead of calibrating offline (≤0 ⇒ calibrate offline) |
| `mp_imbalance_bins` / `mp_spread_bins` | micro-price Markov-chain state granularity |

## Strategies

Three strategies plug into one `Strategy` interface (selected via `strategy`):
`fixed` (constant-spread stand-in), `as` (Avellaneda–Stoikov 2008), and
`microprice_as` (A-S centred on the Stoikov 2018 micro-price). σ and k are
calibrated offline in one pass and held constant; the micro-price is fitted from a
one-pass Markov chain over the LOB. Run the three on the full data and reproduce the
report with:

```bash
make experiments     # -> reports/{fixed,as,microprice_as}.csv
make sweep           # risk-aversion (gamma) sweep -> reports/gamma_sweep.csv
```

See **[docs/STRATEGY.md](docs/STRATEGY.md)** for the models, calibration,
performance tables, and roadmap. Pre-made configs: `configs/{fixed,as,microprice_as}.json`.

## Project layout

```
include/bt/{core,data,book,exec,strategy,metrics,engine}/   public headers
src/bt/**                                                   definitions
apps/{convert_csv,backtest}.cpp                             executables
tests/                                                      Catch2 tests
configs/default.json                                        sample config
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
