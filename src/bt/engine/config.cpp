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
  c.fee_bps = j.value("fee_bps", c.fee_bps);
  c.feed_latency_us = j.value("feed_latency_us", c.feed_latency_us);
  c.order_latency_us = j.value("order_latency_us", c.order_latency_us);
  c.half_spread = j.value("half_spread", c.half_spread);
  c.order_qty = j.value("order_qty", c.order_qty);
  c.max_inventory = j.value("max_inventory", c.max_inventory);
  c.queue_model = j.value("queue_model", c.queue_model);
  return c;
}

} // namespace bt
