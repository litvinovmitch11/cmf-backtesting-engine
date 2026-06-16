#pragma once

#include "bt/core/types.hpp"

#include <cstdint>
#include <variant>

namespace bt {

// A single trade print from the tape (one row of trades.csv).
struct TradePrint {
  Ts ts{};
  Side aggressor{};
  Ticks price{};
  Qty amount{};
};

// Non-owning view of one L2 snapshot. `bids`/`asks` point into the data
// source's backing store (e.g. an mmapped file) and are valid for as long as
// that source is alive. Level [0] is the top of book.
struct BookSnapshot {
  const BookLevel* bids{nullptr};
  const BookLevel* asks{nullptr};
  std::uint32_t depth{0};
};

// A book-update event: the timestamp plus a view of the snapshot at that time.
struct BookUpdate {
  Ts ts{};
  BookSnapshot book{};
};

// The chronological stream the engine consumes is a sequence of these.
using MarketEvent = std::variant<BookUpdate, TradePrint>;

[[nodiscard]] constexpr Ts timestamp_of(const MarketEvent& ev) noexcept {
  return std::visit([](const auto& e) noexcept { return e.ts; }, ev);
}

} // namespace bt
