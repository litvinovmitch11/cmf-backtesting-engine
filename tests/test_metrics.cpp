#include "bt/metrics.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using bt::Side;

namespace {

// Parse the recorder's CSV back into rows of (column -> value) keyed by header.
struct Series {
  std::vector<std::string> header;
  std::vector<std::vector<double>> rows;

  [[nodiscard]] double at(std::size_t row, const std::string& col) const {
    for (std::size_t c = 0; c < header.size(); ++c)
      if (header[c] == col)
        return rows.at(row).at(c);
    throw std::runtime_error("no column " + col);
  }
};

Series read_series(const std::string& path) {
  std::ifstream is(path);
  Series s;
  std::string line;
  std::getline(is, line);
  {
    std::stringstream ss(line);
    std::string cell;
    while (std::getline(ss, cell, ','))
      s.header.push_back(cell);
  }
  while (std::getline(is, line)) {
    std::stringstream ss(line);
    std::string cell;
    std::vector<double> r;
    while (std::getline(ss, cell, ','))
      r.push_back(std::stod(cell));
    s.rows.push_back(std::move(r));
  }
  return s;
}

bt::Fill mk_fill(bt::Ts ts, Side side, bt::Qty qty) {
  return bt::Fill{
      .ts = ts, .order_id = 1, .side = side, .price = 1'000'000, .qty = qty, .maker = true};
}

} // namespace

TEST_CASE("TimeSeriesRecorder samples on the market-time grid and sums bucket volume",
          "[metrics]") {
  const std::filesystem::path path = std::filesystem::temp_directory_path() / "bt_series_test.csv";

  {
    bt::PnLTracker pnl(0.0);
    bt::TimeSeriesRecorder rec(pnl, path.string(), /*interval_us=*/10);

    // Drive the pair the way MetricsFanout does: tracker first, recorder second.
    auto mark = [&](bt::Ts ts, double mid) {
      pnl.on_mark(ts, mid);
      rec.on_mark(ts, mid);
    };
    auto fill = [&](bt::Ts ts, Side side, bt::Qty qty) {
      const bt::Fill f = mk_fill(ts, side, qty);
      pnl.on_fill(f);
      rec.on_fill(f);
    };

    mark(0, 100.0);            // opening row
    fill(5, Side::Buy, 10.0);  // accrues into the next sampled bucket
    mark(5, 100.0);            // 5 < 10: no new row
    mark(12, 100.0);           // crosses the boundary -> a row
    fill(15, Side::Sell, 4.0); // accrues into the closing bucket
    rec.finalize();            // flushes the residual bucket + writes the file
  }

  const Series s = read_series(path.string());
  REQUIRE(s.rows.size() == 3);

  // Opening row: nothing traded yet.
  CHECK(s.at(0, "fills") == Catch::Approx(0));
  CHECK(s.at(0, "inventory") == Catch::Approx(0));
  CHECK(s.at(0, "bucket_qty") == Catch::Approx(0));

  // Boundary row: the buy is now reflected.
  CHECK(s.at(1, "fills") == Catch::Approx(1));
  CHECK(s.at(1, "inventory") == Catch::Approx(10));
  CHECK(s.at(1, "bucket_qty") == Catch::Approx(10));

  // Closing flush row: the residual sell.
  CHECK(s.at(2, "fills") == Catch::Approx(2));
  CHECK(s.at(2, "inventory") == Catch::Approx(6));
  CHECK(s.at(2, "bucket_qty") == Catch::Approx(4));

  // Bucket volume partitions the total traded quantity exactly.
  double bucket_sum = 0;
  for (std::size_t r = 0; r < s.rows.size(); ++r)
    bucket_sum += s.at(r, "bucket_qty");
  CHECK(bucket_sum == Catch::Approx(14));

  std::filesystem::remove(path);
}
