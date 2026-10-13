#!/usr/bin/env bash
# =============================================================================
# MarketState - run the whole application with one command.
#
#   ./run.sh                       build, test, demo, verify, cross-check, sweep
#   ./run.sh 200000                same, with a 200k-event demo stream
#
# Common overrides (environment variables):
#   SIZES=10000,100000             experiment event counts   (default: full)
#   SEVERITIES=light,medium,heavy  chaos severities          (default: all)
#   REFERENCE=python|ocaml         reference used to check   (default: python)
#   SEED=42                        generator/experiment seed
#   SKIP_TESTS=1                   skip the unit-test step
#   SKIP_EXPERIMENTS=1             skip the performance/chaos sweep
#
# Everything it produces lands under data/; the headline is data/report.html
# =============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

ENGINE="$ROOT/build/core/marketstate"
TESTS="$ROOT/build/core/marketstate_tests"
OCAML_MODEL="$ROOT/reference/ocaml/_build/model"

EVENTS="${1:-100000}"
SIZES="${SIZES:-10000,100000,1000000}"
SEVERITIES="${SEVERITIES:-light,medium,heavy}"
SEED="${SEED:-42}"
REFERENCE="${REFERENCE:-python}"
SKIP_TESTS="${SKIP_TESTS:-0}"
SKIP_EXPERIMENTS="${SKIP_EXPERIMENTS:-0}"

c_bold=$'\033[1m'; c_cy=$'\033[1;36m'; c_gr=$'\033[1;32m'; c_rd=$'\033[1;31m'; c_off=$'\033[0m'
step() { printf '\n%s== %s ==%s\n' "$c_cy" "$*" "$c_off"; }
info() { printf '   %s\n' "$*"; }
fail() { printf '%s   %s%s\n' "$c_rd" "$*" "$c_off"; exit 1; }

# --- 0. prerequisites --------------------------------------------------------
step "0/6  Checking prerequisites"
for tool in cmake g++ python3; do
  command -v "$tool" >/dev/null 2>&1 || fail "required tool not found: $tool"
  info "found $tool"
done
JOBS="$( (command -v nproc >/dev/null 2>&1 && nproc) || echo 4 )"

# --- 1. build the C++ engine -------------------------------------------------
step "1/6  Building the C++ engine (release)"
cmake -S "$ROOT" -B "$ROOT/build" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON >/dev/null
cmake --build "$ROOT/build" -j "$JOBS" >/dev/null
info "engine  -> $ENGINE"

# --- 2. build the OCaml reference (optional) --------------------------------
step "2/6  Building the OCaml reference model"
if "$ROOT/reference/ocaml/build.sh" >/dev/null 2>&1; then
  info "OCaml reference ready (two independent models available)"
else
  info "OCaml unavailable - using the Python reference only"
fi

# --- 3. unit tests -----------------------------------------------------------
if [[ "$SKIP_TESTS" != "1" ]]; then
  step "3/6  Running unit tests"
  "$TESTS" --gtest_brief=1
else
  step "3/6  Running unit tests (skipped)"
fi

# --- 4. end-to-end demo: generate -> fault -> recover -> verify --------------
step "4/6  Demo: generate -> fault -> recover -> verify ($EVENTS events)"
python3 tools/generator/marketgen.py --events "$EVENTS" --seed "${DEMO_SEED:-7}" \
  --symbols AAPL,MSFT,GOOG \
  --out data/demo_clean.txt --faulty-out data/demo_faulty.txt \
  --scenario-out data/demo_scenario.json \
  --faults "drop=0.03,dup=0.03,reorder=0.05,corrupt=0.02" >/dev/null
info "generated clean + fault-injected streams"

"$ENGINE" run --events data/demo_faulty.txt --recovery-source data/demo_clean.txt \
  --state-out data/demo_engine.state --metrics-out data/demo_engine.metrics.json --quiet >/dev/null
info "recovered the faulty stream from the retransmission source"

python3 reference/python/model.py --events data/demo_clean.txt \
  --state-out data/demo_ref.state >/dev/null
[[ -x "$OCAML_MODEL" ]] && "$OCAML_MODEL" --events data/demo_clean.txt \
  --state-out data/demo_ref_ml.state >/dev/null
info "computed reference state(s)"

python3 tools/analyzer/verify.py --clean data/demo_clean.txt \
  --faulty data/demo_faulty.txt --report-out data/demo_verify.json >/dev/null
if python3 -c "import json,sys; sys.exit(0 if json.load(open('data/demo_verify.json'))['passed'] else 1)"; then
  info "verification: PASS (differential, determinism, recovery, honest degradation)"
else
  fail "verification FAILED - see data/demo_verify.json"
fi

# --- 5. cross-check engine vs. every reference model -------------------------
step "5/6  Cross-check engine vs. reference models"
python3 tools/analyzer/crosscheck.py --events data/demo_clean.txt

# --- 6. full experiment sweep ------------------------------------------------
if [[ "$SKIP_EXPERIMENTS" != "1" ]]; then
  step "6/6  Experiment sweep (sizes=$SIZES, severities=$SEVERITIES, reference=$REFERENCE)"
  python3 tools/analyzer/experiment.py --sizes "$SIZES" --severities "$SEVERITIES" \
    --seed "$SEED" --reference "$REFERENCE" \
    --json-out data/report.json --html-out data/report.html
else
  step "6/6  Experiment sweep (skipped)"
fi

printf '\n%sDone.%s  Open the report: %s\n' "$c_gr" "$c_off" "$ROOT/data/report.html"
