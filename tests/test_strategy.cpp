#include "bt/book/order_book.hpp"
#include "bt/core/event.hpp"
#include "bt/metrics.hpp"
#include "bt/strategy.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <vector>

using bt::Side;

namespace {

// Records the orders a strategy submits, assigning sequential ids like the sim.
struct ApiCall {
  enum class Kind { Place, Cancel } kind;
  Side side{};
  bt::Ticks px{};
  bt::Qty qty{};
  bt::OrderId id{};
};

class RecordingApi final : public bt::OrderApi {
public:
  bt::OrderId place(Side side, bt::Ticks px, bt::Qty qty) override {
    calls.push_back({ApiCall::Kind::Place, side, px, qty, next_});
    return next_++;
  }
  void cancel(bt::OrderId id) override { calls.push_back({ApiCall::Kind::Cancel, {}, 0, 0, id}); }
  std::vector<ApiCall> calls;
  bt::OrderId next_{1};
};

bt::OrderBook make_book(const bt::BookLevel* bids, const bt::BookLevel* asks) {
  bt::OrderBook ob;
  ob.apply(bt::BookSnapshot{bids, asks, 1});
  return ob;
}

} // namespace

TEST_CASE("FixedSpreadQuoter quotes at mid +/- half_spread and avoids churn", "[strategy]") {
  bt::FixedSpreadQuoter q(
      bt::QuoterParams{.half_spread = 1, .order_qty = 100, .max_inventory = 1000});
  const bt::BookLevel bids[1] = {{100, 500}};
  const bt::BookLevel asks[1] = {{104, 500}};
  bt::OrderBook book = make_book(bids, asks); // mid = 102
  RecordingApi api;

  q.on_book(book, 1, api);
  REQUIRE(api.calls.size() == 2);
  REQUIRE(api.calls[0].kind == ApiCall::Kind::Place);
  REQUIRE(api.calls[0].side == Side::Buy);
  REQUIRE(api.calls[0].px == 101);
  REQUIRE(api.calls[0].qty == 100.0);
  REQUIRE(api.calls[1].side == Side::Sell);
  REQUIRE(api.calls[1].px == 103);

  q.on_book(book, 2, api); // same book -> no new orders
  REQUIRE(api.calls.size() == 2);
}

TEST_CASE("FixedSpreadQuoter re-quotes a side after a full fill and tracks inventory",
          "[strategy]") {
  bt::FixedSpreadQuoter q(
      bt::QuoterParams{.half_spread = 1, .order_qty = 100, .max_inventory = 1000});
  const bt::BookLevel bids[1] = {{100, 500}};
  const bt::BookLevel asks[1] = {{104, 500}};
  bt::OrderBook book = make_book(bids, asks);
  RecordingApi api;

  q.on_book(book, 1, api);
  const bt::OrderId bid_id = api.calls[0].id;

  q.on_fill(bt::Fill{
      .ts = 2, .order_id = bid_id, .side = Side::Buy, .price = 101, .qty = 100, .maker = true});
  REQUIRE(q.inventory() == 100.0);

  const std::size_t before = api.calls.size();
  q.on_book(book, 3, api); // bid fully filled -> re-place bid; ask unchanged
  REQUIRE(api.calls.size() == before + 1);
  REQUIRE(api.calls.back().kind == ApiCall::Kind::Place);
  REQUIRE(api.calls.back().side == Side::Buy);
}

TEST_CASE("FixedSpreadQuoter stops bidding at the inventory cap", "[strategy]") {
  bt::FixedSpreadQuoter q(
      bt::QuoterParams{.half_spread = 1, .order_qty = 100, .max_inventory = 100});
  const bt::BookLevel bids[1] = {{100, 500}};
  const bt::BookLevel asks[1] = {{104, 500}};
  bt::OrderBook book = make_book(bids, asks);
  RecordingApi api;

  q.on_book(book, 1, api);
  const bt::OrderId bid_id = api.calls[0].id;
  q.on_fill(bt::Fill{
      .ts = 2, .order_id = bid_id, .side = Side::Buy, .price = 101, .qty = 100, .maker = true});
  REQUIRE(q.inventory() == 100.0); // at the long cap

  api.calls.clear();
  q.on_book(book, 3, api); // want_bid is false; ask still live -> no orders at all
  REQUIRE(api.calls.empty());
}

TEST_CASE("PnLTracker computes inventory, turnover, and mark-to-market equity", "[metrics]") {
  bt::PnLTracker m; // no fees
  m.on_fill(bt::Fill{.ts = 1,
                     .order_id = 1,
                     .side = Side::Buy,
                     .price = 1'000'000,
                     .qty = 100,
                     .maker = true}); // buy 100 @ 0.1
  m.on_fill(bt::Fill{.ts = 2,
                     .order_id = 2,
                     .side = Side::Sell,
                     .price = 1'010'000,
                     .qty = 100,
                     .maker = true}); // sell 100 @ 0.101
  m.on_mark(3, 0.101);

  const bt::PnlReport r = m.report();
  REQUIRE(r.inventory == Catch::Approx(0.0));
  REQUIRE(r.turnover == Catch::Approx(100 * 0.1 + 100 * 0.101)); // 20.1
  REQUIRE(r.equity == Catch::Approx(0.1));                       // 0.001 * 100 profit
  REQUIRE(r.fees == Catch::Approx(0.0));
  REQUIRE(r.fills == 2);
}

TEST_CASE("PnLTracker charges fees and marks open inventory", "[metrics]") {
  bt::PnLTracker m(10.0); // 10 bps
  m.on_fill(bt::Fill{.ts = 1,
                     .order_id = 1,
                     .side = Side::Buy,
                     .price = 1'000'000,
                     .qty = 100,
                     .maker = true}); // buy 100 @ 0.1, left open
  m.on_mark(2, 0.1);

  const bt::PnlReport r = m.report();
  const double fee = (100 * 0.1) * 10 * 1e-4; // 0.01
  REQUIRE(r.inventory == Catch::Approx(100.0));
  REQUIRE(r.fees == Catch::Approx(fee));
  REQUIRE(r.equity == Catch::Approx(-fee)); // cash (-10 - fee) + inv*mark (+10)
}
