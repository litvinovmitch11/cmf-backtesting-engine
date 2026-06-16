#include "bt/exec/execution_simulator.hpp"

#include "bt/book/order_book.hpp"
#include "bt/exec/queue_model.hpp"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace bt {
namespace {
constexpr Qty kEps = 1e-9;
}

ExecutionSimulator::ExecutionSimulator(const OrderBook& book, const QueueModel& queue,
                                       LatencyModel latency)
    : book_(book), queue_(queue), latency_(latency) {}

OrderId ExecutionSimulator::place(Side side, Ticks px, Qty qty, Ts now) {
  Order o{};
  o.id = next_id_++;
  o.side = side;
  o.px = px;
  o.qty = qty;
  o.remaining = qty;
  o.placed_ts = now;
  o.active_ts = latency_.order_arrival(now);
  o.active = false;
  live_.push_back(o);
  return o.id;
}

void ExecutionSimulator::cancel(OrderId id) {
  const auto it =
      std::find_if(live_.begin(), live_.end(), [id](const Order& o) { return o.id == id; });
  if (it != live_.end())
    live_.erase(it);
}

void ExecutionSimulator::activate_due(Ts now) {
  for (Order& o : live_) {
    if (!o.active && o.active_ts <= now)
      activate(o, now);
  }
  compact();
}

void ExecutionSimulator::activate(Order& o, Ts now) {
  o.active = true;
  if (book_.valid()) {
    const bool marketable = (o.side == Side::Buy && o.px >= book_.best_ask()) ||
                            (o.side == Side::Sell && o.px <= book_.best_bid());
    if (marketable)
      fill_marketable(o, now);
  }
  if (o.remaining > kEps) {
    o.queue_ahead = queue_.initial_queue(book_, o.side, o.px);
    o.last_level_size = o.queue_ahead;
  }
}

void ExecutionSimulator::fill_marketable(Order& o, Ts now) {
  if (!book_.valid())
    return;
  const BookSnapshot& s = book_.snapshot();
  const BookLevel* levels = (o.side == Side::Buy) ? s.asks : s.bids;
  for (std::uint32_t i = 0; i < s.depth && o.remaining > kEps; ++i) {
    const Ticks lpx = levels[i].px;
    const bool reachable = (o.side == Side::Buy) ? (lpx <= o.px) : (lpx >= o.px);
    if (!reachable)
      break; // levels are price-ordered; nothing deeper is reachable
    const Qty f = std::min(o.remaining, levels[i].qty);
    if (f <= kEps)
      continue;
    o.remaining -= f;
    fills_.push_back(
        Fill{.ts = now, .order_id = o.id, .side = o.side, .price = lpx, .qty = f, .maker = false});
  }
}

void ExecutionSimulator::on_book_update() {
  if (!book_.valid())
    return;
  for (Order& o : live_) {
    if (!o.active)
      continue;
    const Qty new_size = book_.size_at(o.side, o.px);
    queue_.on_book_change(o, o.last_level_size, new_size);
    o.last_level_size = new_size;
    o.queue_ahead = std::max(o.queue_ahead, 0.0);
  }
}

void ExecutionSimulator::on_trade(const TradePrint& trade) {
  // A sell aggressor hits resting buys at/above its price; a buy aggressor hits
  // resting sells at/below its price ("market price crossed the order level").
  std::vector<std::size_t> elig;
  for (std::size_t i = 0; i < live_.size(); ++i) {
    const Order& o = live_[i];
    if (!o.active)
      continue;
    const bool crossed =
        (o.side == Side::Buy && trade.aggressor == Side::Sell && trade.price <= o.px) ||
        (o.side == Side::Sell && trade.aggressor == Side::Buy && trade.price >= o.px);
    if (crossed)
      elig.push_back(i);
  }
  if (elig.empty())
    return;

  // Price-time priority among our own crossed orders: best price first, then id.
  std::sort(elig.begin(), elig.end(), [this](std::size_t a, std::size_t b) {
    const Order& oa = live_[a];
    const Order& ob = live_[b];
    if (oa.px != ob.px)
      return oa.side == Side::Buy ? oa.px > ob.px : oa.px < ob.px;
    return oa.id < ob.id;
  });

  Qty avail = trade.amount; // one trade's volume is shared across our orders
  for (const std::size_t idx : elig) {
    if (avail <= kEps)
      break;
    Order& o = live_[idx];

    const Qty eat = std::min(o.queue_ahead, avail); // volume ahead clears first
    o.queue_ahead -= eat;
    avail -= eat;
    o.last_level_size = std::max(0.0, o.last_level_size - eat);
    if (o.queue_ahead > kEps || avail <= kEps)
      continue;

    const Qty f = std::min(o.remaining, avail); // overflow fills us (partial ok)
    if (f <= kEps)
      continue;
    o.remaining -= f;
    avail -= f;
    fills_.push_back(Fill{
        .ts = trade.ts, .order_id = o.id, .side = o.side, .price = o.px, .qty = f, .maker = true});
  }
  compact();
}

std::vector<Fill> ExecutionSimulator::take_fills() {
  std::vector<Fill> out = std::move(fills_);
  fills_.clear();
  return out;
}

void ExecutionSimulator::compact() {
  std::erase_if(live_, [](const Order& o) { return o.remaining <= kEps; });
}

} // namespace bt
