#include "bt/book/order_book.hpp"
#include "bt/strategy/avellaneda_stoikov.hpp"
#include "bt/strategy/calibration.hpp"
#include "bt/strategy/microprice.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <vector>

using bt::Side;

namespace {

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
  // Most recent place on a side, or nullptr.
  [[nodiscard]] const ApiCall* last_place(Side side) const {
    for (auto it = calls.rbegin(); it != calls.rend(); ++it)
      if (it->kind == ApiCall::Kind::Place && it->side == side)
        return &*it;
    return nullptr;
  }
  std::vector<ApiCall> calls;
  bt::OrderId next_{1};
};

bt::OrderBook make_book(const bt::BookLevel* bids, const bt::BookLevel* asks) {
  bt::OrderBook ob;
  ob.apply(bt::BookSnapshot{bids, asks, 1});
  return ob;
}

} // namespace

TEST_CASE("VolatilityEstimator recovers a known per-second variance", "[calibration]") {
  bt::VolatilityEstimator vol(0.05);
  // Constant +0.001 price move every 1s -> dS^2 = 1e-6, dt = 1s -> sigma2 = 1e-6/s.
  double mid = 1.0;
  for (int i = 0; i < 5000; ++i) {
    mid += 0.001;
    vol.update(mid, static_cast<bt::Ts>(i + 1) * 1'000'000); // 1s steps
  }
  REQUIRE(vol.ready());
  REQUIRE(vol.sigma2_per_sec() == Catch::Approx(1e-6).epsilon(0.01));
}

TEST_CASE("ArrivalRateEstimator yields k = 1/E[delta]", "[calibration]") {
  bt::ArrivalRateEstimator arr(/*seed_delta=*/1.0, /*alpha=*/0.05, /*min_delta=*/0.0);
  for (int i = 0; i < 5000; ++i)
    arr.update(0.25); // every market order reaches 0.25 from mid
  REQUIRE(arr.ready());
  REQUIRE(arr.mean_delta() == Catch::Approx(0.25).epsilon(0.01));
  REQUIRE(arr.k() == Catch::Approx(4.0).epsilon(0.01)); // 1/0.25
}

TEST_CASE("Avellaneda-Stoikov quotes symmetrically at zero inventory", "[strategy][as]") {
  bt::AvellanedaStoikovParams p;
  p.sigma = 1e-3; // fixed vol -> deterministic
  p.k = 5.0;      // fixed k
  p.gamma = 0.5;
  p.horizon_us = 1'000'000;
  p.order_qty = 100;
  p.max_inventory = 10'000;
  bt::AvellanedaStoikov as(p);

  const bt::BookLevel bids[1] = {{1'000'000, 500}};
  const bt::BookLevel asks[1] = {{1'000'010, 500}}; // mid tick = 1'000'005
  bt::OrderBook book = make_book(bids, asks);
  RecordingApi api;

  as.on_book(book, 0, api);
  const ApiCall* bid = api.last_place(Side::Buy);
  const ApiCall* ask = api.last_place(Side::Sell);
  REQUIRE(bid != nullptr);
  REQUIRE(ask != nullptr);
  // Reservation == mid (no inventory), quotes symmetric to within rounding.
  REQUIRE(as.last_reservation() == Catch::Approx(book.mid()));
  const bt::Ticks mid_tick = 1'000'005;
  REQUIRE(std::abs((bid->px + ask->px) - 2 * mid_tick) <= 1);
  REQUIRE(ask->px > bid->px);
}

TEST_CASE("Avellaneda-Stoikov skews the reservation price down when long", "[strategy][as]") {
  bt::AvellanedaStoikovParams p;
  p.sigma = 1e-3;
  p.k = 5.0;
  p.gamma = 0.5;
  p.horizon_us = 1'000'000;
  p.order_qty = 100;
  p.max_inventory = 10'000;
  bt::AvellanedaStoikov as(p);

  const bt::BookLevel bids[1] = {{1'000'000, 500}};
  const bt::BookLevel asks[1] = {{1'000'010, 500}};
  bt::OrderBook book = make_book(bids, asks);
  RecordingApi api;

  as.on_book(book, 0, api);
  const bt::Ticks flat_bid = api.last_place(Side::Buy)->px;

  // Acquire a long position, then re-quote: reservation must fall below the mid.
  as.on_fill(bt::Fill{.ts = 1,
                      .order_id = api.last_place(Side::Buy)->id,
                      .side = Side::Buy,
                      .price = flat_bid,
                      .qty = 1000,
                      .maker = true});
  REQUIRE(as.inventory() == 1000.0);

  as.on_book(book, 100, api);
  REQUIRE(as.last_reservation() < book.mid());       // inventory skew
  REQUIRE(api.last_place(Side::Buy)->px < flat_bid); // bid pulled down to discourage buying
}

TEST_CASE("MicropriceModel learns imbalance predicts the next mid move", "[strategy][microprice]") {
  // Three imbalance bins: a heavy-bid state ticks the mid *up* into a neutral
  // (no-drift) state, a heavy-ask state ticks it *down*, and the neutral state
  // stays put. The neutral sink makes the chain mean-reverting, so the
  // micro-price series converges (B*G1 = 0).
  bt::MicropriceModel m({.n_imbalance = 3, .n_spread = 1});
  const double up = 1e-6;
  const double down = -1e-6;
  for (int i = 0; i < 1000; ++i) {
    m.add_transition(0.9, 1, 0.5, 1, up, /*moved=*/true);   // high imb -> up -> neutral
    m.add_transition(0.1, 1, 0.5, 1, down, /*moved=*/true); // low  imb -> down -> neutral
    m.add_transition(0.5, 1, 0.5, 1, 0.0, /*moved=*/false); // neutral stays
  }
  m.fit();
  REQUIRE(m.fitted());
  // High imbalance => positive adjustment; low => negative; symmetric about 0.
  REQUIRE(m.adjustment(0.9, 1) > 0.0);
  REQUIRE(m.adjustment(0.1, 1) < 0.0);
  REQUIRE(m.adjustment(0.9, 1) == Catch::Approx(-m.adjustment(0.1, 1)));
  REQUIRE(m.adjustment(0.5, 1) == Catch::Approx(0.0).margin(1e-9));
}

TEST_CASE("MicropriceModel gives ~zero adjustment when imbalance is uninformative",
          "[strategy][microprice]") {
  bt::MicropriceModel m({.n_imbalance = 2, .n_spread = 1});
  for (int i = 0; i < 1000; ++i) {
    // Same state moves up and down equally often -> no predictive content.
    m.add_transition(0.9, 1, 0.5, 1, 1e-6, true);
    m.add_transition(0.9, 1, 0.5, 1, -1e-6, true);
  }
  m.fit();
  REQUIRE(m.fitted());
  REQUIRE(m.adjustment(0.9, 1) == Catch::Approx(0.0).margin(1e-9));
}
