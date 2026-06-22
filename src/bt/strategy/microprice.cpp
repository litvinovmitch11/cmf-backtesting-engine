#include "bt/strategy/microprice.hpp"

#include "bt/book/order_book.hpp"
#include "bt/core/event.hpp"
#include "bt/data/event_source.hpp"

#include <algorithm>
#include <cmath>
#include <variant>

namespace bt {
namespace {

// Solve A X = B in place for X (A: n x n, B: n x rhs), Gauss-Jordan with partial
// pivoting. A and B are row-major; on return B holds the solution. Singular rows
// (pivot ~ 0) are treated as identity so empty/never-visited states map to 0.
void solve_in_place(std::vector<double>& a, std::vector<double>& b, int n, int rhs) {
  for (int col = 0; col < n; ++col) {
    int piv = col;
    double best = std::abs(a[col * n + col]);
    for (int r = col + 1; r < n; ++r) {
      const double v = std::abs(a[r * n + col]);
      if (v > best) {
        best = v;
        piv = r;
      }
    }
    if (best < 1e-12)
      continue; // singular column -> leave as-is (identity-like)
    if (piv != col) {
      for (int c = 0; c < n; ++c)
        std::swap(a[piv * n + c], a[col * n + c]);
      for (int c = 0; c < rhs; ++c)
        std::swap(b[piv * rhs + c], b[col * rhs + c]);
    }
    const double inv = 1.0 / a[col * n + col];
    for (int c = 0; c < n; ++c)
      a[col * n + c] *= inv;
    for (int c = 0; c < rhs; ++c)
      b[col * rhs + c] *= inv;
    for (int r = 0; r < n; ++r) {
      if (r == col)
        continue;
      const double f = a[r * n + col];
      if (f == 0.0)
        continue;
      for (int c = 0; c < n; ++c)
        a[r * n + c] -= f * a[col * n + c];
      for (int c = 0; c < rhs; ++c)
        b[r * rhs + c] -= f * b[col * rhs + c];
    }
  }
}

} // namespace

MicropriceModel::MicropriceModel() : MicropriceModel(Config{}) {}
MicropriceModel::MicropriceModel(Config cfg) : cfg_(cfg) {}

int MicropriceModel::imbalance_bucket(double imb) const noexcept {
  int b = static_cast<int>(imb * cfg_.n_imbalance);
  return std::clamp(b, 0, cfg_.n_imbalance - 1);
}

int MicropriceModel::spread_bucket(Ticks spread) const noexcept {
  int b = static_cast<int>(spread) - 1; // 1 tick -> bucket 0
  return std::clamp(b, 0, cfg_.n_spread - 1);
}

int MicropriceModel::state(double imb, Ticks spread) const noexcept {
  return spread_bucket(spread) * cfg_.n_imbalance + imbalance_bucket(imb);
}

void MicropriceModel::accumulate(double imb, Ticks spread, double imb_next, Ticks spread_next,
                                 double dM, bool moved) {
  const int x = state(imb, spread);
  const int y = state(imb_next, spread_next);
  n_state_[x] += 1.0;
  if (moved) {
    t_count_[x * nm_ + y] += 1.0;
    r_sum_[x] += dM;
  } else {
    q_count_[x * nm_ + y] += 1.0;
  }
}

void MicropriceModel::add_transition(double imb, Ticks spread, double imb_next, Ticks spread_next,
                                     double dM, bool moved) {
  if (nm_ == 0) {
    nm_ = cfg_.n_imbalance * cfg_.n_spread;
    q_count_.assign(static_cast<std::size_t>(nm_) * nm_, 0.0);
    t_count_.assign(static_cast<std::size_t>(nm_) * nm_, 0.0);
    r_sum_.assign(nm_, 0.0);
    n_state_.assign(nm_, 0.0);
  }
  accumulate(imb, spread, imb_next, spread_next, dM, moved);
  // Symmetrise: the mirror book (I -> 1-I) yields the opposite mid drift.
  accumulate(1.0 - imb, spread, 1.0 - imb_next, spread_next, -dM, moved);
  ++n_samples_;
}

void MicropriceModel::fit() {
  if (nm_ == 0)
    return;
  const int n = nm_;

  // Build (I - Q) and normalised T, rvec from the raw counts.
  std::vector<double> imq(static_cast<std::size_t>(n) * n, 0.0); // I - Q
  std::vector<double> t(static_cast<std::size_t>(n) * n, 0.0);   // normalised T
  std::vector<double> rvec(n, 0.0);
  for (int x = 0; x < n; ++x) {
    const double nx = n_state_[x];
    imq[x * n + x] = 1.0;
    if (nx <= 0.0)
      continue;
    const double invn = 1.0 / nx;
    for (int y = 0; y < n; ++y) {
      imq[x * n + y] -= q_count_[x * n + y] * invn; // I - Q
      t[x * n + y] = t_count_[x * n + y] * invn;    // T
    }
    rvec[x] = r_sum_[x] * invn; // E[dM * 1(moved) | x]
  }

  // G1 = (I-Q)^-1 rvec  and  B = (I-Q)^-1 T  (solve with a shared factorisation
  // by stacking [rvec | T] as the RHS of (I-Q) X = [rvec | T]).
  const int rhs = 1 + n;
  std::vector<double> a = imq; // copied: solve_in_place destroys it
  std::vector<double> rhs_mat(static_cast<std::size_t>(n) * rhs, 0.0);
  for (int x = 0; x < n; ++x) {
    rhs_mat[x * rhs + 0] = rvec[x];
    for (int y = 0; y < n; ++y)
      rhs_mat[x * rhs + (1 + y)] = t[x * n + y];
  }
  solve_in_place(a, rhs_mat, n, rhs);

  std::vector<double> g1(n);
  std::vector<double> b(static_cast<std::size_t>(n) * n);
  for (int x = 0; x < n; ++x) {
    g1[x] = rhs_mat[x * rhs + 0];
    for (int y = 0; y < n; ++y)
      b[x * n + y] = rhs_mat[x * rhs + (1 + y)];
  }

  // G* = sum_{i>=0} B^i G1, summed iteratively until the term is negligible.
  g_star_ = g1;
  std::vector<double> term = g1;
  std::vector<double> next(n);
  for (int it = 0; it < 1000; ++it) {
    double maxabs = 0.0;
    for (int x = 0; x < n; ++x) {
      double s = 0.0;
      for (int y = 0; y < n; ++y)
        s += b[x * n + y] * term[y];
      next[x] = s;
      maxabs = std::max(maxabs, std::abs(s));
    }
    for (int x = 0; x < n; ++x)
      g_star_[x] += next[x];
    term.swap(next);
    if (maxabs < 1e-12)
      break;
  }
  fitted_ = true;
}

double MicropriceModel::adjustment(double imbalance, Ticks spread) const {
  if (!fitted_ || nm_ == 0)
    return 0.0;
  // Spreads wider than the modeled set carry no information in the paper's state
  // space (see Fig. 5, spread=4: adjustment ~ 0) -> fall back to the mid.
  if (spread < 1 || spread > cfg_.n_spread)
    return 0.0;
  return g_star_[state(imbalance, spread)];
}

MicropriceModel MicropriceModel::calibrate(EventSource& lob, Config cfg) {
  MicropriceModel m(cfg);

  // Only spreads 1..n_spread ticks are part of the paper's state space; a
  // transition is recorded only when both endpoints are in range (Section 4).
  const auto in_range = [&](Ticks s) noexcept { return s >= 1 && s <= cfg.n_spread; };

  // Previous emitted sample (the chain's prior grid point).
  bool have_prev = false;
  double prev_imb = 0.0;
  Ticks prev_spread = 0;
  Ticks prev_mid2 = 0; // 2*mid in ticks (exact; mid moves <=> this changes)

  const auto emit = [&](double imb, Ticks spread, Ticks mid2) {
    if (have_prev && in_range(spread) && in_range(prev_spread)) {
      const bool moved = (mid2 != prev_mid2);
      const double dM = to_price(mid2 - prev_mid2) * 0.5;
      m.add_transition(prev_imb, prev_spread, imb, spread, dM, moved);
    }
    prev_imb = imb;
    prev_spread = spread;
    prev_mid2 = mid2;
    have_prev = true;
  };

  // Most recent book state, forward-filled onto the sampling grid.
  bool have_cur = false;
  double cur_imb = 0.5;
  Ticks cur_spread = 0;
  Ticks cur_mid2 = 0;
  Ts grid = 0;
  bool grid_init = false;

  while (lob.has_next()) {
    const MarketEvent ev = lob.next();
    const auto* bu = std::get_if<BookUpdate>(&ev);
    if (bu == nullptr)
      continue;
    const BookSnapshot& s = bu->book;
    if (s.depth == 0 || s.bids == nullptr || s.asks == nullptr)
      continue;
    const Ticks bid = s.bids[0].px;
    const Ticks ask = s.asks[0].px;
    const double qb = s.bids[0].qty;
    const double qa = s.asks[0].qty;
    const double denom = qb + qa;
    const double imb = (denom > 0.0) ? (qb / denom) : 0.5;
    const Ticks spread = ask - bid;
    const Ticks mid2 = bid + ask;
    const Ts ts = bu->ts;

    if (cfg.sample_dt_us <= 0) {
      emit(imb, spread, mid2); // legacy: one transition per book update (event time)
      continue;
    }

    // Fixed-grid discrete-time chain (paper Section 4): emit the book state in
    // effect at each grid point we have passed, then advance the current state.
    if (!grid_init) {
      grid = ts;
      grid_init = true;
    }
    while (have_cur && ts >= grid) {
      emit(cur_imb, cur_spread, cur_mid2);
      grid += cfg.sample_dt_us;
    }
    cur_imb = imb;
    cur_spread = spread;
    cur_mid2 = mid2;
    have_cur = true;
  }
  m.fit();
  return m;
}

} // namespace bt
