#include "bt/engine/backtest_engine.hpp"

#include "bt/book/order_book.hpp"
#include "bt/core/event.hpp"
#include "bt/data/event_source.hpp"
#include "bt/exec/execution_simulator.hpp"
#include "bt/metrics/metrics.hpp"
#include "bt/strategy/strategy.hpp"

#include <variant>

namespace bt {

BacktestEngine::BacktestEngine(EventSource& feed, OrderBook& book, ExecutionSimulator& exec,
                               Strategy& strategy, Metrics& metrics)
    : feed_(feed), book_(book), exec_(exec), strat_(strategy), metrics_(metrics) {}

OrderId BacktestEngine::place(Side side, Ticks px, Qty qty) {
  return exec_.place(side, px, qty, now_);
}

void BacktestEngine::cancel(OrderId id) {
  exec_.cancel(id);
}

EngineStats BacktestEngine::run() {
  EngineStats st;
  bool first = true;

  while (feed_.has_next()) {
    const MarketEvent ev = feed_.next();
    now_ = timestamp_of(ev);
    if (first) {
      st.first_ts = now_;
      first = false;
    }
    st.last_ts = now_;

    if (const auto* bu = std::get_if<BookUpdate>(&ev)) {
      book_.apply(*bu);                   // advance the book
      exec_.on_book_update();             // refresh queue estimates from the new book
      strat_.on_book(book_, now_, *this); // strategy reacts (places/cancels via OrderApi)
      ++st.book_updates;
    } else if (const auto* tp = std::get_if<TradePrint>(&ev)) {
      exec_.on_trade(*tp); // passive fills from this print
      strat_.on_trade(*tp, book_, now_, *this);
      ++st.trades;
    }

    exec_.activate_due(now_); // promote in-flight orders (may fill marketable ones)
    for (const Fill& f : exec_.take_fills()) {
      strat_.on_fill(f);
      metrics_.on_fill(f);
      ++st.fills;
    }
    if (book_.valid())
      metrics_.on_mark(now_, book_.mid());
    ++st.events;
  }

  metrics_.finalize();
  return st;
}

} // namespace bt
