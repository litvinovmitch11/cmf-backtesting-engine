# Architecture

## Objective

Replay historical market data and evaluate market-making strategy performance: L2
order-book simulation, limit-order placement/cancellation, **queue-position-aware**
execution, and PnL / inventory / turnover metrics. The target strategy is
Avellaneda–Stoikov (2008) with the microprice (2018) extension — see [Roadmap](#roadmap).

This document describes the engine's design, its market model and assumptions, the
execution/queue model, and the determinism guarantees.

## Design principles

1. **Determinism** — single-threaded, an event-time virtual clock, and stable tie-breaks.
   Same input ⇒ byte-identical output (verified: two runs produce identical reports).
2. **No look-ahead** — events are delivered in timestamp order; a strategy at time *T*
   sees only data with `ts ≤ T`; orders it submits take effect at `T + order_latency`.
3. **Event-time, not wall-clock** — the clock is the current event's timestamp.
4. **Performance** — CSV is converted once to packed binary and mmapped; prices are int64
   ticks. The full ~6-day sample (~23M events) replays in **<1 s (~25M events/s)**.

## Market model & assumptions (read this first)

The historical files are a **fixed recording**. The strategy's orders are *overlaid* on
top of it:

- The engine **answers the strategy with Fills** — which change inventory, PnL, and open
  orders — but it does **not** alter the recorded tape. "Making a trade" returns a `Fill`;
  it does not move recorded prices.
- This is the standard **price-taker / no-market-impact** assumption, valid when our size
  is small relative to market volume. The exam's rule — *execution occurs when the market
  price crosses the order level* — is exactly this overlay.
- **Known optimism:** pure replay ignores **adverse selection** (in reality you tend to be
  filled right before the price moves against you). **Queue-position modeling** mitigates
  much of it (you fill *behind* the recorded queue). Full impact feedback would require a
  market-impact / agent-based model (Roadmap).

> This optimism is visible in the demo: the placeholder constant-spread quoter accumulates
> inventory and loses money on a trending market — expected for a naive quoter. The point of
> the skeleton is a correct, fast, deterministic *engine*; Avellaneda–Stoikov is the strategy.

## Components

```
EventSource (iface) ─┐
  LobBinSource       ├─► FeedMerger ─► BacktestEngine ─► ExecutionSimulator ─► Strategy (iface)
  TradesBinSource    ┘   (k-way merge) (event loop /     (resting orders,       FixedSpreadQuoter
                                        virtual clock /    QueueModel fills,      (A-S slots in here)
                                        OrderApi)          LatencyModel)
                                            │             ▲          │
                                            ├─► OrderBook ─┘          ▼
                                            └─► Metrics (PnLTracker): PnL / inventory / turnover
```

- **`EventSource` / `FeedMerger`** (`bt/data`) — a source is one chronological channel
  (`LobBinSource`, `TradesBinSource`). `FeedMerger` k-way-merges N sources by next
  timestamp into a single stream. This is the in-process replacement for "pipes": many
  channels, no IPC, fully deterministic.
- **`OrderBook`** (`bt/book`) — a zero-copy view over the latest L2 snapshot; exposes best
  bid/ask, `mid`, `microprice` (imbalance-weighted), `spread`, and `size_at(side, px)`.
- **`ExecutionSimulator` + `QueueModel` + `LatencyModel`** (`bt/exec`) — holds the
  strategy's resting orders and matches them against the recorded market (below).
- **`Strategy` / `OrderApi`** (`bt/strategy`) — the strategy reacts to `on_book` / `on_trade`
  / `on_fill` and emits orders via `OrderApi`. `FixedSpreadQuoter` is the stand-in.
- **`Metrics` / `PnLTracker`** (`bt/metrics`) — observes fills and periodic marks; computes
  realized cash, inventory, turnover, fees, and mark-to-market equity.
- **`BacktestEngine`** (`bt/engine`) — owns the event loop and *is* the strategy's
  `OrderApi` (forwarding to the sim stamped with the current event time).
- **`Config`** (`bt/engine`) — JSON config (paths, fees, latency, quoter params, queue model).

## Data pipeline & binary format

The vendor CSVs (Tardis `book_snapshot_25` + trades) are converted once to packed,
fixed-width, little-endian binary (`convert_csv`) so replays are I/O-bound on a compact
file rather than re-parsing ~1.9 GB of text each run.

- **`LobRecord`** (808 B): `ts(i64)` + 25 × `{px(i64 ticks), qty(f64)}` per side.
- **`TradeRecord`** (32 B): `ts(i64)`, `px(i64 ticks)`, `qty(f64)`, `side(u8)`.
- A 40-byte `BinHeader` (magic / version / depth / price_scale / record_count) prefixes each
  file for validation.

Prices are integer **ticks** (`price × 1e7`) so the crossing/queue logic never compares
floats for equality. The converter reports `max_tick_round_err`; on the sample data it is
`1.46e-11`, confirming the 1e-7 tick captures every price exactly. `BookUpdate` events carry
pointers *into* the mmapped records (`BookLevel` matches the on-disk layout), so iteration
copies nothing — hence `-fno-strict-aliasing` on the library.

## Execution model

The fill model is **trade-driven crossing** with FIFO queue estimation:

- **Eligibility (crossing):** a resting **buy** at `px` fills from a **sell** print with
  `trade_px ≤ px`; a resting **sell** fills from a **buy** print with `trade_px ≥ px`.
- **Queue:** on activation, `queue_ahead = size_at(side, px)` (the volume already resting at
  our level). A trade consumes the queue first; the **overflow** fills us (supporting
  **partial fills**). Improving the touch ⇒ `queue_ahead = 0` ⇒ first in line.
- **Marketable orders** (a buy at/above the ask, etc.) take liquidity by walking the opposite
  book on arrival, producing taker fills.
- **Latency:** a placed order joins the book at `decision_ts + order_latency`.
- **Two `QueueModel`s** bracket fill optimism:
  - `OptimisticQueue` — cancels are assumed *behind* us; only trades shrink the queue
    (upper bound on fills).
  - `ProportionalQueue` — a drop in visible level size shrinks our queue position
    proportionally (more conservative). *Limitation:* on L2 it cannot distinguish
    trade-driven from cancel-driven size drops, so it can over-attribute — a deliberate loose
    lower bound.

## Event loop

```text
while feed.has_next():
    ev   = feed.next();  now = ev.ts
    if BookUpdate:  book.apply(ev); exec.on_book_update();      # refresh queue estimates
                    strategy.on_book(book, now, api)            # may place/cancel (api = engine)
    if TradePrint:  exec.on_trade(ev)                           # passive fills
                    strategy.on_trade(ev, book, now, api)
    exec.activate_due(now)                                      # promote in-flight; marketable fills
    for fill in exec.take_fills(): strategy.on_fill; metrics.on_fill
    metrics.on_mark(now, book.mid)
```

Ordering is chosen to avoid look-ahead: the book advances and queues refresh *before* the
strategy reacts; the strategy's orders take effect only at `activate_due` (subject to
latency); fills are delivered after matching.

