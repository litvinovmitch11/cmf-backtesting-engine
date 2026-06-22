#include "bt/book/order_book.hpp"
#include "bt/data/event_source.hpp"
#include "bt/strategy/avellaneda_stoikov.hpp"
#include "bt/strategy/microprice.hpp"
#include "bt/strategy/microprice_as.hpp"

#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <deque>
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

// Minimal in-memory LOB feed for exercising MicropriceModel::calibrate. Storage
// is held in deques so the non-owning BookSnapshot pointers stay valid.
class FakeLob final : public bt::EventSource {
public:
  void push(bt::Ticks bid_px, bt::Qty bid_q, bt::Ticks ask_px, bt::Qty ask_q, bt::Ts ts) {
    bids_.push_back({bt::BookLevel{bid_px, bid_q}});
    asks_.push_back({bt::BookLevel{ask_px, ask_q}});
    bt::BookUpdate bu;
    bu.ts = ts;
    bu.book = bt::BookSnapshot{bids_.back().data(), asks_.back().data(), 1};
    evs_.emplace_back(bu);
  }
  void push_trade(bt::Ticks px, bt::Qty amount, bt::Ts ts) {
    evs_.emplace_back(bt::TradePrint{.ts = ts, .aggressor = Side::Buy, .price = px, .amount = amount});
  }
  [[nodiscard]] bool has_next() const override { return i_ < evs_.size(); }
  [[nodiscard]] bt::Ts peek_ts() const override { return bt::timestamp_of(evs_[i_]); }
  bt::MarketEvent next() override { return evs_[i_++]; }

private:
  std::deque<std::array<bt::BookLevel, 1>> bids_;
  std::deque<std::array<bt::BookLevel, 1>> asks_;
  std::vector<bt::MarketEvent> evs_;
  std::size_t i_{0};
};

} // namespace

TEST_CASE("Avellaneda-Stoikov quotes symmetrically at zero inventory", "[strategy][as]") {
  bt::AvellanedaStoikovParams p;
  p.sigma = 1e-3; // constant vol (paper holds it fixed) -> deterministic
  p.k = 5.0;      // constant k
  p.gamma = 0.5;
  p.horizon_us = 1'000'000;
  p.order_qty = 100;
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

TEST_CASE("Avellaneda-Stoikov half-spread matches the closed form (eqs. 3.10-3.12)",
          "[strategy][as]") {
  // The symmetric test confirms the bid/ask straddle the mid; this pins the
  // *exact* optimal half-spread d = 1/2 g s^2 (T-t) + (1/g) ln(1 + g/k).
  bt::AvellanedaStoikovParams p;
  p.sigma = 1e-3; // s^2 = 1e-6
  p.k = 5.0;
  p.gamma = 0.5;
  p.horizon_us = 1'000'000; // T - t = 1.0s at the first event
  p.order_qty = 100;
  bt::AvellanedaStoikov as(p);

  const bt::BookLevel bids[1] = {{1'000'000, 500}};
  const bt::BookLevel asks[1] = {{1'000'010, 500}};
  bt::OrderBook book = make_book(bids, asks);
  RecordingApi api;

  as.on_book(book, 0, api);
  const double sigma2 = p.sigma * p.sigma;
  const double expected = 0.5 * p.gamma * sigma2 * 1.0 + (1.0 / p.gamma) * std::log1p(p.gamma / p.k);
  REQUIRE(as.last_half_spread() == Catch::Approx(expected));
  // ...and the reservation is exactly the mid at zero inventory (eq. 3.8, q=0).
  REQUIRE(as.last_reservation() == Catch::Approx(book.mid()));
}

TEST_CASE("Avellaneda-Stoikov::calibrate recovers sigma and k from the data",
          "[strategy][as][calibration]") {
  // The paper's offline calibration step:
  //   sigma^2 = sum(dMid^2)/sum(dt)   and   k = 1 / mean(|trade - mid|).
  // Construct a feed with one known mid increment and trades at known distances.
  FakeLob feed;
  // Two book updates 1s apart: mid 0.1000005 -> 0.1000015, dMid = 1e-6, dt = 1s.
  feed.push(/*bid*/ 1'000'000, 500, /*ask*/ 1'000'010, 500, /*ts*/ 0);
  feed.push(/*bid*/ 1'000'010, 500, /*ask*/ 1'000'020, 500, /*ts*/ 1'000'000);
  // Trades measured against the prevailing mid (0.1000015 = 1'000'015 ticks):
  // distances 2e-6 and 4e-6 -> mean 3e-6 -> k = 1/3e-6.
  feed.push_trade(/*px*/ 1'000'035, 10, /*ts*/ 1'500'000); // +2e-6
  feed.push_trade(/*px*/ 999'975, 10, /*ts*/ 1'600'000);   // -4e-6

  const bt::ASConstants c = bt::AvellanedaStoikov::calibrate(feed);
  REQUIRE(c.sigma == Catch::Approx(1e-6));        // sqrt(1e-12 / 1s)
  REQUIRE(c.k == Catch::Approx(1.0 / 3e-6));      // 1 / mean distance
}

TEST_CASE("MicropriceAS centers quotes on the micro-price (mid + g(I,S))",
          "[strategy][microprice][as]") {
  // The 2018 extension replaces the quote centre (the mid) with mid + g(I,S).
  // Build a model where a heavy-bid 1-tick-spread state has a known positive
  // adjustment, then confirm MicropriceAS's reservation sits exactly that far
  // above plain A-S's (both at zero inventory, so reservation == centre).
  bt::MicropriceModel model({.n_imbalance = 3, .n_spread = 1});
  const double up = 1e-6;
  const double down = -1e-6;
  for (int i = 0; i < 1000; ++i) {
    model.add_transition(0.9, 1, 0.5, 1, up, true);
    model.add_transition(0.1, 1, 0.5, 1, down, true);
    model.add_transition(0.5, 1, 0.5, 1, 0.0, false);
  }
  model.fit();
  const double adj = model.adjustment(0.9, 1);
  REQUIRE(adj > 0.0);

  bt::AvellanedaStoikovParams p;
  p.sigma = 1e-3;
  p.k = 5.0;
  p.gamma = 0.5;
  p.horizon_us = 1'000'000;
  p.order_qty = 100;

  // Heavy-bid book, 1-tick spread: imbalance 900/1000 = 0.9 -> bucket of 0.9.
  const bt::BookLevel bids[1] = {{1'000'000, 900}};
  const bt::BookLevel asks[1] = {{1'000'001, 100}};
  bt::OrderBook book = make_book(bids, asks);

  bt::AvellanedaStoikov plain(p);
  bt::MicropriceAS mp(p, std::move(model));
  RecordingApi api;
  plain.on_book(book, 0, api);
  mp.on_book(book, 0, api);

  REQUIRE(plain.last_reservation() == Catch::Approx(book.mid()));
  REQUIRE(mp.last_reservation() == Catch::Approx(book.mid() + adj));
}

TEST_CASE("Avellaneda-Stoikov skews down when long and never caps a side", "[strategy][as]") {
  bt::AvellanedaStoikovParams p;
  p.sigma = 1e-3;
  p.k = 5.0;
  p.gamma = 0.5;
  p.horizon_us = 1'000'000;
  p.order_qty = 100;
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
                      .qty = 100,
                      .maker = true});
  REQUIRE(as.inventory() == 100.0);

  as.on_book(book, 100, api);
  REQUIRE(as.last_reservation() < book.mid());       // inventory skew pulls reservation down
  REQUIRE(api.last_place(Side::Buy)->px < flat_bid); // bid backs off
  REQUIRE(api.last_place(Side::Sell) != nullptr);    // ask STILL quoted: the paper has no cap
}

