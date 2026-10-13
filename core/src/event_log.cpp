#include "marketstate/event_log.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <limits>

namespace marketstate {

// ---------------------------------------------------------------------------
// MemoryEventLog
// ---------------------------------------------------------------------------

bool MemoryEventLog::append(const Event& event) {
  if (!open_) return false;
  events_.push_back(event);
  return true;
}

bool MemoryEventLog::append_batch(const std::vector<Event>& events) {
  if (!open_) return false;
  events_.insert(events_.end(), events.begin(), events.end());
  return true;
}

std::vector<Event> MemoryEventLog::replay_from(SeqNum start_seq, SeqNum end_seq) {
  std::vector<Event> out;
  for (const auto& e : events_) {
    if (e.sequence_num >= start_seq && e.sequence_num <= end_seq) out.push_back(e);
  }
  std::sort(out.begin(), out.end(),
            [](const Event& a, const Event& b) { return a.sequence_num < b.sequence_num; });
  return out;
}

std::vector<Event> MemoryEventLog::replay_all() { return replay_from(0); }

bool MemoryEventLog::save_checkpoint(const Checkpoint& checkpoint) {
  if (!open_) return false;
  checkpoints_.push_back(checkpoint);
  return true;
}

std::optional<Checkpoint> MemoryEventLog::latest_checkpoint(const std::string& symbol) {
  for (auto it = checkpoints_.rbegin(); it != checkpoints_.rend(); ++it) {
    if (it->symbol == symbol) return *it;
  }
  return std::nullopt;
}

std::vector<Checkpoint> MemoryEventLog::list_checkpoints() {
  std::vector<Checkpoint> out(checkpoints_.rbegin(), checkpoints_.rend());
  return out;
}

void MemoryEventLog::clear() {
  events_.clear();
  checkpoints_.clear();
}

// ---------------------------------------------------------------------------
// SqliteEventLog
// ---------------------------------------------------------------------------

SqliteEventLog::SqliteEventLog(std::string db_path, bool synchronous)
    : db_path_(std::move(db_path)), synchronous_(synchronous) {}

SqliteEventLog::~SqliteEventLog() { close(); }

bool SqliteEventLog::open() {
  if (open_) return true;
  if (sqlite3_open(db_path_.c_str(), &db_) != SQLITE_OK) {
    if (db_) {
      sqlite3_close(db_);
      db_ = nullptr;
    }
    return false;
  }
  open_ = true;
  if (!init_schema()) {
    close();
    return false;
  }
  std::string pragma = "PRAGMA journal_mode=WAL;";
  sqlite3_exec(db_, pragma.c_str(), nullptr, nullptr, nullptr);
  pragma = synchronous_ ? "PRAGMA synchronous=FULL;" : "PRAGMA synchronous=NORMAL;";
  sqlite3_exec(db_, pragma.c_str(), nullptr, nullptr, nullptr);
  return true;
}

void SqliteEventLog::close() {
  if (db_) {
    sqlite3_close(db_);
    db_ = nullptr;
  }
  open_ = false;
}

bool SqliteEventLog::init_schema() {
  const char* schema = R"(
    CREATE TABLE IF NOT EXISTS events (
      event_id     INTEGER NOT NULL,
      stream_id    INTEGER NOT NULL,
      sequence_num INTEGER NOT NULL,
      symbol       TEXT NOT NULL,
      event_type   INTEGER NOT NULL,
      timestamp_ns INTEGER NOT NULL,
      data         TEXT NOT NULL,
      PRIMARY KEY (stream_id, sequence_num)
    );
    CREATE INDEX IF NOT EXISTS idx_events_seq ON events(sequence_num);
    CREATE INDEX IF NOT EXISTS idx_events_symbol ON events(symbol);
    CREATE TABLE IF NOT EXISTS checkpoints (
      id         INTEGER PRIMARY KEY AUTOINCREMENT,
      last_seq   INTEGER NOT NULL,
      symbol     TEXT NOT NULL,
      state_data TEXT NOT NULL,
      timestamp  INTEGER NOT NULL
    );
  )";
  char* err = nullptr;
  int rc = sqlite3_exec(db_, schema, nullptr, nullptr, &err);
  if (rc != SQLITE_OK) {
    sqlite3_free(err);
    return false;
  }
  return true;
}

bool SqliteEventLog::append(const Event& event) {
  return append_batch({event});
}

