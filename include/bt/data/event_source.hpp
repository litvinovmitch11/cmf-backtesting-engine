#pragma once

#include "bt/core/event.hpp"

namespace bt {

// A chronological source of market events ("channel"). Concrete sources read
// one stream (LOB snapshots, trades, ...). FeedMerger combines several into a
// single time-ordered stream — this is the in-process replacement for pipes.
class EventSource {
public:
  EventSource() = default;
  virtual ~EventSource() = default;
  EventSource(const EventSource&) = delete;
  EventSource& operator=(const EventSource&) = delete;

  // True while events remain.
  [[nodiscard]] virtual bool has_next() const = 0;
  // Timestamp of the next event. Precondition: has_next().
  [[nodiscard]] virtual Ts peek_ts() const = 0;
  // Consume and return the next event. Precondition: has_next().
  virtual MarketEvent next() = 0;
};

} // namespace bt
