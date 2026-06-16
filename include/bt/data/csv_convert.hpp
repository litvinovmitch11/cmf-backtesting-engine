#pragma once

#include "bt/core/types.hpp"

#include <cstdint>
#include <string>

namespace bt {

struct ConvertStats {
  std::int64_t records{0};
  // max |price * kPriceScale - nearest_integer| seen; confirms the tick scale
  // is fine enough (should be ~0 if 1e-7 captures every price exactly).
  double max_tick_round_err{0.0};
  Ts first_ts{0};
  Ts last_ts{0};
};

// One-time preprocessing: parse the vendor CSV (mmapped) and write a packed
// binary file the engine can replay quickly. Throws std::exception on I/O error.
ConvertStats convert_lob_csv(const std::string& in_csv, const std::string& out_bin);
ConvertStats convert_trades_csv(const std::string& in_csv, const std::string& out_bin);

} // namespace bt
