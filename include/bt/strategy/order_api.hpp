#pragma once

#include "bt/core/order.hpp"
#include "bt/core/types.hpp"

namespace bt {

// The handle a Strategy uses to submit/cancel orders. The engine implements it,
// forwarding to the ExecutionSimulator with the current clock time.
class OrderApi {
public:
  virtual ~OrderApi() = default;
  virtual OrderId place(Side side, Ticks px, Qty qty) = 0;
  virtual void cancel(OrderId id) = 0;
};

} // namespace bt
