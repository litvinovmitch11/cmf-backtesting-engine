#include "bt/metrics/pnl_tracker.hpp"

namespace bt {

void PnLTracker::on_fill(const Fill& fill) {
  const double px = to_price(fill.price);
  const double notional = fill.qty * px;
  const double fee = notional * fee_bps_ * 1e-4;

  if (fill.side == Side::Buy) {
    cash_ -= notional;
    inventory_ += fill.qty;
  } else {
    cash_ += notional;
    inventory_ -= fill.qty;
  }
  cash_ -= fee;
  fees_ += fee;
  turnover_ += notional;
  ++fills_;
}

void PnLTracker::on_mark(Ts ts, double mid) {
  last_mid_ = mid;
  last_ts_ = ts;
}

PnlReport PnLTracker::report() const {
  PnlReport r;
  r.realized_cash = cash_;
  r.inventory = inventory_;
  r.mark_price = last_mid_;
  r.equity = cash_ + (inventory_ * last_mid_);
  r.turnover = turnover_;
  r.fees = fees_;
  r.fills = fills_;
  return r;
}

} // namespace bt
