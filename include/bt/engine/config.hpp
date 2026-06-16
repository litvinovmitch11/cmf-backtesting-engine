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

  // Strategy selection: "fixed" (constant-spread stand-in), "as"
  // (Avellaneda-Stoikov 2008), or "microprice_as" (A-S + Stoikov 2018 microprice).
  std::string strategy{"fixed"};

  // Avellaneda-Stoikov parameters (used by "as" / "microprice_as").
  double as_gamma{0.3};        // risk aversion (per lot)
  double as_sigma{0.0};        // fixed vol (price/sqrt-s); <=0 => estimate online
  double as_k{0.0};            // fixed arrival decay (1/price); <=0 => estimate online
  double as_horizon_s{60.0};   // rolling session length T (seconds)
  Ticks as_min_half_spread{1}; // floor on each quote's offset from reservation (ticks)
  double as_vol_alpha{1e-3};   // EWMA smoothing for the volatility estimator
  double as_k_alpha{1e-3};     // EWMA smoothing for the arrival-rate estimator
  double as_k_seed_ticks{2.0}; // initial mean trade distance (ticks) before trades arrive

  // Microprice calibration (used by "microprice_as").
  int mp_imbalance_bins{10};
  int mp_spread_bins{4};

  // Load overrides from a JSON file (missing keys keep their default). Throws
  // std::exception on open/parse failure.
  static Config load(const std::string& path);
};

} // namespace bt
