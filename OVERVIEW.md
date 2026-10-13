# MarketState — Project Guide

> Deterministic real-time market-state reconstruction, recovery, and verification
> engine. This document explains what the project is, why it exists, how it is
> built, how data moves through it, how to run it, and exactly what it outputs.

---

## Table of contents

1. [Purpose and goal](#1-purpose-and-goal)
2. [The problem it solves](#2-the-problem-it-solves)
3. [Feature summary](#3-feature-summary)
4. [Overall architecture](#4-overall-architecture)
5. [Data flow](#5-data-flow)
6. [Current workflow](#6-current-workflow)
7. [Output flow in detail](#7-output-flow-in-detail)
8. [Directory map](#8-directory-map)
9. [Design decisions and guarantees](#9-design-decisions-and-guarantees)

---

## 1. Purpose and goal

MarketState ingests a stream of market events (order adds, cancels, modifies,
trades, market updates) that may arrive **duplicated, delayed, missing,
reordered, or corrupted**, and maintains a **deterministic** representation of
the market (a limit order book per symbol plus activity statistics).

Its real purpose is **not** the order book itself. The order book is only the
application domain. The project is about the **correctness and recovery
infrastructure around it**:

- **Detect** when the incoming stream is unreliable.
- **Reconstruct** a trustworthy state from imperfect data and after crashes.
- **Prove** that the reconstructed state is correct, using independent checks.

The single goal:

> Given a stream that can lose, duplicate, reorder, or corrupt events — and a
> process that can crash — produce the same market state that a clean, perfectly
> ordered history would have produced, **and be able to demonstrate that it did**.

---

## 2. The problem it solves

A financial data system receives events and builds internal state. Delivery is
unreliable:

| Failure | What it looks like | Why it is dangerous |
| --- | --- | --- |
| Duplicate | the same event arrives twice | double-counting trades/orders |
| Gap | an event never arrives | state silently drifts from reality |
| Reorder | events arrive out of order | applying B before A is wrong |
| Corruption | bytes flipped in transit | garbage applied as real data |
| Crash | process dies mid-processing | in-memory state lost / half-applied |

The critical question a trading or risk system must answer at any moment is:

> **"Can I still trust my internal market state?"**

Most systems answer this with a log line that says "recovery complete" — with no
evidence. MarketState is built so the answer is **provable**:

- it never silently advances past a gap,
- it repairs gaps from a retransmission source,
- it survives crashes via a write-ahead log + checkpoints,
- and it compares its rebuilt state **byte-for-byte** against two independent
  reference implementations.

---

## 3. Feature summary

| Area | Feature |
| --- | --- |
| **Integrity** | Whole-record FNV-1a checksum (covers header *and* payload); corrupt events are rejected, never applied |
| **Determinism** | Integer ticks only (no floats), no wall-clock in state, canonical sorted serialization |
| **Duplicate handling** | Bounded FIFO duplicate windows on both sequence number and event id |
| **Reorder handling** | Bounded reorder buffer; events held until predecessors arrive, then drained in order |
| **Gap detection** | Explicit gap set; `next_expected` never advances past a hole |
| **Recovery** | Pluggable retransmission source; repairs interior gaps *and* end-of-stream tail loss |
| **Durability** | Append-only log (in-memory or SQLite) with a **log-first commit protocol** |
| **Crash safety** | Checkpoints + crash hooks covering every crash window |
| **Metrics** | Throughput + log-linear latency histogram (p50/p95/p99/p99.9) + fault accounting |
| **Verification** | Two independent reference models + differential, determinism, recovery, and degradation checks |
| **Tooling** | Reproducible chaos generator, cross-check harness, experiment runner, HTML dashboard |
| **One command** | `./run.sh` builds, tests, demos, verifies, cross-checks, and benchmarks everything |

---

## 4. Overall architecture

### 4.1 High-level picture

```
   clean authoritative stream              fault-injected "wire" stream
              │                                        │
              ▼                                        ▼
 ┌──────────────────────────────────────────────────────────────────┐
 │  C++ engine  (core/)                                              │
 │                                                                   │
 │   Event codec ──▶ Sequence tracker ──▶ Reconciler ──▶ Order books │
 │   validate         dup/gap/reorder      recovery FSM   (state)    │
 │   + checksum           │                     ▲                    │
 │                        ▼                     │ retransmission     │
 │            commit protocol: append log,      │ source             │
 │            then apply to state               │                    │
 │                        │                     │                    │
 │              ┌─────────┴─────────┐           │                    │
 │              ▼                   ▼           │                    │
 │      Durable event log     State engine ─────┘                    │
 │      (SQLite/Memory)       (books + metrics)                      │
 │      + checkpoints                                                │
 │                                                                   │
 │   Metrics: throughput, p50..p99.9 latency, fault accounting       │
 └──────────────────────────────────────────────────────────────────┘
              │                                        ▲
              ▼                                        │
 ┌──────────────────────────────────────────────────────────────────┐
 │  Tooling                                                          │
 │   Python generator + chaos injector                               │
 │   Independent reference models:  Python  +  OCaml  (must agree)   │
 │   verifier · cross-check · experiment runner · HTML dashboard     │
 └──────────────────────────────────────────────────────────────────┘
```

### 4.2 Modules

| Module | File(s) | Responsibility |
| --- | --- | --- |
| Types | `types.hpp` | Integer typedefs (`Price`, `Quantity`, `SeqNum`, …), sentinels |
| Event | `event.hpp/.cpp` | Canonical schema, checksummed text codec, validation |
| Order book | `order_book.hpp/.cpp` | Incremental integer-tick book, canonical serialization/restore |
| Sequence tracker | `sequence_tracker.hpp/.cpp` | Duplicate detection, bounded reorder buffer, gap set |
| Reconciler | `reconciler.hpp/.cpp` | Recovery state machine + `RecoverySource` abstraction |
| Event log | `event_log.hpp/.cpp` | `IEventLog` interface; `MemoryEventLog`, `SqliteEventLog`; checkpoints |
| State engine | `state_engine.hpp/.cpp` | Commit protocol, checkpoint, recovery, crash hooks, metrics wiring |
| Metrics | `metrics.hpp/.cpp` | Throughput + log-linear latency histogram, JSON emission |
| CLI | `main.cpp` | `run`, `replay`, `recover`, `bench` commands |
| Benchmarks | `benchmarks/bench_core.cpp` | Optional Google Benchmark target (off by default) |

### 4.3 The recovery state machine (`Reconciler` + `SequenceTracker`)

```
   NORMAL ──gap──▶ GAP_DETECTED ──request──▶ RECOVERING ──ok──▶ VERIFIED
                        │                        │
                        └────────fail────────────┴──▶ DEGRADED ──▶ FAILED
```

Every incoming event is classified with a **disposition**:

| Disposition | Meaning |
| --- | --- |
| `APPLIED` | Applied now; may include buffered followers that just became contiguous |
| `BUFFERED` | Held because predecessors are missing |
| `DUPLICATE` | Sequence number or event id already seen |
| `STALE` | Older than the committed position and not a known duplicate |
| `BUFFER_FULL` | Reorder buffer exhausted → stream degrades |

The tracker decides *what* is wrong; the reconciler decides *how to repair* it
(via a `RecoverySource`) and emits events that are safe to apply, in order.

### 4.4 The commit protocol (why recovery works)

```
validate ──▶ append to durable log ──▶ apply to in-memory state
```

| Crash point | Log has event? | State has event? | After recovery |
| --- | --- | --- | --- |
| before append | no | no | consistent — nothing to do |
| after append, before apply | **yes** | no | log replay applies it — repaired |
| after apply, before checkpoint | yes | yes | checkpoint is older; replay is idempotent |

This ordering is the whole reason a crash cannot leave state ahead of the log.
The `StateEngine` exposes a `crash_hook` fired at `BEFORE_LOG`,
`AFTER_LOG_BEFORE_APPLY`, and `AFTER_APPLY_BEFORE_CHECKPOINT`; the test suite
throws from each to prove correctness.

---

## 5. Data flow

### 5.1 End-to-end data flow

```
              ┌─────────────────────────────────────────────────────────────┐
              │ 1. GENERATE          tools/generator/marketgen.py           │
              │    one authoritative history ─┬─▶ clean stream  (.txt)      │
              │                               └─▶ faulty stream  (.txt)      │
              │                                   + scenario      (.json)   │
              └─────────────────────────────────────────────────────────────┘
                                              │
              ┌───────────────────────────────┼─────────────────────────────┐
              │ 2. INGEST (C++ engine)         │                            │
              │    faulty stream               │ clean stream (as recovery  │
              │        │                        │ source, if configured)    │
              │        ▼                        ▼                          │
              │   decode + checksum ─▶ sequence tracker ─▶ reconciler      │
              │        │                    │                 │            │
              │        │                    │ gap? ───────────┘            │
              │        │                    ▼                              │
              │        └──▶ commit protocol: append log ─▶ apply to books  │
              │                                   │                         │
              │                                   ▼                         │
              │                        canonical state (text)              │
              └─────────────────────────────────┬───────────────────────────┘
                                                │
              ┌─────────────────────────────────┼───────────────────────────┐
              │ 3. VERIFY                        ▼                          │
              │    reference models (Python, OCaml) build the same state    │
              │    from the clean history independently                     │
              │        │                                                    │
              │        ▼                                                    │
              │    compare byte-for-byte  →  PASS / FAIL                    │
              └─────────────────────────────────────────────────────────────┘
```

### 5.2 Per-event flow inside the engine

```
read line
   │
   ▼
decode_event(line)            ← split, parse fields, verify FNV-1a checksum
   │  (invalid → rejected, counted, never applied)
   ▼
StateEngine::ingest(event)
   │
   ├─ validate_event           ← symbols, ranges, side, arity
   │
   ├─ Reconciler::submit       ← SequenceTracker::process
   │       │
   │       ├─ DUPLICATE / STALE ─────────────▶ counted, dropped
   │       ├─ BUFFERED ──────────────────────▶ held; gap recorded
   │       └─ APPLIED ───────────────────────▶ returns ready=[event,+drained]
   │
   ├─ if gap outstanding: Reconciler::recover()
   │       └─ fetch missing seqs from RecoverySource ─▶ ready events
   │
   └─ commit_and_apply(ready):
           1. append batch to durable log   (durability first)
           2. apply each event to its OrderBook
```

### 5.3 Event text format (the wire + log format)

One event per line. The checksum is **last** and covers every preceding field:

```
event_id  stream_id  seq  symbol  type  timestamp  <payload...>  checksum
```

| `type` | Meaning | Payload |
| ---: | --- | --- |
| 0 | `ORDER_ADD` | `order_id side price quantity` |
| 1 | `ORDER_CANCEL` | `order_id quantity` (`-1` = cancel all remaining) |
| 2 | `ORDER_MODIFY` | `order_id new_price new_quantity` (`-1` = unchanged) |
| 3 | `TRADE` | `buy_order_id sell_order_id price quantity` |
| 4 | `MARKET_UPDATE` | `best_bid best_ask` |

`checksum = FNV-1a64(header + " " + payload)`; `-1` (`kNoPrice`/`kNoQuantity`)
is the "none/unchanged" sentinel. The identical encoding is used by the C++
codec, the Python generator, and both reference models, so all parties agree on
what an event *is*.

---

## 6. Current workflow

### 6.1 The one-command workflow (`./run.sh`)

From the project root (`MarketState/`):

```bash
./run.sh                 # default: 100k-event demo + full experiment sweep
./run.sh 200000          # 200k-event demo stream
```

The script runs six stages:

| Stage | What it does | Output |
| ---: | --- | --- |
| 0 | Check `cmake`, `g++`, `python3` | console |
| 1 | Build the C++ engine (Release) + tests | `build/` |
| 2 | Build the OCaml reference model (skips if OCaml absent) | `reference/ocaml/_build/model` |
| 3 | Run the 34 GoogleTest cases | console |
| 4 | Generate clean + faulty streams; recover; verify | `data/demo_*` |
| 5 | Cross-check engine vs. Python vs. OCaml | console JSON |
| 6 | Full experiment sweep (bench + chaos + recovery) | `data/report.json`, `data/report.html` |

Overrides (environment variables):

```bash
SKIP_EXPERIMENTS=1 ./run.sh          # stages 0–5 only (fast)
SKIP_TESTS=1       ./run.sh          # skip unit tests
SIZES=10000,100000 ./run.sh          # lighter experiment sweep
SEVERITIES=light   ./run.sh          # single chaos severity
REFERENCE=ocaml    ./run.sh          # verify against the OCaml reference
SEED=42            ./run.sh          # repin the generator/experiment seed
```

### 6.2 The manual workflow (stage by stage)

```bash
# build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure

# generate a clean stream + a fault-injected wire stream
python3 tools/generator/marketgen.py --events 100000 --seed 7 \
  --out data/demo_clean.txt --faulty-out data/demo_faulty.txt \
  --scenario-out data/demo_scenario.json \
  --faults "drop=0.03,dup=0.03,reorder=0.05,corrupt=0.02"

# ingest the faulty stream, repairing it from the clean stream
./build/core/marketstate run --events data/demo_faulty.txt \
  --recovery-source data/demo_clean.txt \
  --state-out data/demo_engine.state \
  --metrics-out data/demo_engine.metrics.json

# independent references
python3 reference/python/model.py --events data/demo_clean.txt \
  --state-out data/demo_ref.state
reference/ocaml/build.sh && reference/ocaml/_build/model \
  --events data/demo_clean.txt --state-out data/demo_ref_ml.state

# compare
diff data/demo_engine.state data/demo_ref.state && echo MATCH
tools/analyzer/crosscheck.py --events data/demo_clean.txt   # engine+python+ocaml
```

The stage scripts `scripts/run_demo.sh` and `scripts/run_experiments.sh` wrap
subsets of the above. `./run.sh` is the single front door.

### 6.3 CLI reference

| Command | Purpose |
| --- | --- |
| `marketstate run --events F [--db D] [--checkpoint-interval N] [--recovery-source S] [--state-out O] [--metrics-out M] [--quiet]` | Ingest; optionally persist + checkpoint + repair gaps |
| `marketstate replay --events F [--recovery-source S] [--state-out O] [--metrics-out M] [--quiet]` | Pure deterministic replay (no durability) |
| `marketstate recover --db D [--state-out O] [--recovery-out R]` | Restore from checkpoint + replay log; verify vs. full replay (exit 2 on mismatch) |
| `marketstate bench --events F [--repeat N] [--quiet]` | Throughput + latency percentiles |

---

## 7. Output flow in detail

This section describes **everything the system produces**, from console text to
files to the report dashboard.

### 7.1 Console output of `./run.sh`

```
== 0/6  Checking prerequisites ==
   found cmake
   found g++
   found python3

== 1/6  Building the C++ engine (release) ==
   engine  -> /…/build/core/marketstate

== 2/6  Building the OCaml reference model ==
   OCaml reference ready (two independent models available)

== 3/6  Running unit tests ==
[==========] 34 tests from 8 test suites ran. (3 ms total)
[  PASSED  ] 34 tests.

== 4/6  Demo: generate -> fault -> recover -> verify (100000 events) ==
   generated clean + fault-injected streams
   recovered the faulty stream from the retransmission source
   computed reference state(s)
   verification: PASS (differential, determinism, recovery, honest degradation)

== 5/6  Cross-check engine vs. reference models ==
{
  "events": "…/data/demo_clean.txt",
  "hashes": { "engine": "…", "python": "…", "ocaml": "…" },
  "all_equal": true
}

== 6/6  Experiment sweep (sizes=10000,100000,1000000, severities=light,medium,heavy, reference=python) ==
[bench] 10000 events
[recovery] 10000 events
[chaos] 10000 events / light
…

Done.  Open the report: …/data/report.html
```

### 7.2 Command-level JSON outputs

#### `run` / `replay` → summary line + optional files

`run` prints a one-line summary to stdout (suppressed with `--quiet`), and, if
`--state-out` is given, a confirmation line:

```json
{"command":"run","events":100000,"state_hash":"19b3bfd331e7aca8","state_bytes":228771,
 "metrics":{"events_received":100000,"events_applied":100000, … }}
```
```
state written to data/demo_engine.state (hash 19b3bfd331e7aca8)
```

Files:
- `--state-out` → the **canonical state text** (see §7.3).
- `--metrics-out` → the **metrics JSON** (see §7.4).

#### `recover` → recovery report JSON (+ exit code)

```json
{"used_checkpoint":true,"checkpoint_seq":90000,"events_replayed":10000,
 "resumed_at":100001,"duration_us":8123,
 "recovered_state_hash":"a1b2c3d4e5f60718",
 "full_replay_state_hash":"a1b2c3d4e5f60718",
 "equivalent":true}
```

- `used_checkpoint` — whether a checkpoint was found and loaded (false ⇒ full replay).
- `checkpoint_seq` — the committed stream position stored in the checkpoint.
- `events_replayed` — events replayed from the durable log after the checkpoint.
- `resumed_at` — first sequence number the engine will accept next.
- `recovered_state_hash` / `full_replay_state_hash` — must be equal.
- `equivalent` — the headline result. **The process exits with code 2 if false.**

#### `bench` → metrics JSON per repetition

`bench` prints one metrics JSON object per run (once if `--quiet`).

### 7.3 Canonical state format (`.state` files)

This is the comparison artifact — if two implementations are correct, these
strings are **byte-for-byte identical**. One book per symbol, symbols sorted,
joined by a separator:

```
STATE AAPL
SEQ 100000
VOL 1432500
TRD 19341
LEVELS 37 41
O 12 0 10050 100
O 19 1 10070 250
…
END
===BOOK===
STATE MSFT
…
END
```

| Line | Meaning |
| --- | --- |
| `STATE <symbol>` | book identity |
| `SEQ <n>` | highest applied sequence |
| `VOL <n>` | cumulative traded volume |
| `TRD <n>` | cumulative trade count |
| `LEVELS <bids> <asks>` | number of distinct price levels per side |
| `O <id> <side> <price> <qty>` | one active order, sorted by order id |
| `END` | terminator |

The separator between books is `\n===BOOK===\n`. This exact text is produced by
the C++ engine, the Python model, and the OCaml model.

### 7.4 Metrics JSON schema (`--metrics-out`, `bench`, `run` summary)

```json
{
  "events_received": 100000,
  "events_applied": 100000,
  "events_rejected": 0,
  "duplicates_detected": 0,
  "stale_detected": 0,
  "buffered": 0,
  "buffer_full": 0,
  "gaps_detected": 0,
  "recoveries_attempted": 0,
  "recoveries_succeeded": 0,
  "events_recovered": 0,
  "events_unrecovered": 0,
  "throughput_eps": 838000.0,
  "latency_ns": {
    "count": 100000, "min": 120, "mean": 910.5,
    "p50": 288, "p95": 640, "p99": 2600, "p999": 6100, "max": 42000
  }
}
```

| Field | Meaning |
| --- | --- |
| `events_received` | total events handed to `ingest` |
| `events_applied` | events committed to state (incl. recovered/drained) |
| `events_rejected` | failed validation / checksum |
| `duplicates_detected` | duplicate seq or id |
| `stale_detected` | older than committed position |
| `buffered` / `buffer_full` | reorder-buffer holds / overflows |
| `gaps_detected` | times a gap was observed |
| `recoveries_attempted` / `_succeeded` | recovery attempts / successful ones |
| `events_recovered` / `events_unrecovered` | events repaired / still missing |
| `throughput_eps` | applied events per second (measured window) |
| `latency_ns` | `count,min,mean,p50,p95,p99,p999,max` from the histogram |

The latency histogram is **log-linear** (8 sub-buckets per power of two, ~1/8
relative error), so it spans the whole `int64` nanosecond range without storing
every sample.

### 7.5 Files produced by the demo (stage 4)

| File | Producer | Content |
| --- | --- | --- |
| `data/demo_clean.txt` | generator | authoritative event stream |
| `data/demo_faulty.txt` | generator | same events, fault-injected |
| `data/demo_scenario.json` | generator | exact fault config + counts |
| `data/demo_engine.state` | engine `run` | recovered canonical state |
| `data/demo_engine.metrics.json` | engine `run` | metrics JSON (incl. recovery accounting) |
| `data/demo_ref.state` | Python model | reference canonical state |
| `data/demo_ref_ml.state` | OCaml model | second reference canonical state |
| `data/demo_verify.json` | verifier | the four-check report (see §7.6) |

The generator's scenario file records what was injected, e.g.:

```json
{ "seed": 7, "clean_events": 100000, "wire_events": 99876,
  "dropped": 3049, "duplicated": 2925, "corrupted": 1994,
  "fault_rates": { "drop": 0.03, "dup": 0.03, "reorder": 0.05, "delay": 0.0, "corrupt": 0.02 },
  "dropped_seqs": [35, 47, 58, "…"], "duplicated_seqs": [8, 52, "…"], "corrupted_seqs": [38, 56, "…"] }
```

### 7.6 Verification report (`data/demo_verify.json`)

```json
{
  "reference": "python",
  "checks": {
    "differential_vs_reference":     { "passed": true, "engine_hash": "…", "reference_hash": "…" },
    "deterministic_replay":          { "passed": true, "run1_hash": "…", "run2_hash": "…" },
    "recovery_equivalence":          { "passed": true, "recovered_hash": "…", "reference_hash": "…" },
    "unreliable_without_source_differs": { "passed": true, "degraded_hash": "…" }
  },
  "passed": true
}
```

| Check | Meaning |
| --- | --- |
| `differential_vs_reference` | `engine(clean) == reference(clean)` |
| `deterministic_replay` | `engine(clean)` twice produces identical bytes |
| `recovery_equivalence` | `engine(chaos + retransmit) == reference(clean)` |
| `unreliable_without_source_differs` | with no source, the engine state **must differ** (honest degradation) |

### 7.7 Cross-check output (`tools/analyzer/crosscheck.py`)

```json
{
  "events": "…/data/demo_clean.txt",
  "hashes": { "engine": "b3e7cb0cdce862fb",
              "python": "b3e7cb0cdce862fb",
              "ocaml":  "b3e7cb0cdce862fb" },
  "all_equal": true
}
```

Exits non-zero unless all implementations agree byte-for-byte.

### 7.8 Experiment bundle (`data/report.json`)

Produced by stage 6 (`tools/analyzer/experiment.py`):

```json
{
  "generated_at": "2026-10-13 12:45:01",
  "host": "Linux-…",
  "python": "3.14.7",
  "reference": "python",
  "seed": 42,
  "benchmarks": [
    { "events": 1000000, "applied": 1000000, "throughput_eps": 829000.0,
      "latency_ns": { "p50": 320, "p95": 704, "p99": 2600, "p999": 5600, … } }
  ],
  "chaos": [
    { "events": 1000000, "severity": "heavy",
      "equivalent_to_reference": true,
      "metrics": { "events_recovered": 194335, "events_unrecovered": 0, … },
      "scenario": { "dropped": 80167, "duplicated": 55349, "corrupted": 48810, … } }
  ],
  "recovery": [
    { "events": 1000000, "used_checkpoint": true, "checkpoint_seq": 900000,
      "events_replayed": 100000, "duration_us": 91000, "equivalent": true }
  ]
}
```

### 7.9 HTML dashboard (`data/report.html`)

A self-contained (no network) dark-theme dashboard with three tables derived
from `report.json`:

1. **Throughput & latency (clean stream)** — events, applied, eps, p50/p95/p99/p99.9.
2. **Chaos recovery** — per size/severity: dropped, duplicated, corrupted,
   duplicates detected, events rejected, events recovered, unrecovered, and a
   boolean *equivalent to reference*.
3. **Checkpoint + replay recovery** — used checkpoint, checkpoint seq, events
   replayed, duration, equivalent.

This is the **headline artifact** — open it in a browser after a run:

```bash
xdg-open data/report.html      # or: firefox data/report.html
```

### 7.10 Output flow diagram (summary)

```
 generator ─┬─▶ clean.txt ────────────────┬─▶ Python model ─▶ ref.state ─┐
            │                             └─▶ OCaml model  ─▶ ref_ml.state┤
            └─▶ faulty.txt ─▶ engine run ─▶ engine.state ─────────────────┼─▶ diff/crosscheck
                                 │                                        │        │
                                 └─▶ metrics.json                         │     PASS/FAIL
                                                                          │
 experiment.py ─▶ bench/recovery/chaos ─▶ report.json ─▶ report.html ◀───┘
 verify.py ──────────────────────────────▶ demo_verify.json (4 checks)
```

---

## 8. Directory map

```
MarketState/
├── run.sh                  single entry point: build/test/demo/verify/bench
├── CMakeLists.txt          top-level build
├── README.md               concise overview + results
├── OVERVIEW.md             this document (deep guide)
├── core/
│   ├── include/marketstate/  public headers (types, event, book, tracker, …)
│   ├── src/                  implementation + CLI (main.cpp)
│   ├── tests/                GoogleTest suite (34 cases)
│   └── benchmarks/           optional Google Benchmark target
├── reference/
│   ├── python/model.py       independent reference (Python)
│   └── ocaml/                independent reference (OCaml) + build.sh + README
├── tools/
│   ├── generator/marketgen.py   clean + chaos stream generator
│   └── analyzer/                verify.py, crosscheck.py, experiment.py, reflib.py
├── scripts/                  run_demo.sh, run_experiments.sh
├── config/default.json       example experiment configuration
├── data/                     generated streams, states, metrics, reports
│   ├── report.json / .html   headline experiment outputs
│   └── demo_*                demo-stage outputs
└── docs/                     ARCHITECTURE.md, VERIFICATION.md
```

---

## 9. Design decisions and guarantees

- **Integer ticks, never floats.** Prices and quantities are `int64`. This makes
  replay, checkpoint/recovery, and cross-implementation comparison exact.
- **Checksum covers the whole record.** Corruption anywhere (header or payload)
  is detected; a corrupt event is rejected, never silently applied.
- **Canonical, sorted state.** State is serialized to a stable text form so two
  engines — or an engine and two references — can be compared byte-for-byte.
- **Log-first commit.** Events are durable before they are applied; a crash
  between the two is repaired by log replay.
- **No silent advancement past a gap.** Missing events are buffered and the
  stream is marked `GAP_DETECTED`/`DEGRADED` until repaired — state is never
  relabeled "current" without evidence.
- **Honest degradation.** If an event is permanently lost and no source exists,
  MarketState does not invent it: it reports the unresolved gap and degrades.
- **Independent verification.** Correctness is judged against two
  re-implementations in different languages with different data layouts, not
  against the engine itself.

### How to read a run

1. Stage 3 (tests) must be green.
2. Stage 4 `verification: PASS` means the four correctness checks hold.
3. Stage 5 `all_equal: true` means engine, Python, and OCaml agree exactly.
4. Stage 6 `report.html` shows throughput, latency, and 100% chaos-recovery
   equivalence across all sizes and severities.
