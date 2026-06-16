#pragma once

#include "bt/core/order.hpp"
#include "bt/core/types.hpp"
#include "bt/metrics/metrics.hpp"

#include <cstdint>

namespace bt {

struct PnlReport {
  double realized_cash{0}; // signed cash from fills (negative while long)
  double inventory{0};     // signed position (base units)
  double mark_price{0};    // last mid used to mark
  double equity{0};        // cash + inventory * mark = mark-to-market PnL from a 0 start
  double turnover{0};      // cumulative traded notional
  double fees{0};
  std::int64_t fills{0};
};

// Tracks PnL, inventory, and turnover from fills, marking open inventory to the
// latest mid. `fee_bps` is charged on each fill's notional.
class PnLTracker final : public Metrics {
public:
  explicit PnLTracker(double fee_bps = 0.0) : fee_bps_(fee_bps) {}

  void on_fill(const Fill& fill) override;
  void on_mark(Ts ts, double mid) override;
  void finalize() override {}

  [[nodiscard]] PnlReport report() const;
  [[nodiscard]] double inventory() const noexcept { return inventory_; }

private:
  double fee_bps_;
  double cash_{0};
  double inventory_{0};
  double turnover_{0};
  double fees_{0};
  double last_mid_{0};
  Ts last_ts_{0};
  std::int64_t fills_{0};
};

} // namespace bt
