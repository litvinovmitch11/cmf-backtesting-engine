#pragma once

#include "bt/core/order.hpp"
#include "bt/core/types.hpp"

namespace bt {

// A live limit order tracked by the ExecutionSimulator. This is the strategy's
// private overlay on the recorded market — it never appears in the tape.
struct Order {
  OrderId id{};
  Side side{};
  Ticks px{};
  Qty qty{};             // original size
  Qty remaining{};       // unfilled size
  Qty queue_ahead{};     // estimated volume ahead of us at our level (FIFO)
  Qty last_level_size{}; // visible size at our level at last observation
  Ts placed_ts{};
  Ts active_ts{};     // becomes live at this time (placed_ts + order latency)
  bool active{false}; // false while still in flight (latency)
};

} // namespace bt
