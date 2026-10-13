#pragma once

#include "event.hpp"

#include <deque>
#include <map>
#include <string>
#include <unordered_set>
#include <vector>

namespace marketstate {

enum class StreamState {
  NORMAL,
  GAP_DETECTED,
  RECOVERING,
  REBUILDING,
  VERIFIED,
  DEGRADED,
  FAILED,
};

const char* to_string(StreamState state);

// What the sequence tracker decided about one incoming event.
enum class Disposition {
  APPLIED,     // applied now (and possibly drained followers)
  BUFFERED,    // held until missing predecessors arrive
  DUPLICATE,   // already observed sequence or event id
  STALE,       // older than the committed position and not a known duplicate
  BUFFER_FULL, // reorder buffer exhausted; stream is degraded
};

const char* to_string(Disposition disposition);

struct Gap {
  SeqNum start = 0;
  SeqNum end = 0; // inclusive
};

struct ProcessResult {
  Disposition disposition = Disposition::STALE;
  // Events that are now safe to apply, in sequence order. For APPLIED this
  // includes the incoming event and any buffered events that became contiguous.
  std::vector<Event> ready;
};

// Bounded, deterministic duplicate detector. Remembers the most recent `capacity`
// identities; older entries are evicted FIFO so memory cannot grow without bound.
class BoundedSet {
public:
  explicit BoundedSet(size_t capacity = 1000000) : capacity_(capacity) {}

  bool contains(uint64_t value) const { return set_.count(value) != 0; }

  void insert(uint64_t value) {
    if (set_.count(value)) return;
    set_.insert(value);
    order_.push_back(value);
    if (order_.size() > capacity_) {
      set_.erase(order_.front());
      order_.pop_front();
    }
  }

  void clear() {
    set_.clear();
    order_.clear();
  }

  size_t size() const { return set_.size(); }

private:
  size_t capacity_;
  std::unordered_set<uint64_t> set_;
  std::deque<uint64_t> order_;
};

// Tracks the expected sequence for a single stream, buffers limited reordering,
// detects duplicates, and reports outstanding gaps. It does not decide how to
// repair a gap; that is the Reconciler's job.
class SequenceTracker {
public:
  SequenceTracker(SeqNum first_expected = 1, size_t max_buffer = 10000,
                  size_t dup_window = 1000000);

  ProcessResult process(const Event& event);

  SeqNum next_expected() const { return next_expected_; }
  void set_next_expected(SeqNum seq);

  StreamState state() const { return state_; }
  void set_state(StreamState state) { state_ = state; }

  bool has_gap() const { return !gaps_.empty(); }
  const std::vector<Gap>& gaps() const { return gaps_; }

  // Up to `limit` concrete missing sequence numbers, earliest first.
  std::vector<SeqNum> missing_sequences(size_t limit) const;
  SeqNum missing_count() const;

  size_t buffered_count() const { return buffer_.size(); }
  size_t duplicate_count() const { return duplicate_count_; }
  size_t stale_count() const { return stale_count_; }
  size_t buffer_full_count() const { return buffer_full_count_; }

  void reset(SeqNum first_expected = 1);

private:
  bool is_known(const Event& event) const;
  void remember(const Event& event);
  void recompute_gaps();

  SeqNum next_expected_;
  StreamState state_;
  size_t max_buffer_;
  std::map<SeqNum, Event> buffer_;
  std::vector<Gap> gaps_;
  BoundedSet seen_seqs_;
  BoundedSet seen_ids_;
  size_t duplicate_count_ = 0;
  size_t stale_count_ = 0;
  size_t buffer_full_count_ = 0;
};

} // namespace marketstate