bool SqliteEventLog::append_batch(const std::vector<Event>& events) {
  if (!open_ || events.empty()) return open_;
  const char* sql =
      "INSERT OR REPLACE INTO events "
      "(event_id, stream_id, sequence_num, symbol, event_type, timestamp_ns, data) "
      "VALUES (?, ?, ?, ?, ?, ?, ?)";
  sqlite3_exec(db_, "BEGIN", nullptr, nullptr, nullptr);
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
    return false;
  }
  bool ok = true;
  for (const auto& event : events) {
    std::string encoded = encode_event(event);
    sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(event.event_id));
    sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(event.stream_id));
    sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(event.sequence_num));
    sqlite3_bind_text(stmt, 4, event.symbol.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 5, static_cast<int>(event.type));
    sqlite3_bind_int64(stmt, 6, static_cast<sqlite3_int64>(event.timestamp));
    sqlite3_bind_text(stmt, 7, encoded.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
      ok = false;
      break;
    }
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
  }
  sqlite3_finalize(stmt);
  sqlite3_exec(db_, ok ? "COMMIT" : "ROLLBACK", nullptr, nullptr, nullptr);
  return ok;
}

std::vector<Event> SqliteEventLog::replay_from(SeqNum start_seq, SeqNum end_seq) {
  std::vector<Event> out;
  if (!open_) return out;
  const char* sql =
      "SELECT data FROM events WHERE sequence_num >= ? AND sequence_num <= ? "
      "ORDER BY sequence_num";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return out;
  // UINT64_MAX must not be bound directly: it wraps to -1 as a signed int64 and
  // would match nothing. Clamp to the largest positive SQLite integer.
  const sqlite3_int64 end_bound =
      end_seq > static_cast<uint64_t>(std::numeric_limits<sqlite3_int64>::max())
          ? std::numeric_limits<sqlite3_int64>::max()
          : static_cast<sqlite3_int64>(end_seq);
  sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(start_seq));
  sqlite3_bind_int64(stmt, 2, end_bound);
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const unsigned char* text = sqlite3_column_text(stmt, 0);
    if (!text) continue;
    auto decoded = decode_event(reinterpret_cast<const char*>(text));
    if (decoded) out.push_back(*decoded);
  }
  sqlite3_finalize(stmt);
  return out;
}

std::vector<Event> SqliteEventLog::replay_all() { return replay_from(0); }

bool SqliteEventLog::save_checkpoint(const Checkpoint& checkpoint) {
  if (!open_) return false;
  const char* sql =
      "INSERT INTO checkpoints (last_seq, symbol, state_data, timestamp) VALUES (?, ?, ?, ?)";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(checkpoint.last_seq));
  sqlite3_bind_text(stmt, 2, checkpoint.symbol.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, checkpoint.state_data.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 4, static_cast<sqlite3_int64>(checkpoint.timestamp));
  bool ok = sqlite3_step(stmt) == SQLITE_DONE;
  sqlite3_finalize(stmt);
  return ok;
}

std::optional<Checkpoint> SqliteEventLog::latest_checkpoint(const std::string& symbol) {
  if (!open_) return std::nullopt;
  const char* sql =
      "SELECT last_seq, symbol, state_data, timestamp FROM checkpoints "
      "WHERE symbol = ? ORDER BY id DESC LIMIT 1";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;
  sqlite3_bind_text(stmt, 1, symbol.c_str(), -1, SQLITE_TRANSIENT);
  std::optional<Checkpoint> result;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    Checkpoint c;
    c.last_seq = static_cast<SeqNum>(sqlite3_column_int64(stmt, 0));
    c.symbol = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    c.state_data = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    c.timestamp = sqlite3_column_int64(stmt, 3);
    result = c;
  }
  sqlite3_finalize(stmt);
  return result;
}

std::vector<Checkpoint> SqliteEventLog::list_checkpoints() {
  std::vector<Checkpoint> out;
  if (!open_) return out;
  const char* sql = "SELECT last_seq, symbol, state_data, timestamp FROM checkpoints ORDER BY id DESC";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return out;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    Checkpoint c;
    c.last_seq = static_cast<SeqNum>(sqlite3_column_int64(stmt, 0));
    c.symbol = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    c.state_data = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    c.timestamp = sqlite3_column_int64(stmt, 3);
    out.push_back(c);
  }
  sqlite3_finalize(stmt);
  return out;
}

void SqliteEventLog::clear() {
  if (!open_) return;
  sqlite3_exec(db_, "DELETE FROM events", nullptr, nullptr, nullptr);
  sqlite3_exec(db_, "DELETE FROM checkpoints", nullptr, nullptr, nullptr);
}

} // namespace marketstate
