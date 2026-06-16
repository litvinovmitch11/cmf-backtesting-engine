#pragma once

#include <cstddef>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace bt {

// RAII read-only memory map of a whole file. Used by the CSV converter (input)
// and the binary event sources (mmap the .bin and read records in place).
class MmapFile {
public:
  explicit MmapFile(const std::string& path) {
    fd_ = ::open(path.c_str(), O_RDONLY);
    if (fd_ < 0)
      throw std::runtime_error("MmapFile: cannot open " + path);
    struct stat st{};
    if (::fstat(fd_, &st) != 0) {
      ::close(fd_);
      throw std::runtime_error("MmapFile: fstat failed for " + path);
    }
    size_ = static_cast<std::size_t>(st.st_size);
    if (size_ > 0) {
      void* p = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
      if (p == MAP_FAILED) {
        ::close(fd_);
        throw std::runtime_error("MmapFile: mmap failed for " + path);
      }
      data_ = p;
      ::madvise(data_, size_, MADV_SEQUENTIAL);
    }
  }

  ~MmapFile() { reset(); }

  MmapFile(const MmapFile&) = delete;
  MmapFile& operator=(const MmapFile&) = delete;

  MmapFile(MmapFile&& o) noexcept
      : fd_(std::exchange(o.fd_, -1)), data_(std::exchange(o.data_, nullptr)),
        size_(std::exchange(o.size_, 0)) {}

  MmapFile& operator=(MmapFile&& o) noexcept {
    if (this != &o) {
      reset();
      fd_ = std::exchange(o.fd_, -1);
      data_ = std::exchange(o.data_, nullptr);
      size_ = std::exchange(o.size_, 0);
    }
    return *this;
  }

  [[nodiscard]] const std::byte* data() const noexcept {
    return static_cast<const std::byte*>(data_);
  }
  [[nodiscard]] std::size_t size() const noexcept { return size_; }

private:
  void reset() noexcept {
    if (data_ != nullptr)
      ::munmap(data_, size_);
    if (fd_ >= 0)
      ::close(fd_);
    data_ = nullptr;
    fd_ = -1;
    size_ = 0;
  }

  int fd_{-1};
  void* data_{nullptr};
  std::size_t size_{0};
};

} // namespace bt
