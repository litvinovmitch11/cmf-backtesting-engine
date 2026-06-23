#include "bt/strategy/avellaneda_stoikov_online.hpp"

#include "bt/book/order_book.hpp"

#include <algorithm>
#include <cmath>

namespace bt {
namespace {
constexpr Qty kEps = 1e-9;
} // namespace

double AvellanedaStoikovOnline::current_sigma() const noexcept {
  const double s2 = vol_.ready() ? vol_.sigma2_per_sec() : p_.seed_sigma * p_.seed_sigma;
  return std::sqrt(std::max(0.0, s2));
}

double AvellanedaStoikovOnline::current_k() const noexcept {
  return arr_.ready() ? arr_.k() : p_.seed_k;
}

void AvellanedaStoikovOnline::on_book(const OrderBook& book, Ts now, OrderApi& api) {
  if (!book.valid())
    return;

  const double mid = book.mid();
  vol_.update(mid, now); // online sigma refresh

  if (!started_) {
    t0_ = now;
    started_ = true;
  }

  // Rolling horizon: (T - t) saw-tooths in [0, T] and resets each session, so the
  // inventory-skew term never dies out (the key fix vs the single-shot horizon).
  const Ts phase = (p_.horizon_us > 0) ? ((now - t0_) % p_.horizon_us) : 0;
  const double tau = static_cast<double>(p_.horizon_us - phase) * 1e-6; // seconds

  // Online sigma^2 (seeded until the EWMA is ready) and k.
  const double sigma2 = vol_.ready() ? vol_.sigma2_per_sec() : (p_.seed_sigma * p_.seed_sigma);
  const double k = current_k();
  const double q = inventory_ / p_.order_qty; // signed inventory in lots

  // Reservation price (eq. 3.8) and optimal half-spread (eqs. 3.10-3.12).
  const double reservation = mid - (q * p_.gamma * sigma2 * tau);
  double half = 0.5 * p_.gamma * sigma2 * tau;
  if (k > 0.0 && p_.gamma > 0.0)
    half += (1.0 / p_.gamma) * std::log1p(p_.gamma / k);

  last_reservation_ = reservation;
  last_half_spread_ = half;

  // Round to the tick grid, then enforce the min-spread floor (a live-maker risk
  // control absent from the continuous-price paper).
  Ticks target_bid = to_ticks(reservation - half);
  Ticks target_ask = to_ticks(reservation + half);
  const Ticks min_half = p_.min_half_spread;
  const Ticks mid_tick = (book.best_bid() + book.best_ask()) / 2;
  if (target_ask - target_bid < 2 * min_half) {
    target_bid = std::min(target_bid, mid_tick - min_half);
    target_ask = std::max(target_ask, mid_tick + min_half);
  }

  // Inventory cap: quote a side only if it has room (same control as the fixed quoter).
  const bool want_bid = inventory_ < p_.max_inventory - kEps;
  const bool want_ask = inventory_ > -p_.max_inventory + kEps;

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

void AvellanedaStoikovOnline::on_trade(const TradePrint& trade, const OrderBook& book, Ts /*now*/,
                                       OrderApi& /*api*/) {
  // Online k: feed the trade's distance from the prevailing mid.
  if (book.valid())
    arr_.update(std::abs(to_price(trade.price) - book.mid()));
}

void AvellanedaStoikovOnline::on_fill(const Fill& fill) {
  inventory_ += (fill.side == Side::Buy) ? fill.qty : -fill.qty;

  if (fill.order_id == bid_id_) {
    bid_filled_ += fill.qty;
    if (bid_filled_ >= p_.order_qty - kEps) {
      bid_id_ = kInvalidOrderId;
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
