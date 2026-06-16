# Market-Making Strategies: Avellaneda–Stoikov + Micro-Price

This document describes the two market-making strategies implemented on top of the
backtesting engine, how their parameters are calibrated from the data, the
performance results on the sample dataset, and an improvement roadmap.

- **Source:** [`avellaneda_stoikov.{hpp,cpp}`](../include/bt/strategy/avellaneda_stoikov.hpp),
  [`microprice.{hpp,cpp}`](../include/bt/strategy/microprice.hpp),
  [`microprice_as.hpp`](../include/bt/strategy/microprice_as.hpp),
  [`calibration.hpp`](../include/bt/strategy/calibration.hpp).
- **Papers:** Avellaneda & Stoikov (2008), *High-frequency trading in a limit order
  book*; Stoikov (2018), *The Micro-Price* — both in [`docs/papers/`](papers/).

---

## 1. Avellaneda–Stoikov (2008)

### Model

The mid-price is modelled as Brownian motion `S_t = s + σ W_t`. A dealer holding
inventory `q` faces inventory risk (diffusive mid) and execution risk (Poisson
order arrivals `λ(δ) = A e^{-kδ}` decaying with quote distance `δ` from the mid).
Maximising exponential utility of terminal wealth yields a two-step rule:

1. **Reservation (indifference) price** — the mid, shifted against inventory:

   ```
   r(s, q, t) = s − q · γ · σ² · (T − t)
   ```

2. **Optimal half-spread** around the reservation price:

   ```
   δ = ½ · γ · σ² · (T − t) + (1/γ) · ln(1 + γ/k)
   ```

   The quotes are then `bid = r − δ`, `ask = r + δ` (paper eqs. 3.10–3.12). The
   first term widens the spread with volatility/horizon; the second is the
   liquidity-premium floor set by the order-arrival decay `k`.

The reservation price leans away from the mid in proportion to inventory, so the
maker quotes more aggressively on the side that *reduces* risk and shades the side
that *adds* it — the inventory control the constant-spread quoter lacks.

### How it maps onto the engine

- A new `AvellanedaStoikov : Strategy` re-quotes on every `on_book`, cancelling
  and replacing only when the target tick moves (no churn), and stops quoting a
  side at the inventory cap — same lifecycle as `FixedSpreadQuoter`, but with
  `bid`/`ask` from the formulas above.
- **Inventory is measured in lots** of `order_qty` (`q = inventory / order_qty`),
  matching the paper's "one unit per quote" and keeping the risk term scale-stable.
- **Rolling horizon.** The paper's finite horizon `T` liquidates inventory by the
  terminal time. For a continuous multi-day replay we use a *rolling session* of
  length `as_horizon_s`: `(T − t)` counts down within a session and resets at the
  next, so the time-decay term stays active throughout.

### Calibration from the data

| Param | Source |
|--|--|
| `σ²` (vol) | **Online**, `VolatilityEstimator`: `EWMA(ΔS²) / EWMA(Δt)` → variance per second. Tracking the two EWMAs separately keeps the estimate stable when book updates are microseconds apart. |
| `k` (arrival decay) | **Online**, `ArrivalRateEstimator`. The paper derives `λ(δ) ∝ P(impact > δ)`, so trade distances from the mid are exponential with rate `k` ⇒ `k = 1 / E[δ_trade]` (an EWMA). This yields `k` in `1/price` units, correctly scaled to the instrument. |
| `γ` (risk aversion) | User parameter (`as_gamma`) — genuinely a preference. See the scaling note below. |
| `T` (horizon) | User parameter (`as_horizon_s`). |

> **Scaling note (important).** All A-S terms live in *price* units, so the right
> `γ` depends on the instrument's price/tick scale. On this dataset (price ≈ 0.011,
> tick = 1e-7) the inventory skew only becomes a full tick around `γ ≈ 500`; at the
> generic default `γ = 0.3` the skew is sub-tick and A-S degenerates to a symmetric
> quoter. The γ-sweep below makes this concrete and is the recommended way to tune.

---

## 2. Micro-Price extension (Stoikov 2018)

### Model

