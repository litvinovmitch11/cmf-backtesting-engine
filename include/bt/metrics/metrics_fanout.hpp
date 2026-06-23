#pragma once

#include "bt/metrics/metrics.hpp"

#include <vector>

namespace bt {

// Fans every metrics callback out to several observers in registration order, so
// the engine (which holds a single Metrics&) can drive a PnLTracker and a
// TimeSeriesRecorder at once. Add the source-of-truth tracker first: later
// observers that read from it (e.g. the recorder snapshotting equity) then see
// the state already updated for the current event.
class MetricsFanout final : public Metrics {
public:
  void add(Metrics& observer) { observers_.push_back(&observer); }

  void on_fill(const Fill& fill) override {
    for (Metrics* m : observers_)
      m->on_fill(fill);
  }
  void on_mark(Ts ts, double mid) override {
    for (Metrics* m : observers_)
      m->on_mark(ts, mid);
  }
  void finalize() override {
    for (Metrics* m : observers_)
      m->finalize();
  }

private:
  std::vector<Metrics*> observers_;
};

} // namespace bt
