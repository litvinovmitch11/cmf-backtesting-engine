#pragma once

#include "bt/strategy/avellaneda_stoikov_online.hpp"
#include "bt/strategy/microprice.hpp"

#include <utility>

namespace bt {

// The "best of both" market maker: the deployable online Avellaneda-Stoikov
// controls (rolling horizon + online sigma/k + inventory cap + min-spread floor)
// with the Stoikov (2018) micro-price as the quote centre.
//
//   * From AvellanedaStoikovOnline: inventory stays near flat across a multi-day
//     replay (rolling theta keeps the skew alive; the cap is a backstop).
//   * From the micro-price: the quote centre is mid + g(I,S) instead of the raw
//     mid, so the reservation price anticipates imbalance-driven drift on top of
//     the inventory skew.
//
// So it targets a controlled book *and* adverse-selection at once — exactly the
// combination the faithful MicropriceAS (uncontrolled inventory) and plain
// AvellanedaStoikovOnline (mid-centred) each miss.
class MicropriceASOnline final : public AvellanedaStoikovOnline {
public:
  MicropriceASOnline(ASOnlineParams params, MicropriceModel model)
      : AvellanedaStoikovOnline(params), model_(std::move(model)) {}

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
