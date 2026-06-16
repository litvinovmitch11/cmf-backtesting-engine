#pragma once

#include "bt/book/order_book.hpp"
#include "bt/core/event.hpp"
#include "bt/core/order.hpp"
#include "bt/strategy/order_api.hpp"

namespace bt {

// A trading strategy reacts to market events and emits orders/quotes via the
// OrderApi (it never returns "trades" — fills come back through on_fill).
// Avellaneda-Stoikov and the microprice variant implement this same interface.
class Strategy {
public:
  virtual ~Strategy() = default;
  virtual void on_book(const OrderBook& book, Ts now, OrderApi& api) = 0;
  virtual void on_trade(const TradePrint& trade, const OrderBook& book, Ts now, OrderApi& api) = 0;
  virtual void on_fill(const Fill& fill) = 0;
};

} // namespace bt
