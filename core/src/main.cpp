#include "marketstate/event.hpp"
#include "marketstate/event_log.hpp"
#include "marketstate/metrics.hpp"
#include "marketstate/state_engine.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace marketstate;

namespace {

[[noreturn]] void die(const std::string& msg) {
  std::cerr << "error: " << msg << "\n";
  std::exit(1);
}

std::vector<Event> read_events(const std::string& path) {
  std::istream* in = &std::cin;
  std::ifstream file;
  if (path != "-") {
    file.open(path);
    if (!file) die("cannot open events file: " + path);
    in = &file;
  }
  std::vector<Event> events;
  std::string line;
  while (std::getline(*in, line)) {
    if (line.empty() || line[0] == '#') continue;
    auto ev = decode_event(line);
    if (!ev) {
      std::cerr << "warning: skipping unparseable line\n";
      continue;
    }
    events.push_back(*ev);
  }
  return events;
}

std::string state_hash(const std::string& state) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%016llx",
                static_cast<unsigned long long>(compute_checksum(state)));
  return buf;
}

void write_file(const std::string& path, const std::string& content) {
  std::ofstream out(path, std::ios::binary);
  if (!out) die("cannot write file: " + path);
  out << content;
}

struct Args {
  std::string command;
  std::string events;
  std::string db;
  std::string state_out;
  std::string metrics_out;
  std::string recovery_out;
  std::string recovery_source;
  size_t checkpoint_interval = 0;
  size_t repeat = 1;
  bool quiet = false;
};

Args parse_args(int argc, char** argv) {
  if (argc < 2) die("usage: marketstate <run|replay|recover|bench> [flags]");
  Args a;
  a.command = argv[1];
  for (int i = 2; i < argc; ++i) {
    std::string f = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) die("missing value for " + f);
      return argv[++i];
    };
    if (f == "--events") a.events = next();
    else if (f == "--db") a.db = next();
    else if (f == "--state-out") a.state_out = next();
    else if (f == "--metrics-out") a.metrics_out = next();
    else if (f == "--recovery-out") a.recovery_out = next();
    else if (f == "--recovery-source") a.recovery_source = next();
    else if (f == "--checkpoint-interval") a.checkpoint_interval = std::stoull(next());
    else if (f == "--repeat") a.repeat = std::stoull(next());
    else if (f == "--quiet") a.quiet = true;
    else die("unknown flag: " + f);
  }
  return a;
}

// Ingest a full stream, optionally logging and checkpointing.
void run_stream(const Args& a, bool with_log) {
  if (a.events.empty()) die("--events required");
  auto events = read_events(a.events);

  StateEngine engine;
  std::unique_ptr<IEventLog> log;
  std::unique_ptr<SqliteEventLog> sqlite;
  if (with_log && !a.db.empty()) {
    sqlite = std::make_unique<SqliteEventLog>(a.db, /*synchronous=*/true);
    if (!sqlite->open()) die("cannot open db: " + a.db);
    log = std::move(sqlite);
  } else if (with_log) {
    auto mem = std::make_unique<MemoryEventLog>();
    mem->open();
    log = std::move(mem);
  }
  if (log) engine.attach_log(log.get());

  // Optional retransmission source: the authoritative event history used to
  // repair dropped/out-of-order events.
  MemoryRecoverySource recovery;
  if (!a.recovery_source.empty()) {
    recovery.add_range(read_events(a.recovery_source));
    engine.attach_recovery_source(&recovery);
  }

  engine.metrics().start();
  size_t applied_since_checkpoint = 0;
  for (const Event& event : events) {
    IngestResult r = engine.ingest(event);
    applied_since_checkpoint += r.applied;
    if (a.checkpoint_interval > 0 && applied_since_checkpoint >= a.checkpoint_interval && log) {
      engine.checkpoint(*log);
      applied_since_checkpoint = 0;
    }
  }
  // End-of-stream reconciliation repairs any tail losses if a source exists.
  if (!a.recovery_source.empty()) engine.finalize();
  if (log) engine.checkpoint(*log);
  engine.metrics().stop();

  std::string state = engine.canonical_state();
  if (!a.state_out.empty()) write_file(a.state_out, state);
  if (!a.metrics_out.empty()) write_file(a.metrics_out, metrics_json(engine.metrics().metrics()));

  std::ostringstream summary;
  summary << "{\"command\":\"" << a.command << "\",\"events\":" << events.size()
          << ",\"state_hash\":\"" << state_hash(state) << "\",\"state_bytes\":" << state.size()
          << ",\"metrics\":" << metrics_json(engine.metrics().metrics()) << "}";
  if (!a.quiet) {
    std::cout << summary.str() << "\n";
  }
  if (!a.state_out.empty()) {
    std::cout << "state written to " << a.state_out << " (hash " << state_hash(state) << ")\n";
  }
}

// Rebuild a pristine engine from a durable log alone (full replay), used as the
// reference to compare against checkpoint + replay recovery.
std::string full_replay_state(IEventLog& log) {
  StateEngine engine;
  auto events = log.replay_all();
  for (const Event& e : events) engine.get_book(e.symbol).apply_event(e);
  return engine.canonical_state();
}

void cmd_recover(const Args& a) {
  if (a.db.empty()) die("--db required for recover");
  SqliteEventLog log(a.db, true);
  if (!log.open()) die("cannot open db: " + a.db);

  StateEngine engine;
  auto report = engine.recover(log);

  std::string recovered = engine.canonical_state();
  std::string full = full_replay_state(log);
  bool equivalent = (recovered == full);

  std::ostringstream json;
  json << "{\"used_checkpoint\":" << (report.used_checkpoint ? "true" : "false")
       << ",\"checkpoint_seq\":" << report.checkpoint_seq
       << ",\"events_replayed\":" << report.events_replayed
       << ",\"resumed_at\":" << report.resumed_at
       << ",\"duration_us\":" << report.duration_us
       << ",\"recovered_state_hash\":\"" << state_hash(recovered) << "\""
       << ",\"full_replay_state_hash\":\"" << state_hash(full) << "\""
       << ",\"equivalent\":" << (equivalent ? "true" : "false") << "}";

  if (!a.state_out.empty()) write_file(a.state_out, recovered);
  if (!a.recovery_out.empty()) write_file(a.recovery_out, json.str());
  std::cout << json.str() << "\n";
  if (!equivalent) std::exit(2);
}

void cmd_bench(const Args& a) {
  if (a.events.empty()) die("--events required");
  auto events = read_events(a.events);

  for (size_t r = 0; r < a.repeat; ++r) {
    StateEngine engine;
    MemoryEventLog log;
    log.open();
    engine.attach_log(&log);
    engine.metrics().start();
    for (const Event& event : events) engine.ingest(event);
    engine.metrics().stop();
    if (!a.quiet) {
      std::cout << metrics_json(engine.metrics().metrics()) << "\n";
    } else if (r == 0) {
      std::cout << metrics_json(engine.metrics().metrics()) << "\n";
    }
  }
}

} // namespace

int main(int argc, char** argv) {
  Args a = parse_args(argc, argv);
  if (a.command == "run") {
    run_stream(a, /*with_log=*/true);
  } else if (a.command == "replay") {
    run_stream(a, /*with_log=*/false);
  } else if (a.command == "recover") {
    cmd_recover(a);
  } else if (a.command == "bench") {
    cmd_bench(a);
  } else {
    die("unknown command: " + a.command);
  }
  return 0;
}