The micro-price is the martingale limit of expected future mid-prices,
`P^micro = M + g(I, S)`, an adjustment to the mid that depends on the top-of-book
**imbalance** `I = Q_b / (Q_b + Q_a)` and the **spread** `S`. It is a strictly
better short-horizon predictor of the mid than either the mid or the
volume-weighted mid (which over-reacts and isn't a martingale).

`g` is estimated from a finite-state Markov chain on the discretised `(I, S)`:

```
Q   = transitions that leave the mid unchanged (transient)
T   = transitions on which the mid moves (to a new state)
rvec[x] = E[ΔM · 1(mid moved) | state x]          (the paper's R·K product)
G1  = (I − Q)⁻¹ rvec                               (first-order adjustment)
B   = (I − Q)⁻¹ T
G*  = Σ_{i≥0} Bⁱ G1                                (full adjustment)
```

Samples are **symmetrised** (`(I,S,ΔM) → (1−I,S,−ΔM)`) so the chain is unbiased
and the series converges (Theorem 3.1, `B*·G1 = 0`). `(I − Q)` is solved by
Gauss–Jordan with partial pivoting (`MicropriceModel`, ~`nm` states, one-time).

### How it maps onto the engine

- `MicropriceModel::calibrate()` makes **one pass over the LOB** before the run
  (≈1.0M transitions on the sample, <1 s) to fit `G*`.
- `MicropriceAS : AvellanedaStoikov` overrides only the quote *centre*: it replaces
  the mid `s` with the micro-price `M + g(I, S)` in the reservation price.
  Everything else (spread, inventory skew, online σ/k) is inherited. So the
  reservation price now anticipates imbalance-driven drift *on top of* the A-S
  inventory skew — directly targeting the adverse-selection that pure replay
  otherwise ignores.

---

## 3. Performance results

Full sample dataset: **22,901,679 events** (1,036,690 book updates, 21,864,989
trades) over ~6 days. `fee_bps = 1.0`, `order_latency_us = 1000`,
`order_qty = 1000`, `max_inventory = 100,000`, optimistic queue. PnL is
mark-to-market equity from a flat start (engine is a price-taker overlay, so
absolute PnL is optimistic — see [ARCHITECTURE.md](ARCHITECTURE.md#market-model--assumptions);
the *relative* comparison is the signal). Reproduce with `make experiments`.

### Headline (γ = 500, near-flat inventory)

| Strategy | Fills | End inventory | Turnover | Fees | Equity PnL |
|--|--:|--:|--:|--:|--:|
| Fixed-spread (baseline) | 764,349 | −88,008 | 5.43M | 543.3 | **−1525.9** |
| Avellaneda–Stoikov | 261,057 | **90** | 1.96M | 195.8 | **−405.8** |
| Micro-price + A-S | 248,444 | **−52** | 1.88M | 187.7 | **−379.5** |

- **A-S vs fixed:** loss cut by **73%**, inventory flattened from −88,008 to ~90
  (of a 100k cap), turnover and fees more than halved — the inventory control and
  volatility-aware spread doing their job.
- **Micro-price vs A-S:** a further **~6.5%** PnL improvement (−379.5 vs −405.8)
  with even tighter inventory — the better short-horizon predictor reduces the
  rate at which we are filled just before the mid moves against us.

### Risk-aversion (γ) sweep — `reports/gamma_sweep.csv`

| γ | A-S inv | A-S PnL | MP-AS inv | MP-AS PnL |
|--:|--:|--:|--:|--:|
| 1 | 33,110 | −342.1 | 37,892 | −341.3 |
| 10 | −8,770 | −390.5 | 10,964 | −363.7 |
| 100 | −1,589 | −443.4 | −2,226 | −417.0 |
| 500 | 90 | −405.8 | −52 | −379.5 |
| 2000 | 1,249 | −294.2 | 981 | **−276.1** |

The sweep reproduces the paper's qualitative behaviour: **higher γ ⇒ tighter
inventory** (the maker pays a wider effective spread to avoid carrying risk).
Micro-price-A-S **dominates plain A-S on PnL at every γ**. On this trending tape
the PnL-optimal point is the risk-averse `γ ≈ 2000` (less time exposed to the
trend), while `γ ≈ 500` is the textbook "flat book" operating point.

---

## 4. Improvement roadmap

**Strategy**
- **Full finite-horizon `θ`-PDE** (paper §3.1) instead of the symmetric proxy
  `r = s − qγσ²(T−t)`, giving asymmetric `δ^a ≠ δ^b` directly.
- **Multi-level / sized quoting** and an Avellaneda–Stoikov–Cartea inventory
  penalty; size as a function of edge and imbalance.
- **Joint γ/σ/k auto-tuning** (e.g. walk-forward) and per-instrument γ
  normalisation by tick/price scale so the default is sane without a sweep.
- **Micro-price:** richer state (Level-2 depth, recent trade-flow), online/rolling
  re-fit instead of a single calibration pass, and a horizon-aware blend.

**Evaluation**
- **Risk-adjusted metrics:** equity-curve time series → Sharpe, max drawdown,
  fill ratio, inventory distribution, per-session PnL (the engine already marks
  to mid on every event; needs a time-series export from `Metrics`).
- **Adverse-selection diagnostics:** mark fills at +Δt to quantify how often we
  are run over, and confirm the micro-price reduces it.

**Engine realism** (also in [ARCHITECTURE.md](ARCHITECTURE.md#roadmap))
- Feed/cancel latency, multi-level queue accounting, and a market-impact /
  adverse-selection model so absolute (not just relative) PnL is trustworthy.
