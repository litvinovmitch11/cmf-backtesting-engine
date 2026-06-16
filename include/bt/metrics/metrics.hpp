#pragma once

#include "bt/core/order.hpp"
#include "bt/core/types.hpp"

namespace bt {

// Observer of fills and periodic marks. Concrete trackers compute PnL,
// inventory, turnover, and (later) the time-series for the performance report.
class Metrics {
public:
  virtual ~Metrics() = default;
  virtual void on_fill(const Fill& fill) = 0;
  virtual void on_mark(Ts ts, double mid) = 0; // mark-to-market tick (mid in price units)
  virtual void finalize() = 0;
};

} // namespace bt
