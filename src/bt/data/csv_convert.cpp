#include "bt/data/csv_convert.hpp"

#include "bt/data/binary_record.hpp"
#include "bt/data/mmap_file.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace bt {
namespace {

// Forward cursor over the mmapped CSV text. Fields are parsed with from_chars
// (fast, locale-independent); the trailing comma after each field is consumed.
struct Parser {
  const char* p;
  const char* end;

  [[nodiscard]] bool done() const { return p >= end; }

  void skip_line() {
    while (p < end && *p != '\n')
      ++p;
    if (p < end)
      ++p; // consume the '\n'
  }

  std::int64_t i64() {
    std::int64_t v = 0;
    const std::from_chars_result r = std::from_chars(p, end, v);
    if (r.ec == std::errc{})
      p = r.ptr;
    if (p < end && *p == ',')
      ++p;
    return v;
  }

  double f64() {
    double v = 0.0;
    const std::from_chars_result r = std::from_chars(p, end, v);
    if (r.ec == std::errc{})
      p = r.ptr;
    if (p < end && *p == ',')
      ++p;
    return v;
  }

  // First character of a textual field (e.g. 'b'/'s' for buy/sell), then skip it.
  char token() {
    const char c = (p < end) ? *p : '\0';
    while (p < end && *p != ',' && *p != '\n')
      ++p;
    if (p < end && *p == ',')
      ++p;
    return c;
  }
};

void write_bin(const std::string& out, const char* magic, std::uint32_t depth, std::int64_t count,
               const void* data, std::size_t bytes) {
  std::ofstream os(out, std::ios::binary | std::ios::trunc);
  if (!os)
    throw std::runtime_error("convert: cannot open output " + out);
  BinHeader h{};
  std::memcpy(h.magic, magic, sizeof(h.magic));
  h.version = kBinVersion;
  h.depth = depth;
  h.price_scale = kPriceScale;
  h.record_count = count;
  h.reserved = 0;
  os.write(reinterpret_cast<const char*>(&h), sizeof(h));
  if (bytes > 0)
    os.write(static_cast<const char*>(data), static_cast<std::streamsize>(bytes));
  if (!os)
    throw std::runtime_error("convert: write failed for " + out);
}

double round_err(double scaled) {
  return std::fabs(scaled - std::nearbyint(scaled));
}

const char* text_begin(const MmapFile& m) {
  return reinterpret_cast<const char*>(m.data());
}

} // namespace

ConvertStats convert_lob_csv(const std::string& in_csv, const std::string& out_bin) {
  const MmapFile in(in_csv);
  Parser ps{text_begin(in), text_begin(in) + in.size()};
  ps.skip_line(); // header row

  std::vector<LobRecord> recs;
  ConvertStats st{};
  const double scale = static_cast<double>(kPriceScale);

  while (!ps.done()) {
    if (*ps.p == '\n' || *ps.p == '\r') {
      ps.skip_line();
      continue;
    }
    const char* const line_start = ps.p;
    LobRecord r{};
    (void)ps.i64(); // row index
    r.ts = ps.i64();
    for (std::uint32_t i = 0; i < kLobDepth; ++i) {
      const double apx = ps.f64();
      const double aqty = ps.f64();
      const double bpx = ps.f64();
      const double bqty = ps.f64();
      r.asks[i] = BookLevel{to_ticks(apx), aqty};
      r.bids[i] = BookLevel{to_ticks(bpx), bqty};
      st.max_tick_round_err =
          std::max({st.max_tick_round_err, round_err(apx * scale), round_err(bpx * scale)});
    }
    ps.skip_line();
    if (ps.p == line_start) {
      ++ps.p; // forward-progress guard against a malformed line
      continue;
    }
    recs.push_back(r);
  }

  st.records = static_cast<std::int64_t>(recs.size());
  if (!recs.empty()) {
    st.first_ts = recs.front().ts;
    st.last_ts = recs.back().ts;
  }
  write_bin(out_bin, kLobMagic, kLobDepth, st.records, recs.data(),
            recs.size() * sizeof(LobRecord));
  return st;
}

ConvertStats convert_trades_csv(const std::string& in_csv, const std::string& out_bin) {
  const MmapFile in(in_csv);
  Parser ps{text_begin(in), text_begin(in) + in.size()};
  ps.skip_line(); // header row

  std::vector<TradeRecord> recs;
  ConvertStats st{};
  const double scale = static_cast<double>(kPriceScale);

  while (!ps.done()) {
    if (*ps.p == '\n' || *ps.p == '\r') {
      ps.skip_line();
      continue;
    }
    const char* const line_start = ps.p;
    TradeRecord r{};
    (void)ps.i64(); // row index
    r.ts = ps.i64();
    const char side = ps.token();   // "buy" / "sell"
    r.side = (side == 's') ? 1 : 0; // 1 = Sell, 0 = Buy
    const double px = ps.f64();
    r.px = to_ticks(px);
    r.qty = ps.f64();
    st.max_tick_round_err = std::max(st.max_tick_round_err, round_err(px * scale));
    ps.skip_line();
    if (ps.p == line_start) {
      ++ps.p;
      continue;
    }
    recs.push_back(r);
  }

  st.records = static_cast<std::int64_t>(recs.size());
  if (!recs.empty()) {
    st.first_ts = recs.front().ts;
    st.last_ts = recs.back().ts;
  }
  write_bin(out_bin, kTradeMagic, 0, st.records, recs.data(), recs.size() * sizeof(TradeRecord));
  return st;
}

} // namespace bt
