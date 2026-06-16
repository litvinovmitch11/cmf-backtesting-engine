#include "bt/core.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_CASE("price <-> ticks round-trips", "[core]") {
  constexpr double px = 0.0110436;
  const bt::Ticks t = bt::to_ticks(px);
  REQUIRE(t == 110436);
  REQUIRE(bt::to_price(t) == Catch::Approx(px));
}

TEST_CASE("opposite side flips", "[core]") {
  REQUIRE(bt::opposite(bt::Side::Buy) == bt::Side::Sell);
  REQUIRE(bt::opposite(bt::Side::Sell) == bt::Side::Buy);
}

TEST_CASE("timestamp_of visits either alternative", "[core]") {
  const bt::MarketEvent trade = bt::TradePrint{
      .ts = 42, .aggressor = bt::Side::Sell, .price = bt::to_ticks(0.011), .amount = 100.0};
  const bt::MarketEvent book = bt::BookUpdate{.ts = 7};
  REQUIRE(bt::timestamp_of(trade) == 42);
  REQUIRE(bt::timestamp_of(book) == 7);
}
