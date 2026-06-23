#include "bt/metrics/risk_metrics.hpp"

#include <algorithm>
#include <cmath>

namespace bt {

RiskMetrics::RiskMetrics(const PnLTracker& pnl, Ts interval_us)
    : pnl_(pnl), interval_(interval_us > 0 ? interval_us : 1) {}

void RiskMetrics::on_fill(const Fill& fill) {
  if (fill.maker)
    ++maker_;
  else
    ++taker_;
}

void RiskMetrics::on_mark(Ts ts, double /*mid*/) {
  if (!started_) {
    started_ = true;
    next_sample_ = ts + interval_;
    sample(); // opening sample
    return;
  }
  if (ts >= next_sample_) {
    sample();
    do {
      next_sample_ += interval_;
    } while (ts >= next_sample_);
  }
}

void RiskMetrics::finalize() {
  if (started_)
    sample(); // capture the final state
}

void RiskMetrics::sample() {
  const PnlReport r = pnl_.report();
  const double eq = r.equity;
  const double inv = r.inventory;

  // Running peak -> max drawdown.
  if (!have_peak_ || eq > peak_eq_) {
    peak_eq_ = eq;
    have_peak_ = true;
  }
  max_dd_ = std::max(max_dd_, peak_eq_ - eq);

  // Inventory distribution.
  sum_inv_ += inv;
  sum_inv2_ += inv * inv;
  max_abs_inv_ = std::max(max_abs_inv_, std::abs(inv));
  ++n_inv_;
}

RiskReport RiskMetrics::report() const {
  RiskReport r;
  r.max_drawdown = max_dd_;
  r.return_over_maxdd = (max_dd_ > 0.0) ? (pnl_.report().equity / max_dd_) : 0.0;

  if (n_inv_ > 0) {
    const double m = sum_inv_ / static_cast<double>(n_inv_);
    const double v = std::max(0.0, (sum_inv2_ / static_cast<double>(n_inv_)) - (m * m));
    r.inv_mean = m;
    r.inv_std = std::sqrt(v);
  }
  r.inv_max_abs = max_abs_inv_;

  r.maker_fills = maker_;
  r.taker_fills = taker_;
  const std::int64_t total = maker_ + taker_;
  r.maker_fill_share = (total > 0) ? static_cast<double>(maker_) / static_cast<double>(total) : 0.0;
  r.samples = n_inv_;
  return r;
}

} // namespace bt
