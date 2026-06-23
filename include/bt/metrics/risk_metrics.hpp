#pragma once

#include "bt/core/order.hpp"
#include "bt/core/types.hpp"
#include "bt/metrics/metrics.hpp"
#include "bt/metrics/pnl_tracker.hpp"

#include <cstdint>

namespace bt {

// Risk-adjusted summary of a run: the headline numbers a performance report needs
// beyond raw PnL. Computed by streaming the equity/inventory state at a fixed
// market-time interval (no per-row storage), plus a maker/taker fill tally.
//
// Note on the risk-adjusted return: we deliberately report **return / max
// drawdown** (a Calmar-style ratio) rather than a Sharpe ratio. The engine is a
// flat-start price-taker overlay, so its equity path is near-deterministic
// (smooth inventory mark + deterministic fees); the per-period PnL has so little
// dispersion that an annualized Sharpe explodes to meaningless magnitudes at any
// sampling interval. Max drawdown is a path statistic that stays bounded and
// interpretable, so PnL-over-drawdown is the meaningful risk-adjusted figure here.
struct RiskReport {
  double max_drawdown{0};      // worst peak-to-trough of the equity curve (PnL units)
  double return_over_maxdd{0}; // equity PnL / max drawdown (Calmar-style; 0 if no drawdown)
  double inv_mean{0};          // time-sampled inventory: mean
  double inv_std{0};           //                         std
  double inv_max_abs{0};       //                         max |inventory|
  std::int64_t maker_fills{0};
  std::int64_t taker_fills{0};
  double maker_fill_share{0}; // maker / (maker + taker) — share of passive fills
  std::int64_t samples{0};    // equity/inventory samples taken
};

// Observer that turns the engine's fill/mark stream into a RiskReport. Reads — but
// does not own — a PnLTracker (the source of truth for equity/inventory), so
// register both behind a MetricsFanout with the tracker added first; each sample
// then reflects the state for the current event. Sampling is in market time: the
// state is sampled the first time an on_mark crosses each `interval_us` boundary,
// plus a final sample at finalize().
class RiskMetrics final : public Metrics {
public:
  RiskMetrics(const PnLTracker& pnl, Ts interval_us);

  void on_fill(const Fill& fill) override;
  void on_mark(Ts ts, double mid) override;
  void finalize() override;

  [[nodiscard]] RiskReport report() const;

private:
  void sample();

  const PnLTracker& pnl_;
  Ts interval_;

  // Market-time sampling state.
  bool started_{false};
  Ts next_sample_{0};

  // Drawdown.
  bool have_peak_{false};
  double peak_eq_{0};
  double max_dd_{0};

  // Inventory distribution.
  std::int64_t n_inv_{0};
  double sum_inv_{0};
  double sum_inv2_{0};
  double max_abs_inv_{0};

  // Fill tally.
  std::int64_t maker_{0};
  std::int64_t taker_{0};
};

} // namespace bt
