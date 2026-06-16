#pragma once

#include "bt/data/event_source.hpp"

#include <cstddef>
#include <memory>
#include <vector>

namespace bt {

// Merges N EventSources into one chronological stream via a binary min-heap on
// the next timestamp of each source. Ties break by source index (the order in
// which sources were added), so the merged stream is fully deterministic.
class FeedMerger final : public EventSource {
public:
  struct HeapItem {
    Ts ts;
    std::size_t idx;
  };

  // Add a channel. Call before iterating; sources are added in priority order.
  void add_source(std::unique_ptr<EventSource> src);

  [[nodiscard]] bool has_next() const override;
  [[nodiscard]] Ts peek_ts() const override;
  MarketEvent next() override;

  [[nodiscard]] std::size_t source_count() const noexcept { return sources_.size(); }

private:
  std::vector<std::unique_ptr<EventSource>> sources_;
  std::vector<HeapItem> heap_;
};

} // namespace bt
