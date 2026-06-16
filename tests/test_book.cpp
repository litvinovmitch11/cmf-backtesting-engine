#include "bt/book/order_book.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_CASE("OrderBook: top-of-book, spread, mid, microprice, size_at", "[book]") {
  bt::OrderBook ob;
  REQUIRE_FALSE(ob.valid());

  // bids descending, asks ascending; level [0] is the top of book.
  const bt::BookLevel bids[3] = {{100, 300}, {99, 50}, {98, 10}};
  const bt::BookLevel asks[3] = {{102, 100}, {103, 40}, {104, 5}};
  ob.apply(bt::BookSnapshot{bids, asks, 3});

  REQUIRE(ob.valid());
  REQUIRE(ob.depth() == 3);
  REQUIRE(ob.best_bid() == 100);
  REQUIRE(ob.best_ask() == 102);
  REQUIRE(ob.spread() == 2);
  REQUIRE(ob.best_bid_qty() == 300.0);
  REQUIRE(ob.best_ask_qty() == 100.0);

  REQUIRE(ob.size_at(bt::Side::Buy, 99) == 50.0);
  REQUIRE(ob.size_at(bt::Side::Sell, 103) == 40.0);
  REQUIRE(ob.size_at(bt::Side::Buy, 55) == 0.0); // absent level

  const double pb = bt::to_price(100);
  const double pa = bt::to_price(102);
  REQUIRE(ob.mid() == Catch::Approx(0.5 * (pb + pa)));
  // microprice = (P_b*Q_a + P_a*Q_b) / (Q_a + Q_b)
  REQUIRE(ob.microprice() == Catch::Approx((pb * 100.0 + pa * 300.0) / 400.0));
  REQUIRE(ob.microprice() > ob.mid()); // heavier bid size pushes micro above mid
}
