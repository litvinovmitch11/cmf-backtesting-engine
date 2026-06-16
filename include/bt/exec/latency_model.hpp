#pragma once

#include "bt/core/types.hpp"

namespace bt {

// Constant-latency model (microseconds). `feed_latency` (delay before the
// strategy sees an event) is applied by the engine; `order_latency` (delay
// before a placed order joins the book) is applied by the ExecutionSimulator.
struct LatencyModel {
  Ts feed_latency{0};
  Ts order_latency{0};

  [[nodiscard]] Ts order_arrival(Ts decision_ts) const noexcept {
    return decision_ts + order_latency;
  }
};

} // namespace bt
