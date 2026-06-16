#pragma once

#include "bt/book/order_book.hpp"
#include "bt/exec/order.hpp"

namespace bt {

// Estimates our FIFO position in the queue at a price level (we only have
// aggregated L2 size, not per-order identities, so position is an estimate).
class QueueModel {
public:
  virtual ~QueueModel() = default;

  // Volume ahead of us when a new order joins `side` at `px`: the size already
  // resting there. Common to both models (we always join at the back).
  [[nodiscard]] Qty initial_queue(const OrderBook& book, Side side, Ticks px) const noexcept {
    return book.size_at(side, px);
  }

  // Adjust queue_ahead when the visible size at our level changes between
  // snapshots (cancels shrink it, new orders join behind us).
  virtual void on_book_change(Order& o, Qty old_size, Qty new_size) const noexcept = 0;
};

// Cancels are assumed to occur BEHIND us, so only trades reduce our queue.
// Gives an upper bound on fills (most optimistic).
class OptimisticQueue final : public QueueModel {
public:
  void on_book_change(Order& /*o*/, Qty /*old_size*/, Qty /*new_size*/) const noexcept override {}
};

// Cancels are spread proportionally across the queue, so a drop in visible size
// shrinks our position too. More conservative (looser lower bound) on fills.
class ProportionalQueue final : public QueueModel {
public:
  void on_book_change(Order& o, Qty old_size, Qty new_size) const noexcept override {
    if (new_size < old_size && old_size > 0.0) {
      o.queue_ahead *= new_size / old_size;
    }
  }
};

} // namespace bt
