#include "marketstate/event_log.hpp"
#include "marketstate/state_engine.hpp"
#include "test_helpers.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <vector>

using namespace marketstate;
using namespace testutil;

namespace {

std::vector<Event> make_stream(size_t n, const std::string& sym = "AAPL") {
  std::vector<Event> events;
  for (uint64_t seq = 1; seq <= n; ++seq) {
    uint64_t id = seq;
    if (seq % 4 == 0) {
      events.push_back(add(id, seq, sym, Side::ASK, 10020 + (seq % 5), 40));
    } else if (seq % 7 == 0) {
      events.push_back(cancel(id, seq, sym, seq - 1));
    } else if (seq % 5 == 0) {
      events.push_back(trade(id, seq, sym, seq - 1, seq, 10010, 5));
    } else if (seq % 3 == 0) {
      events.push_back(modify(id, seq, sym, seq - 1, kNoPrice, 15 + (seq % 10)));
    } else {
      events.push_back(add(id, seq, sym, Side::BID, 10000 - (seq % 3), 10 + (seq % 20)));
    }
  }
  return events;
}

struct SimulatedCrash : std::exception {
  const char* what() const noexcept override { return "simulated crash"; }
};

} // namespace

TEST(Determinism, SameStreamProducesIdenticalState) {
  auto events = make_stream(200);
  StateEngine a;
  StateEngine b;
  for (const Event& e : events) a.ingest(e);
  for (const Event& e : events) b.ingest(e);
  EXPECT_EQ(a.canonical_state(), b.canonical_state());
  EXPECT_EQ(a.metrics().metrics().events_applied, b.metrics().metrics().events_applied);
}

TEST(Determinism, NoSilentAdvancementPastGap) {
  StateEngine engine;
  engine.ingest(add(1, 1, "AAPL", Side::BID, 10000, 10));
  engine.ingest(add(2, 2, "AAPL", Side::BID, 10000, 10));
  engine.ingest(add(4, 4, "AAPL", Side::BID, 10000, 10)); // 3 missing
  EXPECT_EQ(engine.get_book("AAPL").last_applied_seq(), 2u);
  EXPECT_TRUE(engine.reconciler().tracker().has_gap());
}

TEST(Recovery, CheckpointPlusReplayEqualsFullReplay) {
  auto events = make_stream(500);
  MemoryEventLog log;
  log.open();

  StateEngine engine;
  engine.attach_log(&log);
  for (size_t i = 0; i < events.size(); ++i) {
    engine.ingest(events[i]);
    if (i % 100 == 99) engine.checkpoint(log);
  }
  engine.checkpoint(log);

  StateEngine recovered;
  auto report = recovered.recover(log);
  EXPECT_TRUE(report.used_checkpoint);
  EXPECT_GT(report.checkpoint_seq, 0u);

  // Independent full replay from the durable log.
  StateEngine full;
  for (const Event& e : log.replay_all()) full.get_book(e.symbol).apply_event(e);

  EXPECT_EQ(recovered.canonical_state(), full.canonical_state());
}

TEST(Recovery, CrashAfterLogBeforeApplyIsRepaired) {
  auto events = make_stream(50);
  MemoryEventLog log;
  log.open();

  StateEngine engine;
  engine.attach_log(&log);
  int commits = 0;
  engine.set_crash_hook([&](CrashPoint p) {
    if (p == CrashPoint::AFTER_LOG_BEFORE_APPLY) {
      if (++commits == 20) throw SimulatedCrash{};
    }
  });

  try {
    for (const Event& e : events) engine.ingest(e);
    FAIL() << "expected a simulated crash";
  } catch (const SimulatedCrash&) {
    // Crash after event 20 was durably logged but before it was applied.
  }

  // Restart into a fresh engine; the durable log is the source of truth.
  StateEngine restarted;
  restarted.recover(log);

  StateEngine full;
  for (const Event& e : log.replay_all()) full.get_book(e.symbol).apply_event(e);
  EXPECT_EQ(restarted.canonical_state(), full.canonical_state());
}

TEST(Recovery, CrashBeforeLogLeavesLogAndStateConsistent) {
  auto events = make_stream(50);
  MemoryEventLog log;
  log.open();

  StateEngine engine;
  engine.attach_log(&log);
  int commits = 0;
  engine.set_crash_hook([&](CrashPoint p) {
    if (p == CrashPoint::BEFORE_LOG) {
      if (++commits == 10) throw SimulatedCrash{};
    }
  });

  try {
    for (const Event& e : events) engine.ingest(e);
    FAIL() << "expected a simulated crash";
  } catch (const SimulatedCrash&) {
  }

  // Event 10 was never logged and never applied: recovery replays 1..9 only.
  StateEngine restarted;
  restarted.recover(log);
  EXPECT_EQ(restarted.get_book("AAPL").last_applied_seq(), 9u);
}

TEST(Recovery, CorruptCheckpointFallsBackToFullReplay) {
  auto events = make_stream(100);
  MemoryEventLog log;
  log.open();
  StateEngine engine;
  engine.attach_log(&log);
  for (const Event& e : events) engine.ingest(e);
  engine.checkpoint(log);

  // Overwrite with a deliberately corrupt checkpoint for the engine symbol.
  Checkpoint bad;
  bad.last_seq = 999;
  bad.symbol = "__engine__";
  bad.state_data = "STATE AAPL\nGARBAGE LINE\nEND";
  bad.timestamp = 0;
  ASSERT_TRUE(log.save_checkpoint(bad));

  StateEngine recovered;
  auto report = recovered.recover(log);
  EXPECT_FALSE(report.used_checkpoint);

  StateEngine full;
  for (const Event& e : log.replay_all()) full.get_book(e.symbol).apply_event(e);
  EXPECT_EQ(recovered.canonical_state(), full.canonical_state());
}

TEST(Recovery, FinalizeRepairsTailLossWithoutSuccessor) {
  auto clean = make_stream(20);
  MemoryRecoverySource source;
  source.add_range(clean);

  StateEngine engine;
  engine.attach_recovery_source(&source);
  // Deliver only 1..15; events 16..20 are dropped at the tail and have no
  // successor to reveal the gap during ingestion.
  for (size_t i = 0; i < 15; ++i) engine.ingest(clean[i]);
  EXPECT_EQ(engine.get_book("AAPL").last_applied_seq(), 15u);

  size_t applied = engine.finalize();
  EXPECT_EQ(applied, 5u);
  EXPECT_EQ(engine.get_book("AAPL").last_applied_seq(), 20u);
}

TEST(Recovery, DoesNotReapplyEventsAfterRestart) {
  auto events = make_stream(30);
  MemoryEventLog log;
  log.open();
  StateEngine engine;
  engine.attach_log(&log);
  for (const Event& e : events) engine.ingest(e);

  StateEngine restarted;
  restarted.attach_log(&log);
  restarted.recover(log);
  std::string before = restarted.canonical_state();

  // A late copy of an already-applied event must not be applied again. After a
  // restart the tracker has no memory of the sequence, so it is classified as
  // stale (never applied) rather than duplicate.
  auto dup = restarted.ingest(events[9]);
  EXPECT_NE(dup.disposition, Disposition::APPLIED);
  EXPECT_EQ(restarted.canonical_state(), before);
}
