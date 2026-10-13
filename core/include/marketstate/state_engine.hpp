#pragma once

#include "event_log.hpp"
#include "metrics.hpp"
#include "order_book.hpp"
#include "reconciler.hpp"

#include <functional>
#include <string>
#include <unordered_map>

namespace marketstate {

struct IngestResult {
  Disposition disposition = Disposition::STALE;
  size_t applied = 0;
  bool recovery_attempted = false;
  size_t recovered = 0;
};

struct EngineRecoveryReport {
  bool used_checkpoint = false;
  SeqNum checkpoint_seq = 0;
  size_t events_replayed = 0;
  SeqNum resumed_at = 1;
  int64_t duration_us = 0;
  bool ok = true;
};

enum class CrashPoint {
  NONE,
  BEFORE_LOG,
  AFTER_LOG_BEFORE_APPLY,
  AFTER_APPLY_BEFORE_CHECKPOINT,
};

// Deterministic state engine. The commit protocol is log-first:
//   validate -> append to durable log -> apply to in-memory state.
// A crash after the append but before the apply is repaired on restart because
// the event is replayed from the log. A crash before the append leaves both the
// log and the state untouched, so they stay consistent.
class StateEngine {
public:
  StateEngine();

  void attach_log(IEventLog* log) { log_ = log; }
  void attach_recovery_source(RecoverySource* source) { reconciler_.set_recovery_source(source); }

  IngestResult ingest(const Event& event);

  // End-of-stream reconciliation: pull the remaining expected events from the
  // recovery source (including tail losses) and commit them.
  size_t finalize();

  // Global checkpoint covering every book plus the committed stream position.
  bool checkpoint(IEventLog& log);

  // Restore from the latest checkpoint and replay the durable log suffix.
  EngineRecoveryReport recover(IEventLog& log);

  // Canonical, stable representation of all books (used for verification).
  std::string canonical_state() const;

  OrderBook& get_book(const std::string& symbol);
  const OrderBook& get_book(const std::string& symbol) const;
  const std::unordered_map<std::string, OrderBook>& books() const { return books_; }

  Reconciler& reconciler() { return reconciler_; }
  const Reconciler& reconciler() const { return reconciler_; }

  MetricsCollector& metrics() { return metrics_; }
  const MetricsCollector& metrics() const { return metrics_; }

  void reset();
  void set_crash_hook(std::function<void(CrashPoint)> hook) { crash_hook_ = std::move(hook); }

private:
  size_t commit_and_apply(std::vector<Event> events);
  std::string serialize_state() const;
  bool deserialize_state(const std::string& state);

  std::unordered_map<std::string, OrderBook> books_;
  Reconciler reconciler_;
  MetricsCollector metrics_;
  IEventLog* log_ = nullptr;
  std::function<void(CrashPoint)> crash_hook_;
};

} // namespace marketstate
