#include "bt/book/order_book.hpp"
#include "bt/core/event.hpp"
#include "bt/exec.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using bt::Side;

TEST_CASE("Optimistic queue: a resting buy fills only after its queue clears, with partials",
          "[exec][queue]") {
  const bt::BookLevel bids[1] = {{100, 500}};
  const bt::BookLevel asks[1] = {{102, 500}};
  bt::OrderBook book;
  book.apply(bt::BookSnapshot{bids, asks, 1});
  bt::OptimisticQueue q;
  bt::ExecutionSimulator sim(book, q);

  const bt::OrderId id = sim.place(Side::Buy, 100, 300, /*now=*/0);
  sim.activate_due(0);
  REQUIRE(sim.live_order_count() == 1);
  REQUIRE(sim.live_orders()[0].queue_ahead == 500.0);

  // sell 200 @100: eats queue 500 -> 300, no fill yet
  sim.on_trade(bt::TradePrint{.ts = 1, .aggressor = Side::Sell, .price = 100, .amount = 200});
  REQUIRE(sim.take_fills().empty());
  REQUIRE(sim.live_orders()[0].queue_ahead == 300.0);

  // sell 400 @100: clears remaining 300 of queue, 100 overflow fills us
  sim.on_trade(bt::TradePrint{.ts = 2, .aggressor = Side::Sell, .price = 100, .amount = 400});
  auto fills = sim.take_fills();
  REQUIRE(fills.size() == 1);
  REQUIRE(fills[0].order_id == id);
  REQUIRE(fills[0].qty == 100.0);
  REQUIRE(fills[0].price == 100);
  REQUIRE(fills[0].maker);

  // sell 500 @100: queue empty, fills remaining 200, order done
  sim.on_trade(bt::TradePrint{.ts = 3, .aggressor = Side::Sell, .price = 100, .amount = 500});
  fills = sim.take_fills();
  REQUIRE(fills.size() == 1);
  REQUIRE(fills[0].qty == 200.0);
  REQUIRE(sim.live_order_count() == 0);
}

TEST_CASE("Improving the touch puts us first in queue (queue_ahead == 0)", "[exec][queue]") {
  const bt::BookLevel bids[1] = {{100, 500}};
  const bt::BookLevel asks[1] = {{102, 500}};
  bt::OrderBook book;
  book.apply(bt::BookSnapshot{bids, asks, 1});
  bt::OptimisticQueue q;
  bt::ExecutionSimulator sim(book, q);

  sim.place(Side::Buy, 101, 50, 0); // better than best bid 100, still inside the spread
  sim.activate_due(0);
  REQUIRE(sim.live_orders()[0].queue_ahead == 0.0);

  sim.on_trade(bt::TradePrint{.ts = 1, .aggressor = Side::Sell, .price = 101, .amount = 30});
  const auto fills = sim.take_fills();
  REQUIRE(fills.size() == 1);
  REQUIRE(fills[0].qty == 30.0);
}

TEST_CASE("Marketable order takes liquidity across levels on arrival", "[exec][marketable]") {
  const bt::BookLevel bids[2] = {{99, 100}, {98, 100}};
  const bt::BookLevel asks[2] = {{102, 100}, {103, 100}};
  bt::OrderBook book;
  book.apply(bt::BookSnapshot{bids, asks, 2});
  bt::OptimisticQueue q;
  bt::ExecutionSimulator sim(book, q);

  sim.place(Side::Buy, 103, 150, 0); // crosses asks at 102 then 103
  sim.activate_due(0);
  const auto fills = sim.take_fills();
  REQUIRE(fills.size() == 2);
  REQUIRE(fills[0].price == 102);
  REQUIRE(fills[0].qty == 100.0);
  REQUIRE_FALSE(fills[0].maker);
  REQUIRE(fills[1].price == 103);
  REQUIRE(fills[1].qty == 50.0);
  REQUIRE_FALSE(fills[1].maker);
  REQUIRE(sim.live_order_count() == 0);
}

TEST_CASE("Order latency: an order cannot fill before it arrives", "[exec][latency]") {
  const bt::BookLevel bids[1] = {{100, 0}}; // empty queue at our level
  const bt::BookLevel asks[1] = {{102, 100}};
  bt::OrderBook book;
  book.apply(bt::BookSnapshot{bids, asks, 1});
  bt::OptimisticQueue q;
  bt::ExecutionSimulator sim(book, q, bt::LatencyModel{.feed_latency = 0, .order_latency = 100});

  sim.place(Side::Buy, 100, 50, /*now=*/0); // becomes live at t=100
  sim.activate_due(50);
  REQUIRE_FALSE(sim.live_orders()[0].active);
  sim.on_trade(bt::TradePrint{.ts = 60, .aggressor = Side::Sell, .price = 100, .amount = 50});
  REQUIRE(sim.take_fills().empty()); // not live yet

  sim.activate_due(100);
  REQUIRE(sim.live_orders()[0].active);
  sim.on_trade(bt::TradePrint{.ts = 110, .aggressor = Side::Sell, .price = 100, .amount = 50});
  REQUIRE(sim.take_fills().size() == 1);
}

TEST_CASE("ProportionalQueue shrinks the queue on cancels; Optimistic leaves it", "[exec][queue]") {
  const bt::BookLevel bids0[1] = {{100, 500}};
  const bt::BookLevel asks0[1] = {{102, 500}};

  SECTION("proportional") {
    bt::OrderBook book;
    book.apply(bt::BookSnapshot{bids0, asks0, 1});
    bt::ProportionalQueue q;
    bt::ExecutionSimulator sim(book, q);
    sim.place(Side::Buy, 100, 100, 0);
    sim.activate_due(0);
    REQUIRE(sim.live_orders()[0].queue_ahead == 500.0);

    const bt::BookLevel bids1[1] = {{100, 250}}; // half the level cancelled
    book.apply(bt::BookSnapshot{bids1, asks0, 1});
    sim.on_book_update();
    REQUIRE(sim.live_orders()[0].queue_ahead == Catch::Approx(250.0));
  }

  SECTION("optimistic") {
    bt::OrderBook book;
    book.apply(bt::BookSnapshot{bids0, asks0, 1});
    bt::OptimisticQueue q;
    bt::ExecutionSimulator sim(book, q);
    sim.place(Side::Buy, 100, 100, 0);
    sim.activate_due(0);

    const bt::BookLevel bids1[1] = {{100, 250}};
    book.apply(bt::BookSnapshot{bids1, asks0, 1});
    sim.on_book_update();
    REQUIRE(sim.live_orders()[0].queue_ahead == 500.0); // unchanged
  }
}

TEST_CASE("Cancel removes a resting order so it no longer fills", "[exec]") {
  const bt::BookLevel bids[1] = {{100, 0}};
  const bt::BookLevel asks[1] = {{102, 100}};
  bt::OrderBook book;
  book.apply(bt::BookSnapshot{bids, asks, 1});
  bt::OptimisticQueue q;
  bt::ExecutionSimulator sim(book, q);

  const bt::OrderId id = sim.place(Side::Buy, 100, 50, 0);
  sim.activate_due(0);
  sim.cancel(id);
  REQUIRE(sim.live_order_count() == 0);
  sim.on_trade(bt::TradePrint{.ts = 1, .aggressor = Side::Sell, .price = 100, .amount = 50});
  REQUIRE(sim.take_fills().empty());
}
