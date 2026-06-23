# Market-Making Strategies: Avellaneda–Stoikov + Micro-Price

This document describes the market-making strategies implemented on top of the
backtesting engine — the paper-faithful **Avellaneda–Stoikov (§1)** and **micro-price
A-S (§2)**, plus a deployable **online A-S (§3)** that holds inventory near flat — how
their parameters are calibrated from the data, the performance results on the sample
dataset, and an improvement roadmap.

- **Source:** [`avellaneda_stoikov.{hpp,cpp}`](../include/bt/strategy/avellaneda_stoikov.hpp),
  [`microprice.{hpp,cpp}`](../include/bt/strategy/microprice.hpp),
  [`microprice_as.hpp`](../include/bt/strategy/microprice_as.hpp),
  [`avellaneda_stoikov_online.{hpp,cpp}`](../include/bt/strategy/avellaneda_stoikov_online.hpp).
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

This implementation is deliberately **faithful to the paper** rather than tuned for
the replay:

- `AvellanedaStoikov : Strategy` re-quotes on every `on_book`, cancelling and
  replacing only when the target tick moves (no churn). `bid`/`ask` come straight
  from the formulas above — **no inventory cap and no min-spread floor**: both
  sides are always quoted at exactly `r ± δ`, and the exponential-utility objective
  is what is supposed to control inventory.
- **Inventory is measured in lots** of `order_qty` (`q = inventory / order_qty`),
  matching the paper's "one unit per quote" and keeping the risk term scale-stable.
- **Single finite horizon.** As in the paper, `(T − t)` counts down once from the
  first event and clamps at `0` at the terminal time `T = as_horizon_s` — there is
  **no rolling reset**. The only consequence on a continuous multi-day replay is
  that the time-decay (inventory) term is active only during the first `T` seconds;
  after that the quotes are symmetric (see the results and roadmap below).

### Calibration from the data

`σ` and `k` are estimated **offline in a single pass** over the merged feed before
the run (`AvellanedaStoikov::calibrate`) and then held **constant**, exactly as the
paper estimates them from historical data — no online re-estimation.

| Param | Source |
|--|--|
| `σ²` (vol) | **Offline constant**: `σ² = Σ(ΔS)² / Σ Δt` — the variance of the mid per second over the whole feed. On the full data: `σ ≈ 2.88e-6`. |
| `k` (arrival decay) | **Offline constant**. The paper derives `λ(δ) ∝ P(impact > δ)`, so trade distances from the mid are exponential with rate `k` ⇒ `k = 1 / mean(\|trade − mid\|)`, in `1/price` units. On the full data: `k ≈ 3.99e5`. |
| `γ` (risk aversion) | User parameter (`as_gamma`) — genuinely a preference. |
| `T` (horizon) | User parameter (`as_horizon_s`). |

Both can be pinned in the config (`as_sigma` / `as_k`); a non-positive value means
"calibrate offline".

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
  Everything else (spread, inventory skew, offline σ/k) is inherited. So the
  reservation price now anticipates imbalance-driven drift *on top of* the A-S
  inventory skew — directly targeting the adverse-selection that pure replay
  otherwise ignores.

---

## 3. Avellaneda–Stoikov, online (the deployable variant)

The faithful §1 implementation is correct but impractical on a continuous
multi-day tape: with a single horizon the inventory skew dies after `T` and the
book drifts (see the results below). `AvellanedaStoikovOnline`
([`avellaneda_stoikov_online.{hpp,cpp}`](../include/bt/strategy/avellaneda_stoikov_online.hpp))
keeps the same closed form but relaxes exactly the three things that make it
deployable — a **normal market maker that holds inventory near flat**:

1. **Rolling horizon (θ rebalancing).** `(T − t)` is taken *modulo* the session
   length `T`, so it saw-tooths in `[0, T]` and **resets every session** instead of
   clamping at 0. The skew term `q·γ·σ²·(T − t)` therefore stays alive for the whole
   run — this is what actually controls inventory.
2. **Online σ and k.** Both are re-estimated continuously with EWMAs
   ([`online_estimators.hpp`](../include/bt/strategy/online_estimators.hpp)):
   `σ² = EWMA(ΔS²)/EWMA(Δt)` per second, `k = 1/EWMA(|trade − mid|)`. They are
   **seeded** from the same offline calibration as §1, then adapt to regime changes.
3. **Inventory cap + min-spread floor.** A hard `max_inventory` stops quoting the
   side that would grow the position past the limit, and the half-spread is floored
   at `as_min_half_spread` ticks — basic risk controls a live maker needs.

