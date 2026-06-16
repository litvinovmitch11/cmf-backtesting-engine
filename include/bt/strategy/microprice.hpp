#pragma once

#include "bt/core/types.hpp"

#include <cstdint>
#include <vector>

namespace bt {

class EventSource;
class OrderBook;

// Stoikov (2018), "The Micro-Price: A High Frequency Estimator of Future Prices".
//
// The micro-price is the martingale limit of expected future mid-prices,
//     P^micro = M + g(I, S),
// where M is the mid, I the top-of-book imbalance Qb/(Qb+Qa) and S the spread.
// g is estimated from a finite-state Markov chain on the discretised (I, S):
//
//   * Q  = transitions that leave the mid unchanged (transient),
//   * T  = transitions on which the mid moves (to a new (I,S) state),
//   * rvec[x] = E[dM * 1(mid moved) | state x]   (the R*K product of the paper),
//   * G1 = (I - Q)^-1 rvec                        (first-order adjustment),
//   * B  = (I - Q)^-1 T,
//   * G* = sum_{i>=0} B^i G1                       (full adjustment, converges).
//
// Data is symmetrised ((I,S,dM) -> (1-I,S,-dM)) so the chain is unbiased and the
// series converges (Theorem 3.1). The result G*[x] is the micro-price adjustment
// added to the mid.
class MicropriceModel {
public:
  struct Config {
    int n_imbalance{10}; // imbalance buckets in [0,1)
    int n_spread{4};     // spread buckets: 1..n_spread ticks (capped)
  };

  MicropriceModel();                    // default Config
  explicit MicropriceModel(Config cfg); // (defined out-of-line: nested Config init)

  // Accumulate one observed transition between consecutive book states.
  // `dM` is the mid change in price units; `moved` is whether the mid changed.
  void add_transition(double imb, Ticks spread, double imb_next, Ticks spread_next, double dM,
                      bool moved);

  // Solve the chain and compute G*. Call once after all transitions are added.
  void fit();

  [[nodiscard]] bool fitted() const noexcept { return fitted_; }
  [[nodiscard]] std::size_t samples() const noexcept { return n_samples_; }

  // Micro-price adjustment g(I,S) in price units (0 if not fitted).
  [[nodiscard]] double adjustment(double imbalance, Ticks spread) const;

  // One-shot calibration pass over a stream of book updates.
  static MicropriceModel calibrate(EventSource& lob, Config cfg);

private:
  [[nodiscard]] int imbalance_bucket(double imb) const noexcept;
  [[nodiscard]] int spread_bucket(Ticks spread) const noexcept;
  [[nodiscard]] int state(double imb, Ticks spread) const noexcept;
  void accumulate(double imb, Ticks spread, double imb_next, Ticks spread_next, double dM,
                  bool moved);

  Config cfg_;
  int nm_{0};
  std::size_t n_samples_{0};
  bool fitted_{false};

  // Raw counts (row-major nm x nm) and per-state totals, built during accumulate.
  std::vector<double> q_count_; // no-move transitions
  std::vector<double> t_count_; // move transitions
  std::vector<double> r_sum_;   // sum of dM on move transitions (size nm)
  std::vector<double> n_state_; // total transitions per state (size nm)

  std::vector<double> g_star_; // fitted adjustment per state (size nm)
};

} // namespace bt
