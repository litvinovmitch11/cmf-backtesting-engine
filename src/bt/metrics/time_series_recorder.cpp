#include "bt/metrics/time_series_recorder.hpp"

#include <fstream>
#include <utility>

namespace bt {

TimeSeriesRecorder::TimeSeriesRecorder(const PnLTracker& pnl, std::string path, Ts interval_us)
    : pnl_(pnl), path_(std::move(path)), interval_(interval_us > 0 ? interval_us : 1) {}

void TimeSeriesRecorder::on_fill(const Fill& fill) {
  bucket_qty_ += fill.qty;
  ++bucket_fills_;
}

void TimeSeriesRecorder::on_mark(Ts ts, double mid) {
  last_ts_ = ts;
  last_mid_ = mid;
  if (!started_) {
    started_ = true;
    next_sample_ = ts + interval_;
    snapshot(ts, mid); // opening row
    return;
  }
  if (ts >= next_sample_) {
    snapshot(ts, mid);
    // Skip whole intervals with no marks rather than emitting empty rows.
    do {
      next_sample_ += interval_;
    } while (ts >= next_sample_);
  }
}

void TimeSeriesRecorder::finalize() {
  // A closing row so fills/marks after the last sampled boundary aren't lost.
  if (started_ && bucket_fills_ > 0)
    snapshot(last_ts_, last_mid_);
  write();
}

void TimeSeriesRecorder::snapshot(Ts ts, double mid) {
  const PnlReport r = pnl_.report();
  rows_.push_back(Row{
      .ts = ts,
      .mid = mid,
      .inventory = r.inventory,
      .equity = r.equity,
      .realized_cash = r.realized_cash,
      .turnover = r.turnover,
      .fees = r.fees,
      .fills = r.fills,
      .bucket_qty = bucket_qty_,
      .bucket_fills = bucket_fills_,
  });
  bucket_qty_ = 0;
  bucket_fills_ = 0;
}

void TimeSeriesRecorder::write() const {
  std::ofstream os(path_);
  os << "ts,mid,inventory,equity,realized_cash,turnover,fees,fills,bucket_qty,bucket_fills\n";
  for (const Row& r : rows_) {
    os << r.ts << ',' << r.mid << ',' << r.inventory << ',' << r.equity << ',' << r.realized_cash
       << ',' << r.turnover << ',' << r.fees << ',' << r.fills << ',' << r.bucket_qty << ','
       << r.bucket_fills << '\n';
  }
}

} // namespace bt
