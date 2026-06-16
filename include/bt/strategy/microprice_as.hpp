#pragma once

#include "bt/strategy/avellaneda_stoikov.hpp"
#include "bt/strategy/microprice.hpp"

#include <utility>

namespace bt {

// Avellaneda-Stoikov (2008) with the Stoikov (2018) micro-price extension: the
// quotes are centered on the calibrated micro-price (mid + g(I,S)) instead of
// the raw mid. The micro-price is a better short-horizon predictor of the mid,
// so the reservation price already anticipates imbalance-driven drift, on top of
// the A-S inventory skew. Everything else (spread, inventory control, online
// sigma/k calibration) is inherited unchanged.
class MicropriceAS final : public AvellanedaStoikov {
public:
  MicropriceAS(AvellanedaStoikovParams params, MicropriceModel model)
      : AvellanedaStoikov(params), model_(std::move(model)) {}

protected:
  [[nodiscard]] double center_price(const OrderBook& book) const override {
    const double qb = book.best_bid_qty();
    const double qa = book.best_ask_qty();
    const double denom = qb + qa;
    const double imb = (denom > 0.0) ? (qb / denom) : 0.5;
    return book.mid() + model_.adjustment(imb, book.spread());
  }

private:
  MicropriceModel model_;
};

} // namespace bt
