#include "bt/core/event.hpp"
#include "bt/data/csv_convert.hpp"
#include "bt/data/event_source.hpp"
#include "bt/data/feed_merger.hpp"
#include "bt/data/lob_source.hpp"
#include "bt/data/trades_source.hpp"

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <memory>
#include <utility>
#include <variant>
#include <vector>

namespace {

// In-memory EventSource over a fixed list — used to test the merger in isolation.
class VectorSource final : public bt::EventSource {
public:
  explicit VectorSource(std::vector<bt::MarketEvent> evs) : evs_(std::move(evs)) {}
  bool has_next() const override { return i_ < evs_.size(); }
  bt::Ts peek_ts() const override { return bt::timestamp_of(evs_[i_]); }
  bt::MarketEvent next() override { return evs_[i_++]; }

private:
  std::vector<bt::MarketEvent> evs_;
  std::size_t i_{0};
};

std::filesystem::path tmp(const char* name) {
  return std::filesystem::temp_directory_path() / name;
}

} // namespace

TEST_CASE("FeedMerger yields a globally time-sorted, deterministic stream", "[data][merger]") {
  auto a = std::make_unique<VectorSource>(std::vector<bt::MarketEvent>{
      bt::TradePrint{.ts = 10}, bt::TradePrint{.ts = 30}, bt::TradePrint{.ts = 30}});
  auto b = std::make_unique<VectorSource>(std::vector<bt::MarketEvent>{
      bt::BookUpdate{.ts = 5}, bt::BookUpdate{.ts = 20}, bt::BookUpdate{.ts = 30}});

  bt::FeedMerger m;
  m.add_source(std::move(a)); // source idx 0
  m.add_source(std::move(b)); // source idx 1
  REQUIRE(m.source_count() == 2);

  std::vector<bt::Ts> ts;
  while (m.has_next()) {
    const bt::Ts peeked = m.peek_ts();
    const bt::MarketEvent e = m.next();
    REQUIRE(bt::timestamp_of(e) == peeked); // peek matches what next() returns
    ts.push_back(peeked);
  }
  REQUIRE(ts == std::vector<bt::Ts>{5, 10, 20, 30, 30, 30});
}

TEST_CASE("LOB CSV -> binary round-trips through LobBinSource", "[data][convert]") {
  namespace fs = std::filesystem;
  const fs::path csv = tmp("bt_ut_lob.csv");
  const fs::path bin = tmp("bt_ut_lob.bin");
  {
    std::ofstream os(csv);
    os << ",local_timestamp";
    for (int i = 0; i < 25; ++i)
      os << ",asks[" << i << "].price,asks[" << i << "].amount,bids[" << i << "].price,bids[" << i
         << "].amount";
    os << '\n' << "0,1000";
    for (int i = 0; i < 25; ++i)
      os << ",0.0110436,100,0.0110435,200";
    os << '\n' << "1,2000";
    for (int i = 0; i < 25; ++i)
      os << ",0.0110440,101,0.0110430,201";
    os << '\n';
  }

  const bt::ConvertStats st = bt::convert_lob_csv(csv.string(), bin.string());
  REQUIRE(st.records == 2);
  REQUIRE(st.first_ts == 1000);
  REQUIRE(st.last_ts == 2000);
  REQUIRE(st.max_tick_round_err < 1e-3);

  bt::LobBinSource src(bin.string());
  REQUIRE(src.size() == 2);
  REQUIRE(src.peek_ts() == 1000);

  const bt::MarketEvent e0 = src.next();
  REQUIRE(std::holds_alternative<bt::BookUpdate>(e0));
  const bt::BookUpdate& bu = std::get<bt::BookUpdate>(e0);
  REQUIRE(bu.ts == 1000);
  REQUIRE(bu.book.depth == 25);
  REQUIRE(bu.book.asks[0].px == bt::to_ticks(0.0110436));
  REQUIRE(bu.book.bids[0].px == bt::to_ticks(0.0110435));
  REQUIRE(bu.book.asks[0].qty == 100.0);
  REQUIRE(bu.book.bids[24].qty == 200.0);

  REQUIRE(src.peek_ts() == 2000);
  REQUIRE(src.has_next());
  src.next();
  REQUIRE_FALSE(src.has_next());

  fs::remove(csv);
  fs::remove(bin);
}

TEST_CASE("trades CSV -> binary round-trips through TradesBinSource", "[data][convert]") {
  namespace fs = std::filesystem;
  const fs::path csv = tmp("bt_ut_trades.csv");
  const fs::path bin = tmp("bt_ut_trades.bin");
  {
    std::ofstream os(csv);
    os << ",local_timestamp,side,price,amount\n";
    os << "0,1000,sell,0.0110435,734\n";
    os << "1,1500,buy,0.0110440,50\n";
  }

  const bt::ConvertStats st = bt::convert_trades_csv(csv.string(), bin.string());
  REQUIRE(st.records == 2);

  bt::TradesBinSource src(bin.string());
  REQUIRE(src.size() == 2);

  const bt::MarketEvent e0 = src.next();
  const bt::TradePrint& t0 = std::get<bt::TradePrint>(e0);
  REQUIRE(t0.ts == 1000);
  REQUIRE(t0.aggressor == bt::Side::Sell);
  REQUIRE(t0.price == bt::to_ticks(0.0110435));
  REQUIRE(t0.amount == 734.0);

  const bt::MarketEvent e1 = src.next();
  const bt::TradePrint& t1 = std::get<bt::TradePrint>(e1);
  REQUIRE(t1.ts == 1500);
  REQUIRE(t1.aggressor == bt::Side::Buy);
  REQUIRE(t1.amount == 50.0);
  REQUIRE_FALSE(src.has_next());

  fs::remove(csv);
  fs::remove(bin);
}
