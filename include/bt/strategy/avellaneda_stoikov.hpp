#pragma once

#include "bt/core/order.hpp"
#include "bt/core/types.hpp"
#include "bt/strategy/strategy.hpp"

namespace bt {

class EventSource;

// Parameters for the Avellaneda-Stoikov market maker.
//
// Inventory is measured in *lots* of `order_qty`: q = inventory / order_qty.
// This keeps the risk term q*gamma*sigma^2*(T-t) well scaled regardless of the
// instrument's lot size, and matches the paper's "one unit per quote" setting.
struct AvellanedaStoikovParams {
  double gamma{0.5};         // risk aversion gamma (paper preference)
  double sigma{0.0};         // CONSTANT vol, price/sqrt-s (must be > 0 to quote)
  double k{0.0};             // CONSTANT arrival decay, 1/price (must be > 0 to quote)
  Ts horizon_us{60'000'000}; // finite horizon T, measured once from the first event
  Qty order_qty{1.0};        // "one unit" per quote
};

// Constant sigma/k fitted offline from the data (the paper's calibration step).
struct ASConstants {
  double sigma{0.0}; // price/sqrt-s
  double k{0.0};     // 1/price
};

// Avellaneda-Stoikov (2008), "High-frequency trading in a limit order book" —
// the textbook closed form, implemented faithfully:
//
//   reservation r = center - q * gamma * sigma^2 * (T - t)        (eq. 3.8)
//   half-spread d = 0.5 * gamma * sigma^2 * (T - t)
//                       + (1/gamma) * ln(1 + gamma/k)             (eqs. 3.10-3.12)
//   bid = r - d,  ask = r + d
//
// Faithful to the paper means, specifically:
//   * sigma and k are CONSTANT, calibrated once offline from the data (see
//     calibrate()) and held fixed — no online re-estimation.
//   * a SINGLE finite horizon T: (T - t) counts down once from the first event
//     and clamps at 0 (the paper's terminal time) — no rolling reset.
//   * NO inventory cap and NO min-spread floor: both sides are always quoted at
//     exactly r +/- d. The exponential-utility objective is what controls
//     inventory.
//
// The only unavoidable deviation from the continuous-price paper is rounding the
// quotes to the venue's tick grid.
//
// `center` is the mid (this class) or the micro-price (MicropriceAS, which
// overrides center_price).
class AvellanedaStoikov : public Strategy {
public:
  explicit AvellanedaStoikov(AvellanedaStoikovParams params) : p_(params) {}

  // One-shot offline calibration of the constant sigma and k from a (merged)
  // feed, exactly as the paper estimates them from historical data:
  //   sigma^2 = sum(dMid^2) / sum(dt)      (variance of the mid per second)
  //   k       = 1 / mean(|trade - mid|)    (rate of the exponential fill decay)
  // Consumes the feed; pass a fresh one for the actual run.
  static ASConstants calibrate(EventSource& feed);

  void on_book(const OrderBook& book, Ts now, OrderApi& api) override;
  // k is constant, so there is nothing to learn from a trade print.
  void on_trade(const TradePrint&, const OrderBook&, Ts, OrderApi&) override {}
  void on_fill(const Fill& fill) override;

  [[nodiscard]] Qty inventory() const noexcept { return inventory_; }
  [[nodiscard]] double last_reservation() const noexcept { return last_reservation_; }
  [[nodiscard]] double last_half_spread() const noexcept { return last_half_spread_; }

protected:
  // The price the quotes are centered on. Mid here; micro-price in MicropriceAS.
  [[nodiscard]] virtual double center_price(const OrderBook& book) const;

  AvellanedaStoikovParams p_;

private:
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
