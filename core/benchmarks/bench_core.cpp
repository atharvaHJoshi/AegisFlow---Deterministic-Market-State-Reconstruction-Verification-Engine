// Optional microbenchmarks. Enabled with -DBUILD_BENCHMARKS=ON and a system
// installation of Google Benchmark. The built-in `marketstate bench` command
// provides end-to-end throughput/latency numbers without this dependency.
#include "marketstate/event.hpp"
#include "marketstate/order_book.hpp"
#include "marketstate/state_engine.hpp"

#include <benchmark/benchmark.h>

#include <vector>

using namespace marketstate;

namespace {

std::vector<Event> sample_events(size_t n) {
  std::vector<Event> events;
  events.reserve(n);
  for (uint64_t seq = 1; seq <= n; ++seq) {
    if (seq % 5 == 0) {
      events.push_back(make_trade(seq, 1, seq, "AAPL", seq * 1000, seq - 1, seq, 10000, 5));
    } else if (seq % 3 == 0) {
      events.push_back(make_modify(seq, 1, seq, "AAPL", seq * 1000, seq - 1, kNoPrice, 25));
    } else {
      events.push_back(
          make_add(seq, 1, seq, "AAPL", seq * 1000, seq, Side::BID, 10000 - (seq % 20), 50));
    }
  }
  return events;
}

void BM_EncodeDecode(benchmark::State& state) {
  auto events = sample_events(static_cast<size_t>(state.range(0)));
  for (auto _ : state) {
    for (const auto& e : events) {
      auto line = encode_event(e);
      auto decoded = decode_event(line);
      benchmark::DoNotOptimize(decoded);
    }
  }
  state.SetItemsProcessed(state.iterations() * events.size());
}

void BM_OrderBookApply(benchmark::State& state) {
  auto events = sample_events(static_cast<size_t>(state.range(0)));
  for (auto _ : state) {
    OrderBook book("AAPL");
    for (const auto& e : events) book.apply_event(e);
    benchmark::DoNotOptimize(book.canonical_state());
  }
  state.SetItemsProcessed(state.iterations() * events.size());
}

void BM_EngineIngestMemoryLog(benchmark::State& state) {
  auto events = sample_events(static_cast<size_t>(state.range(0)));
  for (auto _ : state) {
    StateEngine engine;
    MemoryEventLog log;
    log.open();
    engine.attach_log(&log);
    for (const auto& e : events) engine.ingest(e);
    benchmark::DoNotOptimize(engine.canonical_state());
  }
  state.SetItemsProcessed(state.iterations() * events.size());
}

} // namespace

BENCHMARK(BM_EncodeDecode)->Arg(100000);
BENCHMARK(BM_OrderBookApply)->Arg(100000);
BENCHMARK(BM_EngineIngestMemoryLog)->Arg(100000);
