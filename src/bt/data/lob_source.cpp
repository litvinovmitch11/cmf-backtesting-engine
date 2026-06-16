#include "bt/data/lob_source.hpp"

#include "bt/data/binary_record.hpp"
#include "bt/data/mmap_file.hpp"

#include <cstring>
#include <stdexcept>

namespace bt {

LobBinSource::LobBinSource(const std::string& bin_path)
    : mmap_(std::make_unique<MmapFile>(bin_path)) {
  if (mmap_->size() < sizeof(BinHeader))
    throw std::runtime_error("LobBinSource: file too small: " + bin_path);
  const auto* h = reinterpret_cast<const BinHeader*>(mmap_->data());
  if (std::memcmp(h->magic, kLobMagic, sizeof(kLobMagic)) != 0)
    throw std::runtime_error("LobBinSource: bad magic in " + bin_path);
  count_ = static_cast<std::size_t>(h->record_count);
  records_ = reinterpret_cast<const LobRecord*>(mmap_->data() + sizeof(BinHeader));
}

LobBinSource::~LobBinSource() = default;

Ts LobBinSource::peek_ts() const {
  return records_[idx_].ts;
}

MarketEvent LobBinSource::next() {
  const LobRecord& r = records_[idx_++];
  return BookUpdate{.ts = r.ts, .book = BookSnapshot{r.bids, r.asks, kLobDepth}};
}

} // namespace bt
