# MarketState

**Deterministic Real-Time Market State Reconstruction & Recovery Engine**

MarketState is a fault-tolerant, event-driven market-data processing engine. It
ingests a stream of market events that may be **duplicated, delayed, missing,
reordered, or corrupted**, maintains a deterministic market state (limit order
book + activity statistics), recovers that state after gaps or process crashes,
and **independently verifies** that the result is correct.

It is not a matching engine. The order book is the application domain; the real
project is the correctness and recovery infrastructure around it.

---

## The problem

A financial data system receives events and builds an internal market state.
Delivery, however, is unreliable: the same event may arrive twice, events may
arrive out of order, some may never arrive, and the process may crash with
in-memory state lost. The system must be able to answer:

> Can I still trust my internal market state?

MarketState answers this with three connected capabilities:

1. **Detect** — duplicates, sequence gaps, reordering, corruption.
2. **Reconstruct** — repair gaps from a retransmission source, and restore state
   from a durable log + consistent checkpoint after a crash.
3. **Verify** — prove the reconstructed state equals the state implied by the
   authoritative event history, using an independent reference model.

A system that reports "recovery complete" without proving correctness is not
enough. MarketState provides the proof.

---

## Architecture

```
            clean authoritative stream            fault-injected "wire" stream
                        │                                    │
                        ▼                                    ▼
   ┌───────────────────────────────────────────────────────────────────┐
   │  C++ engine (core/)                                                │
   │                                                                    │
   │  Event codec + validation ──▶ Sequence tracker ──▶ Reconciler      │
   │  (checksum, schema)          (dup/gap/reorder)    (recovery FSM)   │
   │                                        │                  ▲        │
   │                                        ▼                  │        │
   │                 commit protocol: append log, then apply  │ source │
   │                                        │                  │        │
   │             ┌──────────────────────────┴──────────┐       │        │
   │             ▼                                      ▼      │        │
   │        Durable event log (SQLite)            State engine ─┘        │
   │        + checkpoints                          (order books)         │
   │             │                                      │               │
   │             └──────────────► Recovery manager ◀────┘               │
   │                              (checkpoint + replay)                 │
   │                                                                    │
   │  Metrics: throughput, p50/p95/p99/p99.9 latency, fault accounting  │
   └───────────────────────────────────────────────────────────────────┘
                        │                                    ▲
                        ▼                                    │
   ┌───────────────────────────────────────────────────────────────────┐
   │  Tooling                                                            │
   │  Python generator + chaos engine                                    │
   │  Independent reference models: Python  +  OCaml  (must agree)       │
   │  verifier · cross-check · experiment runner · HTML dashboard        │
   └───────────────────────────────────────────────────────────────────┘
```

### Modules

| Module | Responsibility |
| --- | --- |
| `event.hpp/.cpp` | Canonical event schema, checksummed text codec, validation |
| `order_book.hpp/.cpp` | Integer-tick incremental book, canonical state serialization |
| `sequence_tracker.hpp/.cpp` | Duplicate detection, bounded reorder buffer, gap tracking |
| `reconciler.hpp/.cpp` | Recovery state machine, retransmission source abstraction |
| `event_log.hpp/.cpp` | Durable log + checkpoints (`IEventLog`, memory + SQLite) |
| `state_engine.hpp/.cpp` | Commit protocol, checkpoint, recovery, crash hooks |
| `metrics.hpp/.cpp` | Throughput and log-linear latency histograms |
| `main.cpp` | CLI: `run`, `replay`, `recover`, `bench` |

### Key design decisions

- **Integer ticks, never floats.** Prices and quantities are `int64_t`. This
  makes replay, checkpoint/recovery, and cross-implementation comparison exact.
- **Checksum covers the whole record.** Corruption anywhere (header or payload)
  is detected; a corrupt event is rejected, never silently applied.
- **Canonical state.** All state is serialized to a stable, sorted text form so
  two engines — or an engine and the reference model — can be compared
  byte-for-byte.
- **Log-first commit protocol.** Events are appended durably *before* being
  applied in memory. A crash after the append but before the apply is repaired
  on restart by replaying the log; a crash before the append leaves both log and
  state untouched.
