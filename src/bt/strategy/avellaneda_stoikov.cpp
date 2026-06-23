#include "bt/strategy/avellaneda_stoikov.hpp"

#include "bt/book/order_book.hpp"
#include "bt/core/event.hpp"
#include "bt/data/event_source.hpp"

#include <algorithm>
#include <cmath>
#include <variant>

namespace bt {
namespace {
constexpr Qty kEps = 1e-9;
} // namespace

ASConstants AvellanedaStoikov::calibrate(EventSource& feed) {
  double sum_d2 = 0.0; // sum of dMid^2
  double sum_dt = 0.0; // sum of dt (seconds)
  double sum_dist = 0.0;
  long long n_trades = 0;

  bool have_mid = false;
  double last_mid = 0.0;
  Ts last_ts = 0;

  while (feed.has_next()) {
    const MarketEvent ev = feed.next();
    if (const auto* bu = std::get_if<BookUpdate>(&ev)) {
      const BookSnapshot& s = bu->book;
      if (s.depth == 0 || s.bids == nullptr || s.asks == nullptr)
        continue;
      const double mid = 0.5 * (to_price(s.bids[0].px) + to_price(s.asks[0].px));
      if (have_mid) {
        const double dt = static_cast<double>(bu->ts - last_ts) * 1e-6;
        if (dt > 0.0) {
          const double d = mid - last_mid;
          sum_d2 += d * d;
          sum_dt += dt;
        }
      }
      last_mid = mid;
      last_ts = bu->ts;
      have_mid = true;
    } else if (const auto* tp = std::get_if<TradePrint>(&ev)) {
      if (have_mid) {
        sum_dist += std::abs(to_price(tp->price) - last_mid);
        ++n_trades;
      }
    }
  }

  ASConstants c;
  c.sigma = (sum_dt > 0.0) ? std::sqrt(sum_d2 / sum_dt) : 0.0;
  const double mean_dist = (n_trades > 0) ? (sum_dist / static_cast<double>(n_trades)) : 0.0;
  c.k = (mean_dist > 0.0) ? (1.0 / mean_dist) : 0.0;
  return c;
}

double AvellanedaStoikov::center_price(const OrderBook& book) const {
  return book.mid();
}

void AvellanedaStoikov::on_book(const OrderBook& book, Ts now, OrderApi& api) {
  if (!book.valid())
    return;

  // Single finite horizon: t runs from the first event to T, then (T - t) stays
  // at 0 (the paper's terminal time). No rolling reset.
  if (!started_) {
    t0_ = now;
    started_ = true;
  }
  const double tau =
      std::max(0.0, static_cast<double>(p_.horizon_us - (now - t0_)) * 1e-6); // (T - t)

  const double center = center_price(book);   // mid (or micro-price in MicropriceAS)
  const double sigma2 = p_.sigma * p_.sigma;  // CONSTANT variance per second
  const double q = inventory_ / p_.order_qty; // signed inventory in lots

  // Reservation price (paper eq. 3.8) and optimal symmetric half-spread (3.10-3.12).
  const double reservation = center - (q * p_.gamma * sigma2 * tau);
  double half = 0.5 * p_.gamma * sigma2 * tau;
  if (p_.k > 0.0 && p_.gamma > 0.0)
    half += (1.0 / p_.gamma) * std::log1p(p_.gamma / p_.k);

  last_reservation_ = reservation;
  last_half_spread_ = half;

  // Continuous prices in the paper; the venue is tick-discrete, so round. This is
  // the only deviation from the paper — no min-spread floor, no forced widening.
  const Ticks target_bid = to_ticks(reservation - half);
  const Ticks target_ask = to_ticks(reservation + half);

  // No inventory cap: both sides are always quoted (the utility controls inventory).
  if (bid_id_ == kInvalidOrderId || target_bid != bid_px_) {
    if (bid_id_ != kInvalidOrderId)
      api.cancel(bid_id_);
    bid_id_ = api.place(Side::Buy, target_bid, p_.order_qty);
    bid_px_ = target_bid;
    bid_filled_ = 0;
  }
  if (ask_id_ == kInvalidOrderId || target_ask != ask_px_) {
    if (ask_id_ != kInvalidOrderId)
      api.cancel(ask_id_);
    ask_id_ = api.place(Side::Sell, target_ask, p_.order_qty);
    ask_px_ = target_ask;
    ask_filled_ = 0;
  }
}

void AvellanedaStoikov::on_fill(const Fill& fill) {
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
