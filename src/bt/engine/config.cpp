#include "bt/engine/config.hpp"

#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace bt {

Config Config::load(const std::string& path) {
  std::ifstream is(path);
  if (!is)
    throw std::runtime_error("Config: cannot open " + path);

  nlohmann::json j;
  is >> j;

  Config c;
  c.lob_bin = j.value("lob_bin", c.lob_bin);
  c.trades_bin = j.value("trades_bin", c.trades_bin);
  c.report_csv = j.value("report_csv", c.report_csv);
  c.series_csv = j.value("series_csv", c.series_csv);
  c.series_interval_ms = j.value("series_interval_ms", c.series_interval_ms);
  c.fee_bps = j.value("fee_bps", c.fee_bps);
  c.feed_latency_us = j.value("feed_latency_us", c.feed_latency_us);
  c.order_latency_us = j.value("order_latency_us", c.order_latency_us);
  c.half_spread = j.value("half_spread", c.half_spread);
  c.order_qty = j.value("order_qty", c.order_qty);
  c.max_inventory = j.value("max_inventory", c.max_inventory);
  c.queue_model = j.value("queue_model", c.queue_model);

  c.strategy = j.value("strategy", c.strategy);
  c.as_gamma = j.value("as_gamma", c.as_gamma);
  c.as_sigma = j.value("as_sigma", c.as_sigma);
  c.as_k = j.value("as_k", c.as_k);
  c.as_horizon_s = j.value("as_horizon_s", c.as_horizon_s);
  c.as_vol_alpha = j.value("as_vol_alpha", c.as_vol_alpha);
  c.as_k_alpha = j.value("as_k_alpha", c.as_k_alpha);
  c.as_min_half_spread = j.value("as_min_half_spread", c.as_min_half_spread);
  c.mp_imbalance_bins = j.value("mp_imbalance_bins", c.mp_imbalance_bins);
  c.mp_spread_bins = j.value("mp_spread_bins", c.mp_spread_bins);
  c.mp_sample_dt_us = j.value("mp_sample_dt_us", c.mp_sample_dt_us);
  return c;
}

} // namespace bt
