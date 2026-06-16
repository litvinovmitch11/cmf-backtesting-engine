#include "bt/book/order_book.hpp"
#include "bt/core/event.hpp"
#include "bt/data/event_source.hpp"
#include "bt/engine.hpp"
#include "bt/exec.hpp"
#include "bt/metrics.hpp"
#include "bt/strategy.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <utility>
#include <vector>

using bt::Side;

namespace {
class VectorSource final : public bt::EventSource {
public:
  explicit VectorSource(std::vector<bt::MarketEvent> evs) : evs_(std::move(evs)) {}
  bool has_next() const override { return i_ < evs_.size(); }
  bt::Ts peek_ts() const override { return bt::timestamp_of(evs_[i_]); }
  bt::MarketEvent next() override { return evs_[i_++]; }

private:
  std::vector<bt::MarketEvent> evs_;
  std::size_t i_{0};
};
} // namespace

TEST_CASE("BacktestEngine wires feed->book->exec->strategy->metrics end-to-end", "[engine]") {
  // Backing book levels must outlive the run (BookUpdate holds views into them).
  const bt::BookLevel bids[1] = {{100, 5000}};
  const bt::BookLevel asks[1] = {{102, 5000}};

  // Quoter with half_spread 0 quotes both sides at mid=101 (improving the touch,
  // so queue_ahead is 0 and our quotes fill on the first crossing print).
  std::vector<bt::MarketEvent> evs;
  evs.push_back(bt::BookUpdate{.ts = 1, .book = bt::BookSnapshot{bids, asks, 1}});
  evs.push_back(bt::TradePrint{.ts = 2, .aggressor = Side::Sell, .price = 101, .amount = 100});
  evs.push_back(bt::TradePrint{.ts = 3, .aggressor = Side::Buy, .price = 101, .amount = 100});

  VectorSource feed(std::move(evs));
  bt::OrderBook book;
  bt::OptimisticQueue queue;
  bt::ExecutionSimulator exec(book, queue);
  bt::FixedSpreadQuoter strat(
      bt::QuoterParams{.half_spread = 0, .order_qty = 100, .max_inventory = 1000});
  bt::PnLTracker metrics;

  bt::BacktestEngine engine(feed, book, exec, strat, metrics);
  const bt::EngineStats st = engine.run();

  REQUIRE(st.events == 3);
  REQUIRE(st.book_updates == 1);
  REQUIRE(st.trades == 2);
  REQUIRE(st.fills == 2); // bid filled by the sell print, ask by the buy print

  const bt::PnlReport r = metrics.report();
  REQUIRE(r.fills == 2);
  REQUIRE(r.inventory == Catch::Approx(0.0)); // +100 then -100
  REQUIRE(r.turnover == Catch::Approx(2 * 100 * bt::to_price(101)));
}
