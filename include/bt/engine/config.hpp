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
  double as_gamma{0.5};      // risk aversion gamma
  double as_sigma{0.0};      // constant vol (price/sqrt-s); <=0 => calibrate offline
  double as_k{0.0};          // constant arrival decay (1/price); <=0 => calibrate offline
  double as_horizon_s{60.0}; // finite horizon T (seconds): single-shot for "as", rolling for "as_online"

  // Online A-S extra knobs (used by "as_online"): sigma/k are re-estimated online
  // with these EWMA weights and seeded from the offline calibration.
  double as_vol_alpha{1e-3};       // EWMA weight for the online volatility estimator
  double as_k_alpha{1e-3};         // EWMA weight for the online arrival-rate estimator
  Ticks as_min_half_spread{1};     // floor on the half-spread, in ticks

  // Microprice calibration (used by "microprice_as").
  int mp_imbalance_bins{10};
  int mp_spread_bins{4};
  Ts mp_sample_dt_us{1'000'000}; // discrete-time chain grid (paper ~1s); 0 => event-time

  // Load overrides from a JSON file (missing keys keep their default). Throws
  // std::exception on open/parse failure.
  static Config load(const std::string& path);
};

} // namespace bt
