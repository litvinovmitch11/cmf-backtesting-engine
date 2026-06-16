#pragma once

#include "bt/core/event.hpp"
#include "bt/core/order.hpp"
#include "bt/exec/latency_model.hpp"
#include "bt/exec/order.hpp"

#include <cstddef>
#include <vector>

namespace bt {

class OrderBook;
class QueueModel;

// Holds the strategy's resting orders and matches them against the recorded
// market under the price-taker assumption (our orders never move the tape).
// Fills come from trade prints crossing/consuming our level (queue-aware);
// marketable orders take liquidity from the opposite book on arrival.
//
// Typical per-event wiring (see BacktestEngine):
//   on a BookUpdate: book.apply(); exec.on_book_update(); strategy reacts;
//                    exec.activate_due(now); drain take_fills()
//   on a Trade:      exec.on_trade(t); strategy reacts; exec.activate_due(now);
//                    drain take_fills()
class ExecutionSimulator {
public:
  ExecutionSimulator(const OrderBook& book, const QueueModel& queue, LatencyModel latency = {});

  // Submit a limit order; it becomes live at now + order_latency. Returns its id.
  OrderId place(Side side, Ticks px, Qty qty, Ts now);
  // Cancel a resting/in-flight order (immediate in this skeleton).
  void cancel(OrderId id);

  // Promote in-flight orders whose latency has elapsed; may fill marketable ones.
  void activate_due(Ts now);
  // Refresh queue estimates of active orders from the current book.
  void on_book_update();
  // Apply a trade print: consume queue, then fill (partial) any crossed orders.
  void on_trade(const TradePrint& trade);

  // Move out the fills generated since the last call.
  [[nodiscard]] std::vector<Fill> take_fills();

  [[nodiscard]] std::size_t live_order_count() const noexcept { return live_.size(); }
  [[nodiscard]] const std::vector<Order>& live_orders() const noexcept { return live_; }

private:
  void activate(Order& o, Ts now);
  void fill_marketable(Order& o, Ts now);
  void compact(); // drop fully-filled orders

  const OrderBook& book_;
  const QueueModel& queue_;
  LatencyModel latency_;
  std::vector<Order> live_;
  std::vector<Fill> fills_;
  OrderId next_id_{1};
};

} // namespace bt
