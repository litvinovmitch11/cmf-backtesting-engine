#pragma once

#include "bt/core/order.hpp"
#include "bt/core/types.hpp"
#include "bt/strategy/online_estimators.hpp"
#include "bt/strategy/strategy.hpp"

namespace bt {

// Parameters for the *online* Avellaneda-Stoikov market maker (the practical,
// inventory-controlled variant — see AvellanedaStoikovOnline below).
struct ASOnlineParams {
  double gamma{0.5};          // risk aversion gamma
  Ts horizon_us{300'000'000}; // rolling session length T (theta resets each T)
  Qty order_qty{1.0};         // size per quote ("one unit")
  Qty max_inventory{100'000}; // safety cap: stop quoting the side that grows it
  Ticks min_half_spread{1};   // floor on the half-spread, in ticks

  // Online-estimator settings.
  double vol_alpha{1e-3}; // EWMA weight for the volatility estimator
  double k_alpha{1e-3};   // EWMA weight for the arrival-rate estimator
  double seed_sigma{0.0}; // initial sigma (price/sqrt-s) until vol warms up
  double seed_k{1.0};     // initial k (1/price) until trades arrive
};

// Avellaneda-Stoikov (2008) with ONLINE parameter rebalancing — a deployable
// market maker that keeps inventory controlled across a continuous multi-day
// replay, unlike the paper-faithful `AvellanedaStoikov` (single horizon, no cap).
//
// It differs from the faithful version on exactly the three points that make the
// closed form impractical on a long tape:
//
//   1. Rolling horizon (theta).  (T - t) is taken modulo the session length T,
//      so it saw-tooths between T and 0 and *resets* each session instead of
//      sticking at 0. The inventory-skew term q*gamma*sigma^2*(T-t) therefore
//      stays alive for the whole run, which is what actually controls inventory.
//   2. Online sigma and k.  Both are re-estimated continuously with EWMAs
//      (VolatilityEstimator / ArrivalRateEstimator) rather than fixed offline,
//      so the quotes adapt to regime changes. Offline values can seed them.
//   3. Inventory cap + min-spread floor.  A hard cap stops quoting the side that
//      would grow inventory past the limit, and the half-spread is floored at
//      `min_half_spread` ticks — basic risk controls a live maker needs.
//
// Reservation price and half-spread are otherwise the same closed form as the
// paper (eqs. 3.8, 3.10-3.12):
//   r = mid - q*gamma*sigma^2*(T-t),  d = 1/2 gamma sigma^2 (T-t) + 1/gamma ln(1+gamma/k).
class AvellanedaStoikovOnline final : public Strategy {
public:
  explicit AvellanedaStoikovOnline(ASOnlineParams params)
      : p_(params), vol_(params.vol_alpha),
        arr_(params.seed_k > 0.0 ? 1.0 / params.seed_k : 0.0, params.k_alpha) {}

  void on_book(const OrderBook& book, Ts now, OrderApi& api) override;
  void on_trade(const TradePrint& trade, const OrderBook& book, Ts now, OrderApi& api) override;
  void on_fill(const Fill& fill) override;

  [[nodiscard]] Qty inventory() const noexcept { return inventory_; }
  [[nodiscard]] double last_reservation() const noexcept { return last_reservation_; }
  [[nodiscard]] double last_half_spread() const noexcept { return last_half_spread_; }
  [[nodiscard]] double current_sigma() const noexcept;
  [[nodiscard]] double current_k() const noexcept;

private:
  ASOnlineParams p_;
  VolatilityEstimator vol_;
  ArrivalRateEstimator arr_;

  Qty inventory_{0};
  Ts t0_{0};
  bool started_{false};
  OrderId bid_id_{kInvalidOrderId};
  OrderId ask_id_{kInvalidOrderId};
  Ticks bid_px_{0};
  Ticks ask_px_{0};
  Qty bid_filled_{0};
  Qty ask_filled_{0};
  double last_reservation_{0.0};
  double last_half_spread_{0.0};
};

} // namespace bt
