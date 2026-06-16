#pragma once

#include "bt/core/types.hpp"

#include <cstdint>

namespace bt {

// On-disk binary layout for the preprocessed market data. Fixed-width, packed,
// little-endian, mmapped and read in place (see MmapFile, *BinSource).

inline constexpr std::uint32_t kLobDepth = 25;
inline constexpr std::uint32_t kBinVersion = 1;

// 8-byte magics identifying each file kind.
inline constexpr char kLobMagic[8] = {'B', 'T', 'L', 'O', 'B', '1', '\0', '\0'};
inline constexpr char kTradeMagic[8] = {'B', 'T', 'T', 'R', 'D', '1', '\0', '\0'};

struct BinHeader {
  char magic[8];
  std::uint32_t version;
  std::uint32_t depth;       // L2 levels per side (LOB only; 0 for trades)
  std::int64_t price_scale;  // kPriceScale used when ticking prices
  std::int64_t record_count; // number of fixed-width records that follow
  std::int64_t reserved;
};
static_assert(sizeof(BinHeader) == 40);

struct LobRecord {
  std::int64_t ts; // microseconds
  BookLevel bids[kLobDepth];
  BookLevel asks[kLobDepth];
};
static_assert(sizeof(LobRecord) == 8 + (2 * kLobDepth * 16)); // 808

struct TradeRecord {
  std::int64_t ts;
  std::int64_t px; // ticks
  double qty;
  std::uint8_t side; // 0 = Buy, 1 = Sell (aggressor); struct pads to 32 bytes
};
static_assert(sizeof(TradeRecord) == 32);

} // namespace bt
