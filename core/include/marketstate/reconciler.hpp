#pragma once

#include "event.hpp"
#include "sequence_tracker.hpp"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace marketstate {

// Abstraction over whatever can supply a previously-missed event: a
// retransmission service, a replay buffer, or a trusted snapshot source.
class RecoverySource {
public:
  virtual ~RecoverySource() = default;
  virtual std::optional<Event> fetch(SeqNum seq) = 0;
  virtual const char* name() const = 0;
};

class NullRecoverySource : public RecoverySource {
public:
  std::optional<Event> fetch(SeqNum) override { return std::nullopt; }
  const char* name() const override { return "null"; }
};

class MemoryRecoverySource : public RecoverySource {
public:
  void add(const Event& event) { events_[event.sequence_num] = event; }
  void add_range(const std::vector<Event>& events) {
    for (const auto& e : events) add(e);
  }
  std::optional<Event> fetch(SeqNum seq) override {
    auto it = events_.find(seq);
    if (it == events_.end()) return std::nullopt;
    return it->second;
  }
  const char* name() const override { return "memory"; }
  size_t size() const { return events_.size(); }

private:
  std::map<SeqNum, Event> events_;
};

struct RecoveryReport {
  bool attempted = false;
  bool resolved = false;
  size_t events_recovered = 0;
  size_t fetch_failures = 0;
  SeqNum first_missing = 0;
  SeqNum still_missing = 0;
  StreamState final_state = StreamState::NORMAL;
};

// Wraps a SequenceTracker and drives the recovery state machine. It never
// applies state itself; it returns the events that are safe to apply.
class Reconciler {
public:
  Reconciler(SeqNum first_expected = 1, size_t max_buffer = 10000);

  void set_recovery_source(RecoverySource* source) { source_ = source; }

  ProcessResult submit(const Event& event);

  // Try to fill outstanding gaps from the recovery source. Returns every event
  // that became ready (in order) and a report of what happened.
  struct RecoveryOutcome {
    std::vector<Event> ready;
    RecoveryReport report;
  };
  RecoveryOutcome recover(size_t max_events = 100000);

  // End-of-stream reconciliation. Unlike recover(), this also repairs missing
  // events at the tail of the stream that have no successor to reveal them. It
  // pulls the next expected sequence from the source until the source has
  // nothing more, then returns the events that became ready.
  std::vector<Event> sync_from_source(size_t max_events = 1000000);

  StreamState state() const { return tracker_.state(); }
  bool is_degraded() const {
    return tracker_.state() == StreamState::DEGRADED || tracker_.state() == StreamState::FAILED;
  }

  SequenceTracker& tracker() { return tracker_; }
  const SequenceTracker& tracker() const { return tracker_; }

private:
  SequenceTracker tracker_;
  RecoverySource* source_ = nullptr;
};

} // namespace marketstate
