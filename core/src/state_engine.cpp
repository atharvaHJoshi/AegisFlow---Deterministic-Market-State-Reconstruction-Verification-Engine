#include "marketstate/state_engine.hpp"

#include <algorithm>
#include <chrono>
#include <vector>

namespace marketstate {

namespace {

constexpr char kEngineCheckpointSymbol[] = "__engine__";
const std::string kBookSeparator = "\n===BOOK===\n";
const std::string kStatePrefix = "STATE ";

int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void fire(const std::function<void(CrashPoint)>& hook, CrashPoint point) {
  if (hook) hook(point);
}

} // namespace

StateEngine::StateEngine() = default;

OrderBook& StateEngine::get_book(const std::string& symbol) {
  auto it = books_.find(symbol);
  if (it == books_.end()) {
    it = books_.emplace(symbol, OrderBook(symbol)).first;
  }
  return it->second;
}

const OrderBook& StateEngine::get_book(const std::string& symbol) const {
  static const OrderBook empty("");
  auto it = books_.find(symbol);
  if (it == books_.end()) return empty;
  return it->second;
}

IngestResult StateEngine::ingest(const Event& event) {
  IngestResult result;
  metrics_.record_received();

  Event validated = event;
  if (!validate_event(validated)) {
    metrics_.record_rejected();
    result.disposition = Disposition::STALE;
    return result;
  }

  int64_t start = now_ns();
  ProcessResult pr = reconciler_.submit(validated);
  result.disposition = pr.disposition;

  switch (pr.disposition) {
    case Disposition::DUPLICATE:
      metrics_.record_duplicate();
      return result;
    case Disposition::STALE:
      metrics_.record_stale();
      return result;
    case Disposition::BUFFERED:
      metrics_.record_buffered();
      if (reconciler_.tracker().has_gap()) metrics_.record_gap();
      break;
    case Disposition::BUFFER_FULL:
      metrics_.record_buffer_full();
      break;
    case Disposition::APPLIED:
      break;
  }

  if (!pr.ready.empty()) {
    result.applied += commit_and_apply(std::move(pr.ready));
  }

  // If a gap remains, try to repair it from the configured source.
  if (reconciler_.tracker().has_gap()) {
    auto outcome = reconciler_.recover();
    result.recovery_attempted = outcome.report.attempted;
    result.recovered = outcome.report.events_recovered;
    if (!outcome.ready.empty()) {
      result.applied += commit_and_apply(std::move(outcome.ready));
    }
    metrics_.record_recovery_attempt(outcome.report.resolved, outcome.report.events_recovered,
                                     outcome.report.still_missing);
  }

  if (result.applied > 0) {
    int64_t latency = now_ns() - start;
    for (size_t i = 0; i < result.applied; ++i) metrics_.record_applied(latency);
  }
  return result;
}

size_t StateEngine::commit_and_apply(std::vector<Event> events) {
  if (events.empty()) return 0;

  // Durability first.
  fire(crash_hook_, CrashPoint::BEFORE_LOG);
  if (log_ != nullptr && log_->is_open()) {
    log_->append_batch(events);
  }
  fire(crash_hook_, CrashPoint::AFTER_LOG_BEFORE_APPLY);

  // Then in-memory state.
  for (const Event& event : events) {
    get_book(event.symbol).apply_event(event);
  }
  fire(crash_hook_, CrashPoint::AFTER_APPLY_BEFORE_CHECKPOINT);
  return events.size();
}

size_t StateEngine::finalize() {
  std::vector<Event> ready = reconciler_.sync_from_source();
  if (ready.empty()) return 0;
  size_t applied = commit_and_apply(std::move(ready));
  metrics_.record_recovery_attempt(true, applied, 0);
  metrics_.record_applied_count(applied);
  return applied;
}

bool StateEngine::checkpoint(IEventLog& log) {
  fire(crash_hook_, CrashPoint::AFTER_APPLY_BEFORE_CHECKPOINT);
  Checkpoint cp;
  cp.last_seq = reconciler_.tracker().next_expected() - 1;
  cp.symbol = kEngineCheckpointSymbol;
  cp.state_data = serialize_state();
  cp.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::system_clock::now().time_since_epoch())
                     .count();
  return log.save_checkpoint(cp);
}

EngineRecoveryReport StateEngine::recover(IEventLog& log) {
  EngineRecoveryReport report;
  int64_t start = now_ns();

  auto cp = log.latest_checkpoint(kEngineCheckpointSymbol);
  SeqNum start_seq = 1;
  if (cp) {
    report.used_checkpoint = true;
    report.checkpoint_seq = cp->last_seq;
    if (!deserialize_state(cp->state_data)) {
      // Corrupt checkpoint: fall back to a full replay from an empty state.
      report.used_checkpoint = false;
      report.checkpoint_seq = 0;
      books_.clear();
      start_seq = 1;
    } else {
      start_seq = cp->last_seq + 1;
    }
  } else {
    books_.clear();
  }

  auto events = log.replay_from(start_seq);
  SeqNum max_seq = start_seq > 0 ? start_seq - 1 : 0;
  for (const Event& event : events) {
    get_book(event.symbol).apply_event(event);
    max_seq = std::max(max_seq, event.sequence_num);
    report.events_replayed++;
  }

  reconciler_.tracker().reset(max_seq + 1);
  reconciler_.tracker().set_state(StreamState::VERIFIED);
  report.resumed_at = max_seq + 1;
  report.duration_us = (now_ns() - start) / 1000;
  return report;
}

std::string StateEngine::serialize_state() const {
  std::vector<std::string> symbols;
  symbols.reserve(books_.size());
  for (const auto& [symbol, book] : books_) {
    (void)book;
    symbols.push_back(symbol);
  }
  std::sort(symbols.begin(), symbols.end());

  std::string out;
  for (size_t i = 0; i < symbols.size(); ++i) {
    if (i > 0) out += kBookSeparator;
    out += books_.at(symbols[i]).canonical_state();
  }
  return out;
}

bool StateEngine::deserialize_state(const std::string& state) {
  books_.clear();
  size_t pos = 0;
  while (pos <= state.size()) {
    size_t next = state.find(kBookSeparator, pos);
    std::string chunk = (next == std::string::npos) ? state.substr(pos)
                                                    : state.substr(pos, next - pos);
    if (!chunk.empty()) {
      OrderBook book;
      if (!book.load_canonical_state(chunk)) return false;
      books_[book.symbol()] = std::move(book);
    }
    if (next == std::string::npos) break;
    pos = next + kBookSeparator.size();
  }
  return true;
}

std::string StateEngine::canonical_state() const {
  return serialize_state();
}

void StateEngine::reset() {
  books_.clear();
  reconciler_.tracker().reset(1);
  metrics_.metrics().reset();
}

} // namespace marketstate
