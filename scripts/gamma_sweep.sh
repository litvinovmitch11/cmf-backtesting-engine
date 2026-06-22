#!/usr/bin/env bash
# Risk-aversion (gamma) sweep for the Avellaneda-Stoikov and micro-price-A-S
# strategies on the full dataset. Reproduces reports/gamma_sweep.csv.
#
#   ./scripts/gamma_sweep.sh            # default gammas, default build
#   GAMMAS="1 100 500" ./scripts/gamma_sweep.sh
#
# sigma/k are calibrated offline from the data and are independent of gamma, so
# every row uses the same calibrated constants (only gamma varies).
set -euo pipefail

BUILD="${BUILD:-build/release}"
BT="$BUILD/backtest"
GAMMAS="${GAMMAS:-1 10 50 100 500 2000}"
OUT="${OUT:-reports/gamma_sweep.csv}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

[ -x "$BT" ] || { echo "build first: cmake --build --preset release" >&2; exit 1; }

mkdir -p "$(dirname "$OUT")"
echo "strategy,gamma,fills,inventory,turnover,fees,equity_pnl" >"$OUT"

val() { grep "^$1," "$2" | cut -d, -f2; }

for strat in as microprice_as; do
  for g in $GAMMAS; do
    cfg="$TMP/$strat-$g.json"
    rep="$TMP/$strat-$g.csv"
    jq --argjson g "$g" --arg rep "$rep" \
       '.strategy=$strat | .as_gamma=$g | .report_csv=$rep' \
       --arg strat "$strat" "configs/$strat.json" >"$cfg"
    # Capture fills from stdout; the rest from the report CSV.
    line="$("$BT" "$cfg" 2>/dev/null)"
    fills="$(sed -E 's/.*fills=([0-9]+).*/\1/' <<<"$line")"
    printf '%s,%s,%s,%s,%s,%s,%s\n' \
      "$strat" "$g" "$fills" \
      "$(val inventory "$rep")" "$(val turnover "$rep")" \
      "$(val fees "$rep")" "$(val equity_pnl "$rep")" >>"$OUT"
    echo "  $strat gamma=$g -> $line" >&2
  done
done

echo "wrote $OUT" >&2
