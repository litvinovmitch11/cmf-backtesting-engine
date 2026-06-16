#pragma once

#include "bt/core/types.hpp"

#include <cstdint>

namespace bt {

using OrderId = std::uint64_t;
inline constexpr OrderId kInvalidOrderId = 0;

// The result of a (possibly partial) match against a strategy order.
struct Fill {
  Ts ts{};
  OrderId order_id{};
  Side side{};
  Ticks price{};
  Qty qty{};
  bool maker{}; // true: passive fill (added liquidity); false: marketable (took liquidity)
};

} // namespace bt
