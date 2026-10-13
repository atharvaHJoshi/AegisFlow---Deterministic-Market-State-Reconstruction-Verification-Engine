# Verification & Experiment Methodology

MarketState's central claim is not "we process events fast" but "we can show
when the state is trustworthy and prove that recovery is correct." This document
describes how that is tested.

## Correctness oracle: independent reference models

There are **two** independent re-implementations of the state-transition rules,
sharing no code with the C++ engine:

- `reference/python/model.py` — Python, dict of orders, levels recomputed on
  demand.
- `reference/ocaml/model.ml` — OCaml, typed variants and `Hashtbl`, 64-bit FNV
  checksum arithmetic via `Int64`.

Both use a different language and data layout from the engine (and from each
other) but emit the same canonical text. Given the same authoritative ordered
history, the engine and *both* references must produce byte-identical canonical
state. Any divergence — including the two references disagreeing with each
other — is a candidate correctness bug. This is the differential test used
throughout.

`tools/analyzer/crosscheck.py` runs the engine and both references on one clean
stream and fails unless all three states are identical; the gate is intended to
run before trusting any experiment result.

## Four verification checks (`tools/analyzer/verify.py`)

1. **Differential correctness**
   `engine(clean) == reference(clean)`.

2. **Deterministic replay**
   `engine(clean)` run twice produces identical bytes. No dependence on wall
   clock, unordered iteration, or unseeded randomness inside market state.

3. **Recovery equivalence**
   `engine(chaos + retransmission source) == reference(clean)`.
   This is exactly `S_checkpoint+replay == S_full_replay` at stream scale, with
   drops, duplicates, reordering, and corruption in between.

4. **Honest degradation**
   With an unreliable stream and *no* recovery source, the engine state must
   differ from the authoritative reference. A system that silently matched would
   be lying about gaps it cannot have repaired.

`tools/analyzer/experiment.py` runs checks 1, 3, and 4 across a matrix of event
counts and fault severities and writes `data/report.json` + `data/report.html`.

## Faults injected

`tools/generator/marketgen.py` derives a "wire" stream from a clean stream using
a reproducible seed:

| Fault | Effect |
| --- | --- |
| `drop` | events omitted (creates gaps and tail loss) |
| `dup` | events delivered twice |
| `reorder` | local adjacent swaps |
| `delay` | events pushed later in delivery order |
| `corrupt` | a payload field is altered, invalidating the checksum |

The exact scenario (seed, rates, affected sequence numbers) is stored next to
the results so every experiment is reproducible.

## Unit / integration tests (GoogleTest, `core/tests/`)

- **Codec**: round-trip for every type; checksum detects payload corruption;
  malformed arity rejected; impossible values rejected.
- **Order book**: level aggregation, partial cancel, modify, canonical
  round-trip, malformed-state rejection.
- **Sequence**: in-order, duplicate, gap-then-drain, multi-buffer drain, stale,
  bounded-buffer degradation.
- **Reconciler**: gap recovery from a memory source, unresolvable gap stays
  `DEGRADED`, no-source degradation.
- **Recovery**: checkpoint+replay == full replay; crash **after log before
  apply** repaired; crash **before log** leaves a consistent boundary;
  corrupt checkpoint falls back to full replay; finalize repairs tail loss;
  post-restart events are not reapplied.
- **Durability regression**: `SqliteEventLog::replay_all()` returns the full
  history (guards against the signed-`uint64` bind bug that once made recovery
  compare against an empty state).

## Reproducing

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON && cmake --build build -j
./build/core/marketstate_tests

# engine + Python + OCaml must agree on one clean stream
tools/analyzer/crosscheck.py --events data/demo_clean.txt

python3 tools/analyzer/verify.py --clean data/demo_clean.txt --faulty data/demo_faulty.txt
python3 tools/analyzer/verify.py --clean data/demo_clean.txt --faulty data/demo_faulty.txt \
        --reference ocaml            # same checks against the OCaml reference
python3 tools/analyzer/experiment.py --sizes 10000,100000,1000000 \
        --severities light,medium,heavy
```

All results quoted in the README come from these commands; the machine, seed,
fault rates, and event counts are recorded in `data/report.json`.
