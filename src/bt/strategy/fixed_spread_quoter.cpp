#include "bt/strategy/fixed_spread_quoter.hpp"

#include "bt/book/order_book.hpp"

namespace bt {
namespace {
constexpr Qty kEps = 1e-9;
}

void FixedSpreadQuoter::on_book(const OrderBook& book, Ts /*now*/, OrderApi& api) {
  if (!book.valid())
    return;

  const Ticks mid = (book.best_bid() + book.best_ask()) / 2;
  const Ticks target_bid = mid - p_.half_spread;
  const Ticks target_ask = mid + p_.half_spread;

  const bool want_bid = inventory_ < p_.max_inventory - kEps;  // room to buy more
  const bool want_ask = inventory_ > -p_.max_inventory + kEps; // room to sell more

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

void FixedSpreadQuoter::on_trade(const TradePrint& /*trade*/, const OrderBook& /*book*/, Ts /*now*/,
                                 OrderApi& /*api*/) {
  // Quoting is driven by book updates; nothing to do on a trade print.
}

void FixedSpreadQuoter::on_fill(const Fill& fill) {
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