- **No silent advancement past a gap.** When sequence 103 is missing, buffered
  events are held and the affected stream is marked `GAP_DETECTED` / `DEGRADED`
  until the gap is repaired. The state is never silently relabeled "current".

---

## System diagrams

The diagrams below use [Mermaid](https://mermaid.js.org/) and render on GitHub and
in Mermaid-aware Markdown viewers.

### System architecture (components)

```mermaid
graph TB
    subgraph EXT["External inputs"]
        GEN["Chaos generator<br/>tools/generator/marketgen.py"]
        CLEAN[["clean authoritative stream<br/>demo_clean.txt"]]
        FAULTY[["fault-injected wire stream<br/>demo_faulty.txt"]]
        SCN[["scenario.json"]]
    end

    subgraph ENGINE["C++ engine — core/ (C++20)"]
        CLI["CLI · main.cpp<br/>run | replay | recover | bench"]
        CODEC["Event codec + validation<br/>FNV-1a checksum · schema"]
        SEQ["Sequence tracker<br/>duplicate · reorder buffer · gaps"]
        RECON["Reconciler<br/>recovery FSM"]
        SE["StateEngine<br/>log-first commit protocol"]
        BOOK["Order books<br/>integer ticks · canonical state"]
        MET["Metrics<br/>throughput + log-linear latency"]
    end

    subgraph DUR["Durability"]
        LOG[("Durable event log<br/>SQLite / memory")]
        CP[("Checkpoints")]
    end

    subgraph REF["Independent reference models"]
        PY["Python · model.py"]
        ML["OCaml · model.ml"]
    end

    subgraph TOOLS["Verification tooling"]
        XCHK["crosscheck.py"]
        VER["verify.py · 4 checks"]
        EXP["experiment.py"]
    end

    subgraph OUT["Outputs"]
        STATE[["canonical .state"]]
        MJSON[["metrics.json"]]
        REPORT[["report.json / report.html"]]
    end

    GEN --> CLEAN
    GEN --> FAULTY
    GEN --> SCN
    CLEAN --> CODEC
    FAULTY --> CODEC
    CLEAN -. retransmission source .-> RECON
    CLI --> SE
    SE --> CODEC
    CODEC --> SEQ
    SEQ --> RECON
    RECON --> SE
    SE --> BOOK
    SE --> MET
    SE --> LOG
    LOG --> CP
    LOG -. replay suffix .-> SE
    BOOK --> STATE
    MET --> MJSON
    CLEAN --> PY
    CLEAN --> ML
    STATE --> XCHK
    PY --> XCHK
    ML --> XCHK
    STATE --> VER
    XCHK --> VER
    VER --> EXP
    EXP --> REPORT
```

### Data flow diagram

**DFD Level 0 — context.** The whole engine as one process with its external
entities.

```mermaid
flowchart LR
    FEED["Market feed /<br/>chaos generator"]
    OP["Operator<br/>(CLI / run.sh)"]
    REF["Independent<br/>reference models"]

    SYS(("0<br/>MarketState<br/>Engine"))

    OP -->|"commands + config"| SYS
    FEED -->|"wire events<br/>+ clean history"| SYS
    SYS -->|"canonical state,<br/>metrics, recovery report"| OP
    SYS -->|"authoritative history"| REF
    REF -->|"reference state"| SYS
    SYS -->|"PASS / FAIL"| OP
```

**DFD Level 1 — major processes and data stores.** Rounded nodes are processes,
cylinders are data stores, rectangles are external entities.

```mermaid
flowchart LR
    FEED["External entity<br/>Market feed / generator"]
    OP["External entity<br/>Operator · CLI"]
    REF["External entity<br/>Reference models"]

    P1(("1.0<br/>Decode &<br/>validate"))
    P2(("2.0<br/>Sequence<br/>track"))
    P3(("3.0<br/>Reconcile &<br/>recover"))
    P4(("4.0<br/>Commit &<br/>apply"))
    P5(("5.0<br/>Checkpoint &<br/>restore"))
    P6(("6.0<br/>Verify"))

    D1[("D1 · Durable<br/>event log")]
    D2[("D2 · Checkpoints")]
    D3[("D3 · Order<br/>books")]
    D4[("D4 · Metrics /<br/>canonical state")]

    OP -->|"run / replay / recover"| P1
    FEED -->|"wire events"| P1
    P1 -->|"valid events"| P2
    P1 -->|"rejects + count"| D4
    P2 -->|"disposition<br/>dup / gap / reorder"| P3
    P3 -->|"retransmit request"| FEED
    FEED -->|"recovered events"| P3
    P3 -->|"ordered ready events"| P4
    P4 -->|"append (log-first)"| D1
    P4 -->|"apply"| D3
    P4 -->|"counters"| D4
    D1 -->|"log suffix"| P5
    D2 -->|"latest checkpoint"| P5
    P5 -->|"restored state"| D3
    OP -->|"verify request"| P6
    D3 -->|"canonical state"| P6
    REF -->|"reference state"| P6
    P6 -->|"PASS / FAIL"| OP
    D4 -->|"state + metrics"| OP
```

### Sequence diagrams

**Ingest path** — validation, sequence tracking, gap recovery, log-first commit.

```mermaid
sequenceDiagram
    autonumber
    actor OP as Operator / CLI
    participant G as Generator
    participant E as StateEngine
    participant T as SequenceTracker
    participant R as Reconciler
    participant S as RecoverySource
    participant L as IEventLog
    participant B as OrderBook
    participant M as Metrics

    OP->>G: marketgen.py --faults ...
    G-->>OP: clean.txt + faulty.txt
    OP->>E: ingest(faulty stream)

    loop each event
        E->>E: validate + FNV-1a checksum
        alt invalid / corrupt
            E->>M: record_rejected()
        else valid
            E->>T: process(event)
            T-->>E: Disposition
            alt DUPLICATE or STALE
                E->>M: record_duplicate / stale
            else BUFFERED · gap outstanding
                E->>M: record_buffered + record_gap
                E->>R: recover()
                R->>S: fetch(missing seq)
                S-->>R: event / nullopt
                R->>T: process(fetched)
                R-->>E: ready events + report
            else APPLIED
                Note over T,E: ready = event + drained followers
            end
            E->>E: commit_and_apply(ready)
            E->>L: append_batch (durability first)
            E->>B: apply_event(event)
            E->>M: record_applied(latency)
        end
    end

    OP->>E: finalize()
    E->>R: sync_from_source()
    R->>S: fetch(next_expected)
    S-->>R: tail events
    E->>L: append_batch
    E->>B: apply_event
    OP->>E: canonical_state()
    E-->>OP: canonical text
```

**Crash recovery path** — `recover --db`, checkpoint restore, log-suffix replay,
and equivalence check against a full replay.

```mermaid
sequenceDiagram
    autonumber
    actor OP as Operator
    participant E as StateEngine
    participant L as SqliteEventLog
    participant B as OrderBook
    participant T as SequenceTracker
    participant F as full_replay_state

    OP->>E: recover(db)
    E->>L: latest_checkpoint("__engine__")
    alt checkpoint exists and is valid
        L-->>E: checkpoint (last_seq, state_data)
        E->>B: deserialize_state(state_data)
        E->>L: replay_from(last_seq + 1)
        L-->>E: log suffix events
        E->>B: apply_event(each)
    else no checkpoint or corrupt
        L-->>E: none / invalid
        E->>B: clear + full replay from seq 1
    end
    E->>T: reset(max_seq + 1)
    E->>T: set_state(VERIFIED)
    E-->>OP: recovery report JSON

    OP->>F: full_replay_state(db)
    F->>L: replay_all()
    L-->>F: full history
    F-->>OP: full replay state
    Note over OP: recovered_hash == full_replay_hash else exit(2)
```

### Flowcharts

**Overall pipeline** — the `./run.sh` stages from build to report.

```mermaid
flowchart TD
    START(["./run.sh"]) --> CHK["Stage 0 · check cmake / g++ / python3"]
    CHK --> BUILD["Stage 1 · build C++ engine + tests"]
    BUILD --> OCB["Stage 2 · build OCaml reference"]
    OCB --> TEST{"Stage 3 · ctest"}
    TEST -->|fail| STOP1(["Abort"])
    TEST -->|pass| GEN["Stage 4a · generate clean + faulty streams"]
    GEN --> RUN["Stage 4b · engine run on faulty<br/>recovery-source = clean"]
    RUN --> REF["Reference models over clean history"]
    REF --> VER{"verify.py · 4 checks pass?"}
    VER -->|no| STOP2(["FAIL"])
    VER -->|yes| XCHK["Stage 5 · crosscheck engine vs python vs ocaml"]
    XCHK --> EQC{"byte-identical?"}
    EQC -->|no| STOP3(["FAIL"])
    EQC -->|yes| EXP["Stage 6 · experiment sweep<br/>bench + chaos + recovery"]
    EXP --> REPORT["report.json + report.html"]
    REPORT --> DONE(["Done"])
```

**Per-event ingestion** — the decision path for a single event line.

```mermaid
flowchart TD
    A([read event line]) --> B{"decode + FNV-1a checksum ok?"}
    B -->|no| R1["reject + count"] --> Z(["next event"])
    B -->|yes| C{"schema / values valid?"}
    C -->|no| R1
    C -->|yes| D["SequenceTracker.process"]
    D --> E{"disposition?"}
    E -->|DUPLICATE| R2["count duplicate"] --> Z
    E -->|STALE| R3["count stale"] --> Z
    E -->|BUFFERED| F["hold event<br/>record gap"]
    E -->|BUFFER_FULL| H["degrade stream<br/>BUFFER_FULL"] --> Z
    E -->|APPLIED| I["ready = event + drained followers"]
    F --> G["Reconciler.recover()"]
    G --> J{"gap repaired?"}
    J -->|yes| I
    J -->|no| K["mark DEGRADED"] --> Z
    I --> L["commit_and_apply(ready)"]
    L --> M["append to durable log"]
    M --> N["apply to order books"]
    N --> O["record metrics"] --> Z
```

---

## Build

Requirements: CMake ≥ 3.20, a C++20 compiler, SQLite3, GoogleTest (for tests),
Python 3 (stdlib only). OCaml is optional and only needed for the second
independent reference model (`reference/ocaml/`); see its README for install
options, including a rootless user-local extraction.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure      # or: ./build/core/marketstate_tests
```

## Quick start

One command builds everything, runs the tests, generates a fault-injected
stream, recovers it, verifies against the independent reference models, and
produces the performance report:

```bash
./run.sh                 # full run (default 100k-event demo, full sweep)
./run.sh 200000          # full run with a 200k-event demo stream
```

Useful overrides:

```bash
SKIP_EXPERIMENTS=1 ./run.sh          # build + test + demo + cross-check only
SKIP_TESTS=1       ./run.sh          # skip the unit-test step
SIZES=10000,100000 ./run.sh          # lighter experiment sweep
SEVERITIES=light   ./run.sh          # single chaos severity
REFERENCE=ocaml    ./run.sh          # verify against the OCaml reference model
```

The headline output is `data/report.html`. The individual stages can also be run
by hand:

```bash
# 1. Generate a clean stream and a corrupted/reordered/duplicated one.
python3 tools/generator/marketgen.py --events 100000 --seed 7 \
  --out data/demo_clean.txt --faulty-out data/demo_faulty.txt \
  --scenario-out data/demo_scenario.json \
  --faults "drop=0.03,dup=0.03,reorder=0.05,corrupt=0.02"

# 2. Ingest the faulty stream and recover it using the clean stream as the
#    retransmission source.
./build/core/marketstate run --events data/demo_faulty.txt \
  --recovery-source data/demo_clean.txt \
  --state-out data/demo_engine.state --metrics-out data/demo_engine.metrics.json

# 3. Independent reference models over the authoritative history.
python3 reference/python/model.py --events data/demo_clean.txt \
  --state-out data/demo_ref.state
reference/ocaml/build.sh && reference/ocaml/_build/model \
  --events data/demo_clean.txt --state-out data/demo_ref_ml.state

# 4. Engine and both references must match exactly.
diff data/demo_engine.state data/demo_ref.state && \
diff data/demo_engine.state data/demo_ref_ml.state && echo "VERIFIED"
```

The individual stage scripts remain available:

```bash
scripts/run_demo.sh          # generate → fault → recover → verify
scripts/run_experiments.sh   # full sweep + HTML dashboard (data/report.html)
```

## CLI

| Command | Purpose |
| --- | --- |
| `marketstate run --events F [--db D] [--checkpoint-interval N] [--recovery-source S]` | Ingest, optionally log + checkpoint, optionally repair gaps |
| `marketstate replay --events F` | Pure deterministic replay (no durability) |
| `marketstate bench --events F --repeat N` | Throughput + latency percentiles |
| `marketstate recover --db D` | Restore from checkpoint + replay log suffix, and verify equivalence |

## Independent reference models

Correctness is not asserted against a copy of the engine — it is checked against
independent re-implementations that share no code with it:

| Reference | Language | Strategy |
| --- | --- | --- |
| `reference/python/model.py` | Python | levels recomputed from the raw order set |
| `reference/ocaml/model.ml` | OCaml | same semantics, typed variant/`Hashtbl` impl |

Both consume the same authoritative history and emit the same canonical text as
the C++ engine. `tools/analyzer/crosscheck.py` runs all three and fails unless
their states are **byte-identical**:

```bash
tools/analyzer/crosscheck.py --events data/demo_clean.txt
```

```
{ "hashes": { "engine": "b3e7cb0c…", "python": "b3e7cb0c…", "ocaml": "b3e7cb0c…" },
  "all_equal": true }
```

The verifier can be run against either reference with
`verify.py --reference {python,ocaml}`; `experiment.py` accepts the same flag.

## Verification

`tools/analyzer/verify.py` runs four checks:

1. **Differential correctness** — `engine(clean) == reference(clean)`.
2. **Deterministic replay** — running the same history twice yields identical state.
3. **Recovery equivalence** — `engine(chaos + retransmit) == reference(clean)`
   (i.e. `S_checkpoint+replay == S_full replay`).
4. **Honest degradation** — with a genuinely unreliable stream and no recovery
   source, the engine state differs from the authoritative reference (it does
   not pretend to be correct).

## Results (this machine)

Reproducible via `scripts/run_experiments.sh` (seed 42). Full data in
`data/report.json` / `data/report.html`.

**Throughput & latency (single thread, clean stream):**

| Events | Throughput | p50 | p95 | p99 | p99.9 |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 10 000 | ~817 K eps | 256 ns | 512 ns | 2.3 µs | 15.4 µs |
| 100 000 | ~838 K eps | 288 ns | 640 ns | 2.6 µs | 6.1 µs |
| 1 000 000 | ~829 K eps | 320 ns | 704 ns | 2.6 µs | 5.6 µs |

**Chaos recovery — final state equality vs. independent reference model, 1M events:**

| Severity | Dropped | Duplicated | Corrupted | Recovered | Unresolved | Equivalent |
| --- | ---: | ---: | ---: | ---: | ---: | :---: |
| light | 10 054 | 9 801 | 5 028 | 33 879 | 0 | ✅ |
| medium | 30 205 | 29 229 | 20 027 | 91 907 | 0 | ✅ |
| heavy | 80 167 | 55 349 | 48 810 | 194 335 | 0 | ✅ |

Every scenario (10K/100K/1M × light/medium/heavy) recovers to a byte-identical
state. Checkpoint + replay recovery is equivalent to full replay at all sizes.

## Project layout

```
MarketState/
├── run.sh              one-command build / test / demo / verify / benchmark
├── OVERVIEW.md         full project guide: purpose, architecture, data & output flow
├── core/               C++20 engine + GoogleTest suite
├── reference/python/   independent reference model (Python)
├── reference/ocaml/    independent reference model (OCaml)
├── tools/generator/    event generator + chaos injector
├── tools/analyzer/     verifier, cross-check, experiment runner, HTML dashboard
├── scripts/            demo + experiment runners
├── config/             example configuration
├── data/               generated streams, logs, reports
└── docs/               architecture and verification notes
```

## Scope and honesty

Recovery requires *sufficient information*: a retransmission source or an
authoritative snapshot. If an event is permanently lost and no source exists,
MarketState does not invent it — it reports the unresolved gap and marks the
state `DEGRADED`. Every quoted number is produced by the actual implementation
and the exact configuration is stored alongside the run.
