#include "marketstate/reconciler.hpp"

namespace marketstate {

Reconciler::Reconciler(SeqNum first_expected, size_t max_buffer)
    : tracker_(first_expected, max_buffer) {}

ProcessResult Reconciler::submit(const Event& event) { return tracker_.process(event); }

Reconciler::RecoveryOutcome Reconciler::recover(size_t max_events) {
  RecoveryOutcome outcome;
  outcome.report.first_missing = tracker_.next_expected();
  outcome.report.still_missing = tracker_.missing_count();

  if (!tracker_.has_gap()) {
    outcome.report.final_state = tracker_.state();
    return outcome;
  }
  if (source_ == nullptr) {
    tracker_.set_state(StreamState::DEGRADED);
    outcome.report.final_state = tracker_.state();
    return outcome;
  }

  outcome.report.attempted = true;
  tracker_.set_state(StreamState::RECOVERING);

  while (tracker_.has_gap() && outcome.report.events_recovered < max_events) {
    // Earliest missing sequence is always the current expected position when a
    // gap exists at the head of the stream.
    SeqNum missing = tracker_.next_expected();
    std::optional<Event> fetched = source_->fetch(missing);
    if (!fetched) {
      outcome.report.fetch_failures++;
      break;
    }
    ProcessResult r = tracker_.process(*fetched);
    if (r.disposition == Disposition::APPLIED) {
      for (auto& e : r.ready) outcome.ready.push_back(std::move(e));
      outcome.report.events_recovered++;
    } else if (r.disposition == Disposition::DUPLICATE || r.disposition == Disposition::STALE) {
      // Guard against a misbehaving source returning the same event: force
      // progress failure rather than looping forever.
      outcome.report.fetch_failures++;
      break;
    } else {
      break;
    }
  }

  outcome.report.still_missing = tracker_.missing_count();
  if (tracker_.has_gap()) {
    // Could not repair fully: do not pretend the stream is complete.
    tracker_.set_state(StreamState::DEGRADED);
    outcome.report.resolved = false;
  } else {
    tracker_.set_state(StreamState::VERIFIED);
    outcome.report.resolved = true;
  }
  outcome.report.final_state = tracker_.state();
  return outcome;
}

std::vector<Event> Reconciler::sync_from_source(size_t max_events) {
  std::vector<Event> ready;
  if (source_ == nullptr) return ready;

  while (ready.size() < max_events) {
    std::optional<Event> fetched = source_->fetch(tracker_.next_expected());
    if (!fetched) break; // source has nothing further: genuine end (or a hole)
    ProcessResult r = tracker_.process(*fetched);
    if (r.disposition == Disposition::APPLIED) {
      for (auto& e : r.ready) ready.push_back(std::move(e));
    } else {
      // Duplicate/stale/buffered: the source is not advancing our position.
      break;
    }
  }

  if (!tracker_.has_gap()) tracker_.set_state(StreamState::VERIFIED);
  return ready;
}

} // namespace marketstate
