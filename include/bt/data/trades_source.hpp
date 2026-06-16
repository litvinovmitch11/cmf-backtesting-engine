#pragma once

#include "bt/data/event_source.hpp"

#include <cstddef>
#include <memory>
#include <string>

namespace bt {

class MmapFile;     // pimpl (keeps POSIX headers out of this header)
struct TradeRecord; // backing record type (see binary_record.hpp)

// Streams TradePrint events from a converted trades .bin file.
class TradesBinSource final : public EventSource {
public:
  explicit TradesBinSource(const std::string& bin_path);
  ~TradesBinSource() override;

  [[nodiscard]] bool has_next() const override { return idx_ < count_; }
  [[nodiscard]] Ts peek_ts() const override;
  MarketEvent next() override;

  [[nodiscard]] std::size_t size() const noexcept { return count_; }

private:
  std::unique_ptr<MmapFile> mmap_;
  const TradeRecord* records_{nullptr};
  std::size_t count_{0};
  std::size_t idx_{0};
};

} // namespace bt
