#include "marketstate/sequence_tracker.hpp"

#include <algorithm>

namespace marketstate {

const char* to_string(StreamState state) {
  switch (state) {
    case StreamState::NORMAL: return "NORMAL";
    case StreamState::GAP_DETECTED: return "GAP_DETECTED";
    case StreamState::RECOVERING: return "RECOVERING";
    case StreamState::REBUILDING: return "REBUILDING";
    case StreamState::VERIFIED: return "VERIFIED";
    case StreamState::DEGRADED: return "DEGRADED";
    case StreamState::FAILED: return "FAILED";
  }
  return "UNKNOWN";
}

const char* to_string(Disposition disposition) {
  switch (disposition) {
    case Disposition::APPLIED: return "APPLIED";
    case Disposition::BUFFERED: return "BUFFERED";
    case Disposition::DUPLICATE: return "DUPLICATE";
    case Disposition::STALE: return "STALE";
    case Disposition::BUFFER_FULL: return "BUFFER_FULL";
  }
  return "UNKNOWN";
}

SequenceTracker::SequenceTracker(SeqNum first_expected, size_t max_buffer, size_t dup_window)
    : next_expected_(first_expected),
      state_(StreamState::NORMAL),
      max_buffer_(max_buffer),
      seen_seqs_(dup_window),
      seen_ids_(dup_window) {}

bool SequenceTracker::is_known(const Event& event) const {
  return seen_seqs_.contains(event.sequence_num) || seen_ids_.contains(event.event_id);
}

void SequenceTracker::remember(const Event& event) {
  seen_seqs_.insert(event.sequence_num);
  seen_ids_.insert(event.event_id);
}

ProcessResult SequenceTracker::process(const Event& event) {
  ProcessResult result;

  if (!event.valid) {
    result.disposition = Disposition::STALE;
    return result;
  }

  if (is_known(event)) {
    duplicate_count_++;
    result.disposition = Disposition::DUPLICATE;
    return result;
  }

  if (event.sequence_num > next_expected_) {
    if (buffer_.size() >= max_buffer_) {
      buffer_full_count_++;
      state_ = StreamState::DEGRADED;
      result.disposition = Disposition::BUFFER_FULL;
      return result;
    }
    remember(event);
    buffer_[event.sequence_num] = event;
    state_ = StreamState::GAP_DETECTED;
    recompute_gaps();
    result.disposition = Disposition::BUFFERED;
    return result;
  }

  if (event.sequence_num < next_expected_) {
    // Older than the committed position and not previously observed.
    stale_count_++;
    result.disposition = Disposition::STALE;
    return result;
  }

  // event.sequence_num == next_expected_ : apply and drain any followers.
  remember(event);
  result.ready.push_back(event);
  next_expected_++;

  auto it = buffer_.find(next_expected_);
  while (it != buffer_.end()) {
    result.ready.push_back(it->second);
    buffer_.erase(it);
    next_expected_++;
    it = buffer_.find(next_expected_);
  }

  recompute_gaps();
  if (gaps_.empty() && (state_ == StreamState::GAP_DETECTED || state_ == StreamState::RECOVERING ||
                        state_ == StreamState::REBUILDING)) {
    state_ = StreamState::NORMAL;
  }
  result.disposition = Disposition::APPLIED;
  return result;
}

void SequenceTracker::recompute_gaps() {
  gaps_.clear();
  if (buffer_.empty()) return;
  SeqNum expected = next_expected_;
  for (const auto& [seq, event] : buffer_) {
    (void)event;
    if (seq > expected) gaps_.push_back(Gap{expected, seq - 1});
    expected = seq + 1;
  }
}

std::vector<SeqNum> SequenceTracker::missing_sequences(size_t limit) const {
  std::vector<SeqNum> missing;
  for (const Gap& gap : gaps_) {
    for (SeqNum s = gap.start; s <= gap.end && missing.size() < limit; ++s) {
      missing.push_back(s);
    }
    if (missing.size() >= limit) break;
  }
  return missing;
}

SeqNum SequenceTracker::missing_count() const {
  SeqNum total = 0;
  for (const Gap& gap : gaps_) total += (gap.end - gap.start + 1);
  return total;
}

void SequenceTracker::set_next_expected(SeqNum seq) { next_expected_ = seq; }

void SequenceTracker::reset(SeqNum first_expected) {
  next_expected_ = first_expected;
  state_ = StreamState::NORMAL;
  buffer_.clear();
  gaps_.clear();
  seen_seqs_.clear();
  seen_ids_.clear();
  duplicate_count_ = 0;
  stale_count_ = 0;
  buffer_full_count_ = 0;
}

} // namespace marketstate
