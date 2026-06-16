#pragma once

#include "bt/core/event.hpp"
#include "bt/core/types.hpp"

#include <cstdint>

namespace bt {

// Maintains the current L2 book as a zero-copy view over the latest snapshot
// (the BookSnapshot points into the data source's backing store). Trade events
// do not change the book; only BookUpdate snapshots do. Level [0] is the top.
class OrderBook {
public:
  void apply(const BookUpdate& u) noexcept { snap_ = u.book; }
  void apply(const BookSnapshot& s) noexcept { snap_ = s; }

  [[nodiscard]] bool valid() const noexcept {
    return snap_.depth > 0 && snap_.bids != nullptr && snap_.asks != nullptr;
  }
  [[nodiscard]] std::uint32_t depth() const noexcept { return snap_.depth; }
  [[nodiscard]] const BookSnapshot& snapshot() const noexcept { return snap_; }

  [[nodiscard]] Ticks best_bid() const noexcept { return snap_.bids[0].px; }
  [[nodiscard]] Ticks best_ask() const noexcept { return snap_.asks[0].px; }
  [[nodiscard]] Qty best_bid_qty() const noexcept { return snap_.bids[0].qty; }
  [[nodiscard]] Qty best_ask_qty() const noexcept { return snap_.asks[0].qty; }
  [[nodiscard]] Ticks spread() const noexcept { return best_ask() - best_bid(); }

  [[nodiscard]] double mid() const noexcept {
    return 0.5 * (to_price(best_bid()) + to_price(best_ask()));
  }

  // Imbalance-weighted mid (simple micro-price): the bid price is weighted by
  // ask size and vice versa, so heavier bid size pushes the estimate toward the
  // ask. The full Stoikov (2018) microprice estimator is on the roadmap.
  [[nodiscard]] double microprice() const noexcept {
    const double qb = best_bid_qty();
    const double qa = best_ask_qty();
    const double denom = qb + qa;
    if (denom <= 0.0)
      return mid();
    return (to_price(best_bid()) * qa + to_price(best_ask()) * qb) / denom;
  }

  // Total resting size at a given price level on a side (0 if that level is
  // absent from the visible book). Used by the queue model.
  [[nodiscard]] Qty size_at(Side side, Ticks px) const noexcept {
    const BookLevel* lv = (side == Side::Buy) ? snap_.bids : snap_.asks;
    for (std::uint32_t i = 0; i < snap_.depth; ++i) {
      if (lv[i].px == px)
        return lv[i].qty;
    }
    return 0.0;
  }

private:
  BookSnapshot snap_{};
};

} // namespace bt
