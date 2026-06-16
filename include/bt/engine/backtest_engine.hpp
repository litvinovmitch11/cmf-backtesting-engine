#pragma once

#include "bt/core/order.hpp"
#include "bt/core/types.hpp"
#include "bt/strategy/order_api.hpp"

#include <cstdint>

namespace bt {

class EventSource;
class OrderBook;
class ExecutionSimulator;
class Strategy;
class Metrics;

struct EngineStats {
  std::int64_t events{0};
  std::int64_t book_updates{0};
  std::int64_t trades{0};
  std::int64_t fills{0};
  Ts first_ts{0};
  Ts last_ts{0};
};

// Owns the single-threaded, deterministic event loop and acts as the strategy's
// OrderApi (forwarding place/cancel to the ExecutionSimulator stamped with the
// current event time). Components are injected by reference.
class BacktestEngine final : public OrderApi {
public:
  BacktestEngine(EventSource& feed, OrderBook& book, ExecutionSimulator& exec, Strategy& strategy,
                 Metrics& metrics);

  // OrderApi — invoked by the strategy during on_book/on_trade.
  OrderId place(Side side, Ticks px, Qty qty) override;
  void cancel(OrderId id) override;

  // Replay the whole feed to completion.
  EngineStats run();

private:
  EventSource& feed_;
  OrderBook& book_;
  ExecutionSimulator& exec_;
  Strategy& strat_;
  Metrics& metrics_;
  Ts now_{0};
};

} // namespace bt
