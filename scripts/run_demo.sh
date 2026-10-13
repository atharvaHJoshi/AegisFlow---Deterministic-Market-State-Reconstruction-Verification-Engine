#!/usr/bin/env bash
# End-to-end MarketState demo: generate -> fault -> recover -> verify.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENGINE="$ROOT/build/core/marketstate"
DATA="$ROOT/data"
mkdir -p "$DATA"

if [[ ! -x "$ENGINE" ]]; then
  echo "Building engine..."
  cmake -S "$ROOT" -B "$ROOT/build" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON >/dev/null
  cmake --build "$ROOT/build" -j >/dev/null
fi

EVENTS="${1:-100000}"
SEED="${2:-7}"

echo "== 1. Generate clean + fault-injected streams ($EVENTS events, seed $SEED) =="
python3 "$ROOT/tools/generator/marketgen.py" \
  --events "$EVENTS" --seed "$SEED" --symbols AAPL,MSFT,GOOG \
  --out "$DATA/demo_clean.txt" \
  --faulty-out "$DATA/demo_faulty.txt" \
  --scenario-out "$DATA/demo_scenario.json" \
  --faults "drop=0.03,dup=0.03,reorder=0.05,corrupt=0.02"

echo
echo "== 2. Recover the faulty stream using the clean stream as retransmission source =="
"$ENGINE" run --events "$DATA/demo_faulty.txt" \
  --recovery-source "$DATA/demo_clean.txt" \
  --state-out "$DATA/demo_engine.state" \
  --metrics-out "$DATA/demo_engine.metrics.json" --quiet

echo
echo "== 3. Independent reference models over the authoritative history =="
python3 "$ROOT/reference/python/model.py" \
  --events "$DATA/demo_clean.txt" --state-out "$DATA/demo_ref.state"
if "$ROOT/reference/ocaml/build.sh" >/dev/null 2>&1; then
  "$ROOT/reference/ocaml/_build/model" \
    --events "$DATA/demo_clean.txt" --state-out "$DATA/demo_ref_ml.state" >/dev/null
else
  echo "(OCaml not available; skipping the second reference model)"
fi

echo
echo "== 4. Full verification suite =="
python3 "$ROOT/tools/analyzer/verify.py" \
  --clean "$DATA/demo_clean.txt" --faulty "$DATA/demo_faulty.txt" \
  --report-out "$DATA/demo_verify.json"

echo
echo "== 5. Cross-check engine vs. all reference models =="
python3 "$ROOT/tools/analyzer/crosscheck.py" --events "$DATA/demo_clean.txt" || true

echo
echo "Recovered state and reference state are identical: $(cmp -s "$DATA/demo_engine.state" "$DATA/demo_ref.state" && echo YES || echo NO)"
