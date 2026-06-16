#pragma once

#include "bt/core/order.hpp"
#include "bt/core/types.hpp"
#include "bt/strategy/strategy.hpp"

namespace bt {

struct QuoterParams {
  Ticks half_spread{2};    // quote offset from mid, in ticks
  Qty order_qty{100};      // size per quote
  Qty max_inventory{1000}; // hard position cap (stop quoting the side that grows it)
};

// Symmetric constant-spread market maker: maintains one bid and one ask at
// mid +/- half_spread, re-quoting only when the target price moves (no churn),
// and stops quoting a side at the inventory cap. A stand-in to exercise the
// whole pipeline; Avellaneda-Stoikov plugs into this same interface, changing
// only how the center and half-spread are computed.
class FixedSpreadQuoter final : public Strategy {
public:
  explicit FixedSpreadQuoter(QuoterParams params) : p_(params) {}

  void on_book(const OrderBook& book, Ts now, OrderApi& api) override;
  void on_trade(const TradePrint& trade, const OrderBook& book, Ts now, OrderApi& api) override;
  void on_fill(const Fill& fill) override;

  [[nodiscard]] Qty inventory() const noexcept { return inventory_; }

private:
  QuoterParams p_;
  Qty inventory_{0};
  OrderId bid_id_{kInvalidOrderId};
  OrderId ask_id_{kInvalidOrderId};
  Ticks bid_px_{0};
  Ticks ask_px_{0};
  Qty bid_filled_{0};
  Qty ask_filled_{0};
};

} // namespace bt
