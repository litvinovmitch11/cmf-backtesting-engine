#pragma once

#include "bt/core/types.hpp"

#include <string>

namespace bt {

// Backtest configuration. Defaults are sensible for the sample crypto data;
// load() overrides any keys present in a JSON file.
struct Config {
  std::string lob_bin{"market_data/lob.bin"};
  std::string trades_bin{"market_data/trades.bin"};
  std::string report_csv{"report.csv"};

  double fee_bps{0.0};
  Ts feed_latency_us{0}; // reserved (engine applies order latency; feed latency is roadmap)
  Ts order_latency_us{0};

  Ticks half_spread{2};
  Qty order_qty{100.0};
  Qty max_inventory{1000.0};
  std::string queue_model{"optimistic"}; // "optimistic" | "proportional"

  // Load overrides from a JSON file (missing keys keep their default). Throws
  // std::exception on open/parse failure.
  static Config load(const std::string& path);
};

} // namespace bt
