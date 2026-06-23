#pragma once

#include "bt/core/types.hpp"

#include <algorithm>

namespace bt {

// Online (rolling) estimator of the mid-price volatility used by the
// Avellaneda-Stoikov online variant. A-S models the mid as Brownian motion
// S_t = s + sigma*W_t, so sigma^2 is the variance of the price *per unit time*.
// It is estimated from realised squared mid increments, normalised by the
// elapsed time between observations:
//
//     sigma2_per_sec = EWMA(dS^2) / EWMA(dt)
//
// Tracking E[dS^2] and E[dt] separately (rather than a per-sample E[dS^2/dt])
// keeps the estimate stable when book updates arrive microseconds apart — a tiny
// dt would otherwise blow up an instantaneous ratio.
class VolatilityEstimator {
public:
  explicit VolatilityEstimator(double alpha = 1e-3) : alpha_(alpha) {}

  // Feed the latest mid (price units) at time `ts` (microseconds).
  void update(double mid, Ts ts) noexcept {
    if (have_) {
      const double dprice = mid - last_mid_;
      const double dt_s = static_cast<double>(ts - last_ts_) * 1e-6;
      if (dt_s > 0.0) {
        ewma_d2_ += alpha_ * (dprice * dprice - ewma_d2_);
        ewma_dt_ += alpha_ * (dt_s - ewma_dt_);
        ready_ = true;
      }
    }
    last_mid_ = mid;
    last_ts_ = ts;
    have_ = true;
  }

  [[nodiscard]] bool ready() const noexcept { return ready_; }

  // Variance of the mid per second (price^2 / s). Zero until the first sample.
  [[nodiscard]] double sigma2_per_sec() const noexcept {
    return (ewma_dt_ > 0.0) ? (ewma_d2_ / ewma_dt_) : 0.0;
  }

private:
  double alpha_;
  double ewma_d2_{0.0};
  double ewma_dt_{0.0};
  double last_mid_{0.0};
  Ts last_ts_{0};
  bool have_{false};
  bool ready_{false};
};

// Online estimator of the order-arrival decay constant `k` in the A-S intensity
// lambda(delta) = A * exp(-k * delta). The paper derives
// lambda(delta) = Lambda * P(market-order impact > delta), so the distance a
// market order reaches from the mid is exponentially distributed with rate k.
// Hence k = 1 / E[delta], with E[delta] tracked by an EWMA. `k` comes out in
// 1/price units — exactly what (1/gamma)*ln(1+gamma/k) expects — so it is
// correctly scaled to the instrument without manual tuning.
class ArrivalRateEstimator {
public:
  // `seed_delta` (price units) is the initial mean distance, used until trades
  // arrive. `min_delta` floors a trade's distance so k stays finite.
  explicit ArrivalRateEstimator(double seed_delta, double alpha = 1e-3, double min_delta = 0.0)
      : ewma_delta_(seed_delta), alpha_(alpha), min_delta_(min_delta) {}

  // Feed a trade's distance from the prevailing mid (price units, may be 0).
  void update(double delta) noexcept {
    const double d = std::max(delta, min_delta_);
    ewma_delta_ += alpha_ * (d - ewma_delta_);
    ready_ = true;
  }

  [[nodiscard]] bool ready() const noexcept { return ready_; }

  // Arrival-decay k = 1 / E[delta] (1/price). Zero if no positive mean yet.
  [[nodiscard]] double k() const noexcept {
    return (ewma_delta_ > 0.0) ? (1.0 / ewma_delta_) : 0.0;
  }

private:
  double ewma_delta_;
  double alpha_;
  double min_delta_;
  bool ready_{false};
};

} // namespace bt
