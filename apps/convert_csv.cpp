#include "bt/data/csv_convert.hpp"

#include <cstdio>
#include <exception>
#include <string>

// One-time preprocessing: vendor CSV -> packed binary the engine replays fast.
//   convert_csv lob    data/lob.csv    data/lob.bin
//   convert_csv trades data/trades.csv data/trades.bin
int main(int argc, char** argv) {
  if (argc != 4) {
    std::fprintf(stderr, "usage: %s <lob|trades> <input.csv> <output.bin>\n",
                 argc > 0 ? argv[0] : "convert_csv");
    return 2;
  }
  const std::string type = argv[1];
  const std::string in = argv[2];
  const std::string out = argv[3];
  try {
    bt::ConvertStats st;
    if (type == "lob") {
      st = bt::convert_lob_csv(in, out);
    } else if (type == "trades") {
      st = bt::convert_trades_csv(in, out);
    } else {
      std::fprintf(stderr, "convert_csv: unknown type '%s' (use lob|trades)\n", type.c_str());
      return 2;
    }
    std::printf("%s: %lld records, ts[%lld..%lld], max_tick_round_err=%.3g -> %s\n", type.c_str(),
                static_cast<long long>(st.records), static_cast<long long>(st.first_ts),
                static_cast<long long>(st.last_ts), st.max_tick_round_err, out.c_str());
  } catch (const std::exception& e) {
    std::fprintf(stderr, "convert_csv: error: %s\n", e.what());
    return 1;
  }
  return 0;
}
