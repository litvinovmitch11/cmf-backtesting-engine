#include "bt/data/feed_merger.hpp"

#include <algorithm>
#include <utility>

namespace bt {
namespace {

struct HeapCmp {
  bool operator()(const FeedMerger::HeapItem& a, const FeedMerger::HeapItem& b) const noexcept {
    // greater-than comparator => std::*_heap behave as a MIN-heap on ts;
    // lower source index wins on ts ties, making the merge deterministic.
    return a.ts != b.ts ? a.ts > b.ts : a.idx > b.idx;
  }
};

} // namespace

void FeedMerger::add_source(std::unique_ptr<EventSource> src) {
  const std::size_t idx = sources_.size();
  sources_.push_back(std::move(src));
  if (sources_[idx]->has_next()) {
    heap_.push_back(HeapItem{sources_[idx]->peek_ts(), idx});
    std::push_heap(heap_.begin(), heap_.end(), HeapCmp{});
  }
}

bool FeedMerger::has_next() const {
  return !heap_.empty();
}

Ts FeedMerger::peek_ts() const {
  return heap_.front().ts;
}

MarketEvent FeedMerger::next() {
  std::pop_heap(heap_.begin(), heap_.end(), HeapCmp{});
  const HeapItem item = heap_.back();
  heap_.pop_back();

  MarketEvent ev = sources_[item.idx]->next();
  if (sources_[item.idx]->has_next()) {
    heap_.push_back(HeapItem{sources_[item.idx]->peek_ts(), item.idx});
    std::push_heap(heap_.begin(), heap_.end(), HeapCmp{});
  }
  return ev;
}

} // namespace bt