## Determinism

`FeedMerger` uses a binary min-heap keyed on next timestamp, breaking ties by **source index**
(the order sources were added — LOB before trades). Combined with a single thread and the
event-time clock, replays are reproducible bit-for-bit.

## Results (sample data, ~6 days)

| run | events | fills | time | throughput |
|--|--|--|--|--|
| full | 22,901,679 | 764,349 | ~0.9 s | ~25M ev/s |

Two consecutive runs produce byte-identical `report.csv` (determinism check).

## Strategies

The three strategies and their calibration are documented in
**[STRATEGY.md](STRATEGY.md)** (models, performance results, roadmap). In brief:

- **Avellaneda–Stoikov (2008)** *(done)* — `AvellanedaStoikov : Strategy`, reservation
  price `r = s − q·γ·σ²·(T−t)` and half-spread `δ = ½γσ²(T−t) + (1/γ)ln(1+γ/k)`, with
  `σ` and `k` calibrated **online** from the data (`calibration.hpp`).
- **Micro-price (2018)** *(done)* — `MicropriceAS : AvellanedaStoikov` centres the quotes
  on the full Stoikov micro-price `M + g(I,S)`, where `g` is fitted from a finite-state
  Markov chain over the LOB (`MicropriceModel`, one-pass calibration).

## Roadmap

- **Performance report** — equity curve, drawdown, Sharpe, fill ratio, inventory
  distribution (time-series export from `Metrics`).
- **Strategy** — full finite-horizon `θ`-PDE (asymmetric δ), multi-level/sized quoting,
  joint γ/σ/k auto-tuning, richer micro-price state with online re-fit.
- **Execution refinements** — feed latency, cancel latency, multi-level queue accounting,
  adverse-selection / market-impact models, fee tiers.
