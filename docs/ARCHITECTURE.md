# MarketState Architecture

This document explains the data flow, the commit protocol, and the recovery
state machine in more detail than the README.

## Event model

Every event is a fixed, line-oriented record:

```
event_id stream_id seq symbol type timestamp payload... checksum
```

- `seq` is the stream position. Sequence tracking is scoped to a stream
  (`stream_id`), not to a symbol — a single feed carrying several instruments
  has one monotonic sequence.
- `type` is `0=ORDER_ADD, 1=ORDER_CANCEL, 2=ORDER_MODIFY, 3=TRADE,
  4=MARKET_UPDATE`.
- payload fields use `-1` as a "none / unchanged" sentinel.
- `checksum` is FNV-1a over every preceding field (header *and* payload), so
  corruption anywhere is detected.

The same canonical text is used by the C++ codec, the Python generator, and both
reference models (Python and OCaml), which guarantees that all of them agree on
what an event *is*.

## Order book

The book is deterministic and uses integer ticks:

- `orders_` : active orders by id.
- `bids_`   : `std::map<Price, PriceLevel, std::greater<Price>>`.
- `asks_`   : `std::map<Price, PriceLevel>`.

Price levels are maintained **incrementally** (add/remove deltas on add, cancel,
modify); they are never recomputed from scratch. `best_bid`/`best_ask` are then
`begin()` on the ordered maps. Both reference models deliberately use the
opposite strategy (recompute levels from the order set at serialization time),
so a bug in incremental maintenance shows up as a differential mismatch.

### Canonical state

```
STATE <symbol>
SEQ <last_applied_seq>
VOL <total_volume>
TRD <total_trades>
LEVELS <bid_level_count> <ask_level_count>
O <id> <side> <price> <qty>      (one line per active order, sorted by id)
...
END
```

Multiple symbols are concatenated with a `\n===BOOK===\n` separator, sorted by
symbol. Two implementations are considered correct-and-equal only if these
strings match byte-for-byte.

## Reconciliation

`SequenceTracker::process` returns a `Disposition`:

| Disposition | Meaning |
| --- | --- |
| `APPLIED` | Applied now; may include drained buffered followers |
| `BUFFERED` | Held because predecessors are missing |
| `DUPLICATE` | Sequence or event id already observed |
| `STALE` | Older than the committed position, never seen |
| `BUFFER_FULL` | Reorder buffer exhausted; stream degraded |

Duplicate detection uses bounded FIFO sets (`BoundedSet`) so memory cannot grow
without limit. Gaps are recomputed from the buffered set after each event, and
`next_expected` never advances past a hole.

`Reconciler` wraps the tracker and owns the recovery state machine:

```
NORMAL ──gap──▶ GAP_DETECTED ──request──▶ RECOVERING ──ok──▶ VERIFIED
                     │                          │
                     └──────────fail────────────┴──▶ DEGRADED ──▶ FAILED
```

- `recover()` repairs gaps that are revealed by a successor event.
- `sync_from_source()` (end-of-stream) also repairs **tail losses** — dropped
  events with no successor to expose them — by pulling the next expected
  sequence from the source until it runs dry.

## Commit protocol

```
validate ──▶ append to durable log ──▶ apply to in-memory state
```

This ordering is what makes crash recovery sound:

| Crash point | Log | State | After recovery |
| --- | --- | --- | --- |
| before append | missing event | unchanged | nothing to do — consistent |
| after append, before apply | has event | missing event | log replay applies it — repaired |
| after apply, before checkpoint | has event | has event | checkpoint is older; replay is idempotent |

Recovery is idempotent because it always starts from the checkpoint's
`last_seq + 1` and the checkpoint state exactly represents `last_seq`. Events at
or below the checkpoint are never replayed.

`StateEngine` exposes a `crash_hook` invoked at `BEFORE_LOG`,
`AFTER_LOG_BEFORE_APPLY`, and `AFTER_APPLY_BEFORE_CHECKPOINT`; the GoogleTest
suite throws from these hooks to exercise every window.

## Checkpoints

A checkpoint stores the canonical state of *all* books plus the global committed
sequence. Recovery:

1. load the latest checkpoint;
2. deserialize state (`load_canonical_state`); if it fails, fall back to a full
   replay from an empty state;
3. replay log events with `seq > checkpoint_seq`;
4. reset the tracker to `max_seq + 1` and mark the stream `VERIFIED`.

The invariant is `S_full_replay == S_checkpoint+replay`, checked on every run.

## Metrics

`LatencyHistogram` is a log-linear histogram (8 sub-buckets per power of two,
~1/8 relative error) covering the full `int64` nanosecond range, so p99.9 is
meaningful without storing every sample. `metrics_json()` emits a stable JSON
object consumed by the Python report/dashboard.
