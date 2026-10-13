#!/usr/bin/env bash
# Full reproducible verification + performance sweep; writes data/report.{json,html}.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENGINE="$ROOT/build/core/marketstate"

if [[ ! -x "$ENGINE" ]]; then
  cmake -S "$ROOT" -B "$ROOT/build" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON >/dev/null
  cmake --build "$ROOT/build" -j >/dev/null
fi

SIZES="${SIZES:-10000,100000,1000000}"
SEVERITIES="${SEVERITIES:-light,medium,heavy}"
SEED="${SEED:-42}"
REFERENCE="${REFERENCE:-python}"

echo "Running tests..." 
"$ROOT/build/core/marketstate_tests" --gtest_brief=1

echo "Running experiments (sizes=$SIZES severities=$SEVERITIES seed=$SEED reference=$REFERENCE)..."
python3 "$ROOT/tools/analyzer/experiment.py" \
  --sizes "$SIZES" --severities "$SEVERITIES" --seed "$SEED" \
  --reference "$REFERENCE" \
  --json-out "$ROOT/data/report.json" --html-out "$ROOT/data/report.html"

echo
echo "Report: $ROOT/data/report.html"
