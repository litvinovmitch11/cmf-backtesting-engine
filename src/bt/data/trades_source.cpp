#include "bt/data/trades_source.hpp"

#include "bt/data/binary_record.hpp"
#include "bt/data/mmap_file.hpp"

#include <cstring>
#include <stdexcept>

namespace bt {

TradesBinSource::TradesBinSource(const std::string& bin_path)
    : mmap_(std::make_unique<MmapFile>(bin_path)) {
  if (mmap_->size() < sizeof(BinHeader))
    throw std::runtime_error("TradesBinSource: file too small: " + bin_path);
  const auto* h = reinterpret_cast<const BinHeader*>(mmap_->data());
  if (std::memcmp(h->magic, kTradeMagic, sizeof(kTradeMagic)) != 0)
    throw std::runtime_error("TradesBinSource: bad magic in " + bin_path);
  count_ = static_cast<std::size_t>(h->record_count);
  records_ = reinterpret_cast<const TradeRecord*>(mmap_->data() + sizeof(BinHeader));
}

TradesBinSource::~TradesBinSource() = default;

Ts TradesBinSource::peek_ts() const {
  return records_[idx_].ts;
}

MarketEvent TradesBinSource::next() {
  const TradeRecord& r = records_[idx_++];
  return TradePrint{
      .ts = r.ts, .aggressor = static_cast<Side>(r.side), .price = r.px, .amount = r.qty};
}

} // namespace bt
