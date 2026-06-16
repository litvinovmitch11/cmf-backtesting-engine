#pragma once

#include <cmath>
#include <cstdint>

namespace bt {

// Time: microseconds since the Unix epoch (matches the data's `local_timestamp`).
using Ts = std::int64_t;

// Price as integer ticks (raw_price * kPriceScale) so the crossing/queue logic
// never compares floating-point prices for equality.
using Ticks = std::int64_t;

// Quantity / size, in the instrument's base units.
using Qty = double;

// One aggregated L2 level (price in ticks, total resting size). Fixed at 16
// bytes so binary records can be mmapped and read in place (zero-copy).
struct BookLevel {
  Ticks px{};
  Qty qty{};
};
static_assert(sizeof(BookLevel) == 16);

// Default tick scale: the sample data quotes 7 decimal places -> 1e-7 tick.
// Confirmed/overridden by the CSV->binary converter (see Milestone 2).
inline constexpr std::int64_t kPriceScale = 10'000'000;

enum class Side : std::uint8_t { Buy, Sell };

[[nodiscard]] constexpr Side opposite(Side s) noexcept {
  return s == Side::Buy ? Side::Sell : Side::Buy;
}

// Decimal price <-> integer ticks. Round-to-nearest via llround (correct for any
// sign; avoids the `(int)(x + 0.5)` rounding pitfall).
[[nodiscard]] inline Ticks to_ticks(double price) noexcept {
  return static_cast<Ticks>(std::llround(price * static_cast<double>(kPriceScale)));
}

[[nodiscard]] constexpr double to_price(Ticks t) noexcept {
  return static_cast<double>(t) / static_cast<double>(kPriceScale);
}

} // namespace bt
