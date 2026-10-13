#include "marketstate/event_log.hpp"
#include "test_helpers.hpp"

#include <gtest/gtest.h>

#include <cstdio>

using namespace marketstate;
using namespace testutil;

namespace {
std::string temp_db() { return "/tmp/marketstate_test_log.sqlite"; }
} // namespace

TEST(SqliteEventLog, AppendReplayAndCheckpointRoundTrip) {
  std::remove(temp_db().c_str());
  SqliteEventLog log(temp_db(), /*synchronous=*/false);
  ASSERT_TRUE(log.open());

  std::vector<Event> events = {
      add(1, 1, "AAPL", Side::BID, 10000, 50),
      add(2, 2, "AAPL", Side::ASK, 10020, 40),
      trade(3, 3, "AAPL", 1, 2, 10010, 10),
      cancel(4, 4, "AAPL", 1),
  };
  ASSERT_TRUE(log.append_batch(events));

  // Regression: replay_all() must not silently return nothing (the default end
  // bound used to wrap to -1 as a signed integer).
  auto all = log.replay_all();
  ASSERT_EQ(all.size(), events.size());
  for (size_t i = 0; i < events.size(); ++i) {
    EXPECT_EQ(encode_event(all[i]), encode_event(events[i]));
  }

  Checkpoint cp;
  cp.last_seq = 4;
  cp.symbol = "__engine__";
  cp.state_data = "STATE AAPL\nSEQ 4\nVOL 0\nTRD 0\nLEVELS 0 0\nEND";
  cp.timestamp = 123;
  ASSERT_TRUE(log.save_checkpoint(cp));
  auto loaded = log.latest_checkpoint("__engine__");
  ASSERT_TRUE(loaded.has_value());
  EXPECT_EQ(loaded->last_seq, 4u);
  EXPECT_EQ(loaded->state_data, cp.state_data);
  log.close();
  std::remove(temp_db().c_str());
}
