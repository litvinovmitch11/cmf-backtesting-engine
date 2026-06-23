#pragma once

#include "bt/core/order.hpp"
#include "bt/core/types.hpp"
#include "bt/metrics/metrics.hpp"
#include "bt/metrics/pnl_tracker.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace bt {

// Samples the PnL/inventory/price state at a fixed market-time interval and
// writes a tidy per-row CSV for offline plotting (price, equity, inventory,
// turnover, traded volume). It reads — but does not own — a PnLTracker, so
// register both behind a MetricsFanout with the tracker added first; each
// snapshot then reflects the mark for the current event.
//
// Sampling is in market time: a row is emitted the first time an `on_mark`
// crosses the next `interval_us` boundary, plus a final flush row at
// finalize(). Between samples it accumulates per-bucket fill volume so the
// volume series is a faithful sum rather than a subsample.
class TimeSeriesRecorder final : public Metrics {
public:
  TimeSeriesRecorder(const PnLTracker& pnl, std::string path, Ts interval_us);

  void on_fill(const Fill& fill) override;
  void on_mark(Ts ts, double mid) override;
  void finalize() override;

  [[nodiscard]] std::size_t rows() const noexcept { return rows_.size(); }

private:
  struct Row {
    Ts ts{0};
    double mid{0};
    double inventory{0};
    double equity{0};
    double realized_cash{0};
    double turnover{0};
    double fees{0};
    std::int64_t fills{0};        // cumulative fills
    double bucket_qty{0};         // strategy volume traded since the previous row
    std::int64_t bucket_fills{0}; // fill count since the previous row
  };

  void snapshot(Ts ts, double mid);
  void write() const;

  const PnLTracker& pnl_;
  std::string path_;
  Ts interval_;
  Ts next_sample_{0};
  Ts last_ts_{0};
  double last_mid_{0};
  bool started_{false};
  double bucket_qty_{0};
  std::int64_t bucket_fills_{0};
  std::vector<Row> rows_;
};

} // namespace bt
