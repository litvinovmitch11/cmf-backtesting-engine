#pragma once

#include "bt/data/event_source.hpp"

#include <cstddef>
#include <memory>
#include <string>

namespace bt {

class MmapFile;   // mmap held by pimpl so POSIX headers stay out of this header
struct LobRecord; // backing record type (see binary_record.hpp)

// Streams BookUpdate events from a converted LOB .bin file. The BookSnapshot in
// each event points directly into the mmapped records (valid for this object's
// lifetime), so iteration copies nothing.
class LobBinSource final : public EventSource {
public:
  explicit LobBinSource(const std::string& bin_path);
  ~LobBinSource() override;

  [[nodiscard]] bool has_next() const override { return idx_ < count_; }
  [[nodiscard]] Ts peek_ts() const override;
  MarketEvent next() override;

  [[nodiscard]] std::size_t size() const noexcept { return count_; }

private:
  std::unique_ptr<MmapFile> mmap_;
  const LobRecord* records_{nullptr};
  std::size_t count_{0};
  std::size_t idx_{0};
};

} // namespace bt
