#include "marketstate/reconciler.hpp"
#include "marketstate/sequence_tracker.hpp"
#include "test_helpers.hpp"

#include <gtest/gtest.h>

using namespace marketstate;
using namespace testutil;

TEST(SequenceTracker, AppliesInOrder) {
  SequenceTracker t(1);
  for (uint64_t seq = 1; seq <= 5; ++seq) {
    auto r = t.process(add(seq, seq, "AAPL", Side::BID, 10000 + seq, 10));
    EXPECT_EQ(r.disposition, Disposition::APPLIED);
    EXPECT_EQ(r.ready.size(), 1u);
  }
  EXPECT_EQ(t.next_expected(), 6u);
  EXPECT_FALSE(t.has_gap());
}

TEST(SequenceTracker, DetectsDuplicateBySequenceAndId) {
  SequenceTracker t(1);
  t.process(add(1, 1, "AAPL", Side::BID, 10000, 10));
  // Same sequence number, different event id.
  auto dup_seq = t.process(add(99, 1, "AAPL", Side::BID, 10000, 10));
  EXPECT_EQ(dup_seq.disposition, Disposition::DUPLICATE);
  EXPECT_EQ(t.duplicate_count(), 1u);
}

TEST(SequenceTracker, BuffersGapThenDrains) {
  SequenceTracker t(1);
  t.process(add(1, 1, "AAPL", Side::BID, 10000, 10));
  t.process(add(2, 2, "AAPL", Side::BID, 10000, 10));

  auto ahead = t.process(add(4, 4, "AAPL", Side::BID, 10000, 10));
  EXPECT_EQ(ahead.disposition, Disposition::BUFFERED);
  EXPECT_TRUE(t.has_gap());
  ASSERT_EQ(t.gaps().size(), 1u);
  EXPECT_EQ(t.gaps()[0].start, 3u);
  EXPECT_EQ(t.gaps()[0].end, 3u);
  EXPECT_EQ(t.next_expected(), 3u); // no silent advancement

  auto fill = t.process(add(3, 3, "AAPL", Side::BID, 10000, 10));
  EXPECT_EQ(fill.disposition, Disposition::APPLIED);
  ASSERT_EQ(fill.ready.size(), 2u);
  EXPECT_EQ(fill.ready[0].sequence_num, 3u);
  EXPECT_EQ(fill.ready[1].sequence_num, 4u);
  EXPECT_EQ(t.next_expected(), 5u);
  EXPECT_FALSE(t.has_gap());
}

TEST(SequenceTracker, MultipleOutOfOrderDrainAtOnce) {
  SequenceTracker t(1);
  t.process(add(3, 3, "AAPL", Side::BID, 10000, 10));
  t.process(add(4, 4, "AAPL", Side::BID, 10000, 10));
  t.process(add(5, 5, "AAPL", Side::BID, 10000, 10));
  EXPECT_EQ(t.buffered_count(), 3u);
  EXPECT_EQ(t.missing_count(), 2u); // 1, 2

  auto r1 = t.process(add(1, 1, "AAPL", Side::BID, 10000, 10));
  EXPECT_EQ(r1.disposition, Disposition::APPLIED);
  auto r2 = t.process(add(2, 2, "AAPL", Side::BID, 10000, 10));
  EXPECT_EQ(r2.disposition, Disposition::APPLIED);
  ASSERT_EQ(r2.ready.size(), 4u);
  EXPECT_EQ(t.next_expected(), 6u);
  EXPECT_FALSE(t.has_gap());
}

TEST(SequenceTracker, StaleEventIsNotApplied) {
  // Simulate a committed position at 5 with no memory of earlier events (as
  // after a recovery): an unseen older event is stale, never applied.
  SequenceTracker t(5);
  auto stale = t.process(add(500, 3, "AAPL", Side::BID, 10000, 10));
  EXPECT_EQ(stale.disposition, Disposition::STALE);
  EXPECT_EQ(t.stale_count(), 1u);
}

TEST(SequenceTracker, BoundedBufferDegrades) {
  SequenceTracker t(1, /*max_buffer=*/2);
  t.process(add(10, 10, "AAPL", Side::BID, 10000, 10));
  t.process(add(11, 11, "AAPL", Side::BID, 10000, 10));
  auto full = t.process(add(12, 12, "AAPL", Side::BID, 10000, 10));
  EXPECT_EQ(full.disposition, Disposition::BUFFER_FULL);
  EXPECT_EQ(t.state(), StreamState::DEGRADED);
}

TEST(Reconciler, RecoversGapFromMemorySource) {
  MemoryRecoverySource source;
  source.add(add(3, 3, "AAPL", Side::BID, 10000, 10));

  Reconciler r(1);
  r.set_recovery_source(&source);
  r.submit(add(1, 1, "AAPL", Side::BID, 10000, 10));
  r.submit(add(2, 2, "AAPL", Side::BID, 10000, 10));
  r.submit(add(4, 4, "AAPL", Side::BID, 10000, 10)); // gap at 3, buffers 4
  EXPECT_TRUE(r.tracker().has_gap());

  auto outcome = r.recover();
  EXPECT_TRUE(outcome.report.attempted);
  EXPECT_TRUE(outcome.report.resolved);
  ASSERT_EQ(outcome.ready.size(), 2u); // recovered 3 and drained 4
  EXPECT_EQ(outcome.ready[0].sequence_num, 3u);
  EXPECT_EQ(outcome.ready[1].sequence_num, 4u);
  EXPECT_EQ(r.state(), StreamState::VERIFIED);
}

TEST(Reconciler, UnresolvableGapStaysDegraded) {
  MemoryRecoverySource source; // empty: cannot supply event 3
  Reconciler r(1);
  r.set_recovery_source(&source);
  r.submit(add(1, 1, "AAPL", Side::BID, 10000, 10));
  r.submit(add(4, 4, "AAPL", Side::BID, 10000, 10));

  auto outcome = r.recover();
  EXPECT_FALSE(outcome.report.resolved);
  EXPECT_EQ(r.state(), StreamState::DEGRADED);
  EXPECT_TRUE(r.is_degraded());
}

TEST(Reconciler, NoSourceMarksDegraded) {
  Reconciler r(1);
  r.submit(add(1, 1, "AAPL", Side::BID, 10000, 10));
  r.submit(add(3, 3, "AAPL", Side::BID, 10000, 10));
  auto outcome = r.recover();
  EXPECT_FALSE(outcome.report.attempted);
  EXPECT_EQ(r.state(), StreamState::DEGRADED);
}