Config: `strategy: "as_online"` with `as_gamma`, `as_horizon_s` (rolling session),
`max_inventory`, `as_vol_alpha`, `as_k_alpha`, `as_min_half_spread`
([`configs/as_online.json`](../configs/as_online.json)). Everything else is the §1
closed form. This is the recommended strategy when the goal is a controlled book
rather than paper fidelity.

---

## 4. Performance results

Full sample dataset: **22,901,679 events** (1,036,690 book updates, 21,864,989
trades) over ~6 days. `fee_bps = 1.0`, `order_latency_us = 1000`,
`order_qty = 1000`, `max_inventory = 100,000`, optimistic queue. PnL is
mark-to-market equity from a flat start (engine is a price-taker overlay, so
absolute PnL is optimistic — see [ARCHITECTURE.md](ARCHITECTURE.md#market-model--assumptions);
the *relative* comparison is the signal). Reproduce with `make experiments`.

Calibrated constants (offline, full data): `σ ≈ 2.88e-6`, `k ≈ 3.99e5`.

### Headline (γ = 500)

| Strategy | Fills | End inventory | Turnover | Fees | Equity PnL |
|--|--:|--:|--:|--:|--:|
| Fixed-spread (baseline) | 764,349 | −88,008 | 5.43M | 543.3 | **−1525.9** |
| Avellaneda–Stoikov (faithful) | 173,921 | 478,255 | 1.28M | 127.8 | **+1486.0** |
| Micro-price + A-S (faithful) | 173,841 | 909,931 | 1.27M | 127.4 | **+1194.4** |
| A-S online (§3) | 260,181 | **−3,262** | 1.95M | 195.2 | **−396.4** |

- **A-S (faithful) vs fixed:** the volatility-aware spread quotes far less (173.9k vs
  764.3k fills, turnover and fees cut to ~¼) and PnL is positive on this rising tape.
- **The faithful inventory is large, and this is the expected consequence of
  paper-faithfulness.** With a *single* finite horizon the skew term `q·γ·σ²·(T−t)`
  is only active during the first `T = 300s`; over the remaining ~6 days `(T−t) = 0`,
  so the quotes are symmetric and inventory drifts with the trend (to +478k for A-S,
  +910k for micro-price-A-S, which leans further into the trend). The high "PnL" is
  largely that drifted long position marked against a rising tape — a directional
  bet, not market-making edge.
- **A-S online (§3) is the one to read as a market maker.** The rolling horizon and
  cap hold inventory at **−3,262** (≈3 lots of 1,000) instead of letting it run, so
  the **−396.4** PnL is the honest cost of providing liquidity on this optimistic
  price-taker overlay — comparable spread/fills profile to a real maker. Same engine,
  same calibration; only the three §3 relaxations differ.

### Risk-aversion (γ) sweep — `reports/gamma_sweep.csv` (`make sweep`)

| γ | A-S inv | A-S PnL | MP-AS inv | MP-AS PnL | online inv | online PnL |
|--:|--:|--:|--:|--:|--:|--:|
| 1 | 486,617 | +1458.5 | 918,840 | +1165.1 | 30,525 | −328.6 |
| 10 | 483,617 | +1468.4 | 912,840 | +1184.9 | −3,878 | −397.3 |
| 50 | 481,114 | +1476.6 | 910,840 | +1191.5 | −3,427 | −435.9 |
| 100 | 480,141 | +1479.8 | 908,777 | +1198.3 | −2,240 | −435.9 |
| 500 | 478,255 | +1486.0 | 909,931 | +1194.4 | −3,262 | −396.4 |
| 2000 | 480,550 | **+1919.2** | 898,325 | +1330.6 | **−126** | **−285.2** |

For the **faithful** A-S/MP-AS, `γ` has **almost no effect on ending inventory** — the
single-horizon signature: outside the first `T` seconds the skew is zero regardless of
`γ`, so all rows sit at roughly the same drifted position. For the **online** variant
`γ` is a *real* inventory control: ending inventory tightens from 30.5k (`γ = 1`) to
near-flat (`−126` at `γ = 2000`), reproducing the paper's qualitative "higher γ ⇒
tighter book", and `γ ≈ 2000` is both flattest and least-negative PnL here.

---

## 5. Improvement roadmap

**Strategy**
- **Full finite-horizon `θ`-PDE** (paper §3.1) in place of the symmetric proxy
  `r = s − qγσ²(T−t)`, giving asymmetric `δ^a ≠ δ^b` directly. (Inventory control
  across the full replay is already addressed by the §3 online variant's rolling
  horizon; the PDE is the more principled, model-internal alternative.)
- **A micro-price + online A-S combination** — the §3 rolling/online controls plus
  the §2 micro-price centre, to get a controlled book *and* the drift anticipation.
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