TEST_CASE("Avellaneda-Stoikov horizon is single-shot: skew vanishes after T", "[strategy][as]") {
  bt::AvellanedaStoikovParams p;
  p.sigma = 1e-3;
  p.k = 5.0;
  p.gamma = 0.5;
  p.horizon_us = 1'000'000; // T = 1s
  p.order_qty = 100;
  bt::AvellanedaStoikov as(p);

  const bt::BookLevel bids[1] = {{1'000'000, 500}};
  const bt::BookLevel asks[1] = {{1'000'010, 500}};
  bt::OrderBook book = make_book(bids, asks);
  RecordingApi api;

  as.on_book(book, 0, api); // starts the session clock at t=0
  as.on_fill(bt::Fill{.ts = 1,
                      .order_id = api.last_place(Side::Buy)->id,
                      .side = Side::Buy,
                      .price = api.last_place(Side::Buy)->px,
                      .qty = 100,
                      .maker = true});

  // Well past the horizon: (T - t) clamps to 0, so the inventory term disappears
  // and the reservation collapses back to the raw mid -- the single-horizon
  // degeneration that the (relaxed) rolling horizon existed to avoid.
  as.on_book(book, 5'000'000, api); // t = 5s >> T = 1s
  REQUIRE(as.last_reservation() == Catch::Approx(book.mid()));
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

TEST_CASE("MicropriceModel::calibrate samples on the fixed grid", "[strategy][microprice]") {
  // 11 book updates, 1 microsecond apart, all spread = 1 tick.
  const auto fit = [](bt::Ts dt_us) {
    FakeLob lob;
    for (int i = 0; i < 11; ++i)
      lob.push(/*bid*/ 100 + i, 900, /*ask*/ 101 + i, 100, /*ts*/ static_cast<bt::Ts>(i));
    return bt::MicropriceModel::calibrate(
        lob, {.n_imbalance = 4, .n_spread = 4, .sample_dt_us = dt_us});
  };
  // Event-time (dt=0): one transition per consecutive pair -> 10.
  REQUIRE(fit(0).samples() == 10);
  // A 5us grid over a 10us span yields far fewer (forward-filled) samples.
  const std::size_t grid = fit(5).samples();
  REQUIRE(grid >= 1);
  REQUIRE(grid < 10);
}

TEST_CASE("MicropriceModel::calibrate drops spreads outside the modeled range",
          "[strategy][microprice]") {
  FakeLob lob;
  for (int i = 0; i < 11; ++i)
    lob.push(/*bid*/ 100, 900, /*ask*/ 110, 100, /*ts*/ static_cast<bt::Ts>(i)); // spread = 10 ticks
  const bt::MicropriceModel m =
      bt::MicropriceModel::calibrate(lob, {.n_imbalance = 4, .n_spread = 4, .sample_dt_us = 0});
  REQUIRE(m.samples() == 0);             // every transition filtered out (spread 10 > 4)
  REQUIRE(m.adjustment(0.9, 10) == 0.0); // out-of-range spread query -> mid
}
