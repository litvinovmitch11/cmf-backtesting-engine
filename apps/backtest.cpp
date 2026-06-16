#include "bt/book/order_book.hpp"
#include "bt/data/feed_merger.hpp"
#include "bt/data/lob_source.hpp"
#include "bt/data/trades_source.hpp"
#include "bt/engine.hpp"
#include "bt/exec.hpp"
#include "bt/metrics.hpp"
#include "bt/strategy.hpp"

#include <chrono>
#include <cstdio>
#include <exception>
#include <fstream>
#include <memory>
#include <string>

// Replay the converted market data through the engine with a strategy.
//   backtest [config.json]   (defaults if no config given)
int main(int argc, char** argv) {
  try {
    const bt::Config cfg = (argc >= 2) ? bt::Config::load(argv[1]) : bt::Config{};

    bt::FeedMerger feed;
    feed.add_source(std::make_unique<bt::LobBinSource>(cfg.lob_bin));       // channel 0
    feed.add_source(std::make_unique<bt::TradesBinSource>(cfg.trades_bin)); // channel 1

    bt::OrderBook book;

    std::unique_ptr<bt::QueueModel> queue =
        (cfg.queue_model == "proportional")
            ? std::unique_ptr<bt::QueueModel>(std::make_unique<bt::ProportionalQueue>())
            : std::unique_ptr<bt::QueueModel>(std::make_unique<bt::OptimisticQueue>());

    bt::ExecutionSimulator exec(book, *queue,
                                bt::LatencyModel{cfg.feed_latency_us, cfg.order_latency_us});
    bt::FixedSpreadQuoter strat(
        bt::QuoterParams{cfg.half_spread, cfg.order_qty, cfg.max_inventory});
    bt::PnLTracker metrics(cfg.fee_bps);

    bt::BacktestEngine engine(feed, book, exec, strat, metrics);

    const auto t0 = std::chrono::steady_clock::now();
    const bt::EngineStats st = engine.run();
    const auto t1 = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(t1 - t0).count();

    const bt::PnlReport r = metrics.report();

    // Deterministic summary CSV (no timing, so two runs produce identical files).
    {
      std::ofstream os(cfg.report_csv);
      os << "metric,value\n"
         << "events," << st.events << '\n'
         << "book_updates," << st.book_updates << '\n'
         << "trades," << st.trades << '\n'
         << "fills," << st.fills << '\n'
         << "inventory," << r.inventory << '\n'
         << "turnover," << r.turnover << '\n'
         << "fees," << r.fees << '\n'
         << "realized_cash," << r.realized_cash << '\n'
         << "mark_price," << r.mark_price << '\n'
         << "equity_pnl," << r.equity << '\n';
    }

    std::printf("events=%lld book=%lld trades=%lld fills=%lld | inv=%.4f turnover=%.6g fees=%.6g "
                "equity_pnl=%.6g | %.2fs (%.2fM ev/s) -> %s\n",
                static_cast<long long>(st.events), static_cast<long long>(st.book_updates),
                static_cast<long long>(st.trades), static_cast<long long>(st.fills), r.inventory,
                r.turnover, r.fees, r.equity, secs,
                static_cast<double>(st.events) / 1e6 / (secs > 0.0 ? secs : 1.0),
                cfg.report_csv.c_str());
  } catch (const std::exception& e) {
    std::fprintf(stderr, "backtest: error: %s\n", e.what());
    return 1;
  }
  return 0;
}
