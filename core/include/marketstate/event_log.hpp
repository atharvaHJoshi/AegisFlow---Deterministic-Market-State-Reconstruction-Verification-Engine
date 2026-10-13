#pragma once

#include "event.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace marketstate {

struct Checkpoint {
  SeqNum last_seq = 0;
  std::string symbol;
  std::string state_data; // canonical state produced by OrderBook::canonical_state()
  int64_t timestamp = 0;
};

// Durable, append-only event history plus consistent checkpoints. The commit
// protocol is defined by callers: append events durably, then apply them to
// in-memory state. Recovery replays the log suffix after the latest checkpoint.
class IEventLog {
public:
  virtual ~IEventLog() = default;

  virtual bool open() = 0;
  virtual void close() = 0;
  virtual bool is_open() const = 0;

  virtual bool append(const Event& event) = 0;
  virtual bool append_batch(const std::vector<Event>& events) = 0;

  virtual std::vector<Event> replay_from(SeqNum start_seq,
                                         SeqNum end_seq = UINT64_MAX) = 0;
  virtual std::vector<Event> replay_all() = 0;

  virtual bool save_checkpoint(const Checkpoint& checkpoint) = 0;
  virtual std::optional<Checkpoint> latest_checkpoint(const std::string& symbol) = 0;
  virtual std::vector<Checkpoint> list_checkpoints() = 0;

  virtual void clear() = 0;
};

// In-memory log used for tests and as the fast path in benchmarks.
class MemoryEventLog : public IEventLog {
public:
  bool open() override { open_ = true; return true; }
  void close() override { open_ = false; }
  bool is_open() const override { return open_; }

  bool append(const Event& event) override;
  bool append_batch(const std::vector<Event>& events) override;
  std::vector<Event> replay_from(SeqNum start_seq, SeqNum end_seq = UINT64_MAX) override;
  std::vector<Event> replay_all() override;
  bool save_checkpoint(const Checkpoint& checkpoint) override;
  std::optional<Checkpoint> latest_checkpoint(const std::string& symbol) override;
  std::vector<Checkpoint> list_checkpoints() override;
  void clear() override;

private:
  bool open_ = false;
  std::vector<Event> events_;
  std::vector<Checkpoint> checkpoints_;
};

// SQLite-backed durable log. Stores the canonical text encoding of each event
// so replay is byte-exact with the original stream.
class SqliteEventLog : public IEventLog {
public:
  explicit SqliteEventLog(std::string db_path, bool synchronous = true);
  ~SqliteEventLog() override;

  bool open() override;
  void close() override;
  bool is_open() const override { return open_; }

  bool append(const Event& event) override;
  bool append_batch(const std::vector<Event>& events) override;
  std::vector<Event> replay_from(SeqNum start_seq, SeqNum end_seq = UINT64_MAX) override;
  std::vector<Event> replay_all() override;
  bool save_checkpoint(const Checkpoint& checkpoint) override;
  std::optional<Checkpoint> latest_checkpoint(const std::string& symbol) override;
  std::vector<Checkpoint> list_checkpoints() override;
  void clear() override;

private:
  bool init_schema();

  std::string db_path_;
  sqlite3* db_ = nullptr;
  bool open_ = false;
  bool synchronous_;
};

} // namespace marketstate
