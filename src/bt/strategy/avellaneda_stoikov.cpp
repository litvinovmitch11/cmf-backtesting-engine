#include "bt/strategy/avellaneda_stoikov.hpp"

#include "bt/book/order_book.hpp"

#include <algorithm>
#include <cmath>

namespace bt {
namespace {
constexpr Qty kEps = 1e-9;
} // namespace

AvellanedaStoikov::AvellanedaStoikov(AvellanedaStoikovParams params)
    : p_(params), vol_(params.vol_alpha),
      arr_(params.k_seed_ticks * to_price(1), params.k_alpha, to_price(1) * 0.5) {}

double AvellanedaStoikov::center_price(const OrderBook& book) const {
  return book.mid();
}

double AvellanedaStoikov::time_to_horizon(Ts now) noexcept {
  if (!session_started_) {
    session_start_ = now;
    session_started_ = true;
  }
  double remaining_s = static_cast<double>(p_.horizon_us - (now - session_start_)) * 1e-6;
  if (remaining_s <= 0.0) {
    session_start_ = now; // roll into a fresh session
    remaining_s = static_cast<double>(p_.horizon_us) * 1e-6;
  }
  return remaining_s;
}

void AvellanedaStoikov::on_book(const OrderBook& book, Ts now, OrderApi& api) {
  if (!book.valid())
    return;
  vol_.update(book.mid(), now);
  requote(book, now, api);
}

void AvellanedaStoikov::on_trade(const TradePrint& trade, const OrderBook& book, Ts /*now*/,
                                 OrderApi& /*api*/) {
  if (!book.valid())
    return;
  // Distance the market order reached from the mid -> feeds the k estimator.
  arr_.update(std::abs(to_price(trade.price) - book.mid()));
}

void AvellanedaStoikov::requote(const OrderBook& book, Ts now, OrderApi& api) {
  const double tau = time_to_horizon(now);
  const double sigma2 = (p_.sigma > 0.0) ? (p_.sigma * p_.sigma) : vol_.sigma2_per_sec();
  const double k = (p_.k > 0.0) ? p_.k : arr_.k();
  const double q = inventory_ / p_.order_qty; // signed inventory in lots

  const double center = center_price(book);
  const double risk = q * p_.gamma * sigma2 * tau; // inventory skew
  const double reservation = center - risk;        // r = s - q*gamma*sigma^2*(T-t)
  double half = 0.5 * p_.gamma * sigma2 * tau;     // first (inventory/vol) term
  if (k > 0.0 && p_.gamma > 0.0)
    half += (1.0 / p_.gamma) * std::log1p(p_.gamma / k); // (1/gamma)*ln(1+gamma/k)

  last_reservation_ = reservation;
  last_half_spread_ = half;

  const double min_half = to_price(p_.min_half_spread);
  half = std::max(half, min_half);

  Ticks target_bid = to_ticks(reservation - half);
  Ticks target_ask = to_ticks(reservation + half);
  // Guarantee a strictly positive spread of at least the configured floor.
  if (target_ask - target_bid < 2 * p_.min_half_spread) {
    const Ticks center_tick = to_ticks(reservation);
    target_bid = center_tick - p_.min_half_spread;
    target_ask = center_tick + p_.min_half_spread;
  }

  const bool want_bid = inventory_ < p_.max_inventory - kEps;  // room to buy
  const bool want_ask = inventory_ > -p_.max_inventory + kEps; // room to sell

  if (want_bid) {
    if (bid_id_ == kInvalidOrderId || target_bid != bid_px_) {
      if (bid_id_ != kInvalidOrderId)
        api.cancel(bid_id_);
      bid_id_ = api.place(Side::Buy, target_bid, p_.order_qty);
      bid_px_ = target_bid;
      bid_filled_ = 0;
    }
  } else if (bid_id_ != kInvalidOrderId) {
    api.cancel(bid_id_);
    bid_id_ = kInvalidOrderId;
  }

  if (want_ask) {
    if (ask_id_ == kInvalidOrderId || target_ask != ask_px_) {
      if (ask_id_ != kInvalidOrderId)
        api.cancel(ask_id_);
      ask_id_ = api.place(Side::Sell, target_ask, p_.order_qty);
      ask_px_ = target_ask;
      ask_filled_ = 0;
    }
  } else if (ask_id_ != kInvalidOrderId) {
    api.cancel(ask_id_);
    ask_id_ = kInvalidOrderId;
  }
}

void AvellanedaStoikov::on_fill(const Fill& fill) {
  inventory_ += (fill.side == Side::Buy) ? fill.qty : -fill.qty;

  if (fill.order_id == bid_id_) {
    bid_filled_ += fill.qty;
    if (bid_filled_ >= p_.order_qty - kEps) {
      bid_id_ = kInvalidOrderId; // fully filled -> requote next book update
      bid_filled_ = 0;
    }
  } else if (fill.order_id == ask_id_) {
    ask_filled_ += fill.qty;
    if (ask_filled_ >= p_.order_qty - kEps) {
      ask_id_ = kInvalidOrderId;
      ask_filled_ = 0;
    }
  }
}

} // namespace bt
