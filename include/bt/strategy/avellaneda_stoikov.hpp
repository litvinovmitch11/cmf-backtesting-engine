#pragma once

#include "bt/core/order.hpp"
#include "bt/core/types.hpp"
#include "bt/strategy/calibration.hpp"
#include "bt/strategy/strategy.hpp"

namespace bt {

// Parameters for the Avellaneda-Stoikov market maker.
//
// Inventory is measured in *lots* of `order_qty`: q = inventory / order_qty.
// This keeps the risk term q*gamma*sigma^2*(T-t) well scaled regardless of the
// instrument's lot size, and matches the paper's "one unit per quote" setting.
struct AvellanedaStoikovParams {
  double gamma{0.3};         // risk aversion (per lot). Higher => skews harder on inventory.
  double sigma{0.0};         // fixed vol (price/sqrt-s); <=0 => estimate online (recommended).
  double k{0.0};             // fixed arrival decay (1/price); <=0 => estimate online.
  Ts horizon_us{60'000'000}; // session length T; (T-t) resets each session (rolling horizon).

  Qty order_qty{1000.0};       // size per quote (one "lot").
  Qty max_inventory{100000.0}; // hard position cap.
  Ticks min_half_spread{1};    // floor on each quote's offset from the reservation price (ticks).

  double vol_alpha{1e-3};   // EWMA smoothing for the volatility estimator.
  double k_alpha{1e-3};     // EWMA smoothing for the arrival-rate estimator.
  double k_seed_ticks{2.0}; // initial mean trade distance (ticks) before trades arrive.
};

// Avellaneda-Stoikov (2008), "High-frequency trading in a limit order book".
//
// Two-step quoting around an inventory-adjusted *reservation price*:
//
//   reservation r = center - q * gamma * sigma^2 * (T - t)
//   half-spread  d = 0.5 * gamma * sigma^2 * (T - t) + (1/gamma) * ln(1 + gamma/k)
//   bid = r - d,  ask = r + d
//
// where `center` is the mid (this class) or the microprice (MicropriceAS, which
// overrides center_price). sigma and k are calibrated from the data online.
//
// The reservation price leans away from the mid in proportion to inventory, so
// the maker is more eager to unwind risk than to add to it — the inventory
// control the constant-spread quoter lacks.
class AvellanedaStoikov : public Strategy {
public:
  explicit AvellanedaStoikov(AvellanedaStoikovParams params);

  void on_book(const OrderBook& book, Ts now, OrderApi& api) override;
  void on_trade(const TradePrint& trade, const OrderBook& book, Ts now, OrderApi& api) override;
  void on_fill(const Fill& fill) override;

  [[nodiscard]] Qty inventory() const noexcept { return inventory_; }
  [[nodiscard]] double last_reservation() const noexcept { return last_reservation_; }
  [[nodiscard]] double last_half_spread() const noexcept { return last_half_spread_; }

protected:
  // The price the quotes are centered on. Mid here; microprice in MicropriceAS.
  [[nodiscard]] virtual double center_price(const OrderBook& book) const;

  AvellanedaStoikovParams p_;

private:
  // Seconds remaining in the current rolling session; resets when it hits 0.
  [[nodiscard]] double time_to_horizon(Ts now) noexcept;
  void requote(const OrderBook& book, Ts now, OrderApi& api);

  VolatilityEstimator vol_;
  ArrivalRateEstimator arr_;

  Qty inventory_{0};
  OrderId bid_id_{kInvalidOrderId};
  OrderId ask_id_{kInvalidOrderId};
  Ticks bid_px_{0};
  Ticks ask_px_{0};
  Qty bid_filled_{0};
  Qty ask_filled_{0};

  Ts session_start_{0};
  bool session_started_{false};
  double last_reservation_{0.0};
  double last_half_spread_{0.0};
};

} // namespace bt
