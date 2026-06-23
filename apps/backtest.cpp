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

    // --- Strategy selection -------------------------------------------------
    // Faithful A-S needs constant sigma/k; calibrate them once offline from the
    // data (the paper's calibration step) when not pinned in the config.
    auto as_params = [&]() {
      bt::AvellanedaStoikovParams p{
          .gamma = cfg.as_gamma,
          .sigma = cfg.as_sigma,
          .k = cfg.as_k,
          .horizon_us = static_cast<bt::Ts>(cfg.as_horizon_s * 1e6),
          .order_qty = cfg.order_qty,
      };
      if (p.sigma <= 0.0 || p.k <= 0.0) {
        bt::FeedMerger cal;
        cal.add_source(std::make_unique<bt::LobBinSource>(cfg.lob_bin));
        cal.add_source(std::make_unique<bt::TradesBinSource>(cfg.trades_bin));
        const bt::ASConstants c = bt::AvellanedaStoikov::calibrate(cal);
        if (p.sigma <= 0.0)
          p.sigma = c.sigma;
        if (p.k <= 0.0)
          p.k = c.k;
        std::fprintf(stderr, "a-s: calibrated sigma=%.6g k=%.6g\n", p.sigma, p.k);
      }
      return p;
    };

    std::unique_ptr<bt::Strategy> strat;
    if (cfg.strategy == "as") {
      strat = std::make_unique<bt::AvellanedaStoikov>(as_params());
    } else if (cfg.strategy == "as_online") {
      // Online variant: rolling horizon + online sigma/k, seeded from the same
      // offline calibration so the quotes are sane from the first event.
      const bt::AvellanedaStoikovParams seed = as_params();
      strat = std::make_unique<bt::AvellanedaStoikovOnline>(bt::ASOnlineParams{
          .gamma = cfg.as_gamma,
          .horizon_us = static_cast<bt::Ts>(cfg.as_horizon_s * 1e6),
          .order_qty = cfg.order_qty,
          .max_inventory = cfg.max_inventory,
          .min_half_spread = cfg.as_min_half_spread,
          .vol_alpha = cfg.as_vol_alpha,
          .k_alpha = cfg.as_k_alpha,
          .seed_sigma = seed.sigma,
          .seed_k = seed.k,
      });
    } else if (cfg.strategy == "microprice_as") {
      const bt::AvellanedaStoikovParams p = as_params();
      // One-shot calibration pass over the LOB to fit the Stoikov micro-price.
      bt::LobBinSource cal_lob(cfg.lob_bin);
      bt::MicropriceModel model =
          bt::MicropriceModel::calibrate(cal_lob, {.n_imbalance = cfg.mp_imbalance_bins,
                                                   .n_spread = cfg.mp_spread_bins,
                                                   .sample_dt_us = cfg.mp_sample_dt_us});
      std::fprintf(stderr, "microprice: calibrated on %zu transitions\n", model.samples());
      strat = std::make_unique<bt::MicropriceAS>(p, std::move(model));
    } else if (cfg.strategy == "microprice_as_online") {
      // Best-of-both: online A-S controls + the Stoikov micro-price as the centre.
      const bt::AvellanedaStoikovParams seed = as_params();
      bt::LobBinSource cal_lob(cfg.lob_bin);
      bt::MicropriceModel model =
          bt::MicropriceModel::calibrate(cal_lob, {.n_imbalance = cfg.mp_imbalance_bins,
                                                   .n_spread = cfg.mp_spread_bins,
                                                   .sample_dt_us = cfg.mp_sample_dt_us});
      std::fprintf(stderr, "microprice: calibrated on %zu transitions\n", model.samples());
      strat = std::make_unique<bt::MicropriceASOnline>(
          bt::ASOnlineParams{
              .gamma = cfg.as_gamma,
              .horizon_us = static_cast<bt::Ts>(cfg.as_horizon_s * 1e6),
              .order_qty = cfg.order_qty,
              .max_inventory = cfg.max_inventory,
              .min_half_spread = cfg.as_min_half_spread,
              .vol_alpha = cfg.as_vol_alpha,
              .k_alpha = cfg.as_k_alpha,
              .seed_sigma = seed.sigma,
              .seed_k = seed.k,
          },
          std::move(model));
    } else {
      strat = std::make_unique<bt::FixedSpreadQuoter>(
          bt::QuoterParams{cfg.half_spread, cfg.order_qty, cfg.max_inventory});
    }

    bt::PnLTracker metrics(cfg.fee_bps);

    // Optional per-row time-series for plotting. Fan the engine's single metrics
    // sink out to the tracker (source of truth, added first) and the recorder.
    bt::MetricsFanout sink;
    sink.add(metrics);
    // Risk-adjusted summary (drawdown, return/drawdown, fill ratio, inventory
    // distribution), sampled on the same market-time grid as the series.
    const bt::Ts sample_us = static_cast<bt::Ts>(cfg.series_interval_ms) * 1000;
    bt::RiskMetrics risk(metrics, sample_us);
    sink.add(risk);
    std::unique_ptr<bt::TimeSeriesRecorder> recorder;
    if (!cfg.series_csv.empty()) {
      recorder = std::make_unique<bt::TimeSeriesRecorder>(metrics, cfg.series_csv, sample_us);
      sink.add(*recorder);
    }

    bt::BacktestEngine engine(feed, book, exec, *strat, sink);

    const auto t0 = std::chrono::steady_clock::now();
    const bt::EngineStats st = engine.run();
    const auto t1 = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(t1 - t0).count();

    const bt::PnlReport r = metrics.report();
    const bt::RiskReport rk = risk.report();

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
         << "equity_pnl," << r.equity << '\n'
         << "max_drawdown," << rk.max_drawdown << '\n'
         << "return_over_maxdd," << rk.return_over_maxdd << '\n'
         << "maker_fill_share," << rk.maker_fill_share << '\n'
         << "inv_mean," << rk.inv_mean << '\n'
         << "inv_std," << rk.inv_std << '\n'
         << "inv_max_abs," << rk.inv_max_abs << '\n';
    }

    std::printf("events=%lld book=%lld trades=%lld fills=%lld | inv=%.4f turnover=%.6g fees=%.6g "
                "equity_pnl=%.6g | max_dd=%.6g ret/dd=%.3f maker=%.1f%% inv_max=%.6g | "
                "%.2fs (%.2fM ev/s) -> %s\n",
                static_cast<long long>(st.events), static_cast<long long>(st.book_updates),
                static_cast<long long>(st.trades), static_cast<long long>(st.fills), r.inventory,
                r.turnover, r.fees, r.equity, rk.max_drawdown, rk.return_over_maxdd,
                rk.maker_fill_share * 100.0, rk.inv_max_abs, secs,
                static_cast<double>(st.events) / 1e6 / (secs > 0.0 ? secs : 1.0),
                cfg.report_csv.c_str());

    if (recorder)
      std::fprintf(stderr, "series: %zu rows -> %s\n", recorder->rows(), cfg.series_csv.c_str());
  } catch (const std::exception& e) {
    std::fprintf(stderr, "backtest: error: %s\n", e.what());
    return 1;
  }
  return 0;
}
