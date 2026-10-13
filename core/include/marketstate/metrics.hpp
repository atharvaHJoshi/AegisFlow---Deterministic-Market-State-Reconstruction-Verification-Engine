#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace marketstate {

// Log-linear histogram: constant relative error (~1/8) across the full range,
// so p99.9 is meaningful without storing every sample.
class LatencyHistogram {
public:
  LatencyHistogram() { counts_.assign(kBuckets, 0); }

  void record(int64_t value_ns);
  void reset();

  uint64_t count() const { return count_; }
  int64_t min_ns() const;
  int64_t max_ns() const;
  double mean_ns() const;
  int64_t percentile_ns(double percentile) const;

private:
  static constexpr int kSubBits = 3;
  static constexpr int kSubCount = 1 << kSubBits; // 8
  static constexpr int kBuckets = 64 * kSubCount;

  static int index_of(int64_t value);
  static int64_t lower_bound_of(int index);
  int64_t value_at_quantile(double q) const;

  std::vector<uint64_t> counts_;
  uint64_t count_ = 0;
  int64_t sum_ = 0;
  int64_t min_ = 0;
  int64_t max_ = 0;
};

struct Metrics {
  uint64_t events_received = 0;
  uint64_t events_applied = 0;
  uint64_t events_rejected = 0;
  uint64_t duplicates_detected = 0;
  uint64_t stale_detected = 0;
  uint64_t buffered = 0;
  uint64_t buffer_full = 0;
  uint64_t gaps_detected = 0;
  uint64_t recoveries_attempted = 0;
  uint64_t recoveries_succeeded = 0;
  uint64_t events_recovered = 0;
  uint64_t events_unrecovered = 0;
  double throughput_eps = 0.0;

  LatencyHistogram latency;

  void reset() { *this = Metrics{}; }
};

class MetricsCollector {
public:
  void start();
  void stop();
  void record_received();
  void record_applied(int64_t latency_ns);
  void record_applied_count(uint64_t n); // applied without a latency sample
  void record_rejected();
  void record_duplicate();
  void record_stale();
  void record_buffered();
  void record_buffer_full();
  void record_gap();
  void record_recovery_attempt(bool success, uint64_t recovered, uint64_t unrecovered);

  const Metrics& metrics() const { return metrics_; }
  Metrics& metrics() { return metrics_; }
  bool running() const { return running_; }
  int64_t elapsed_ns() const;

private:
  Metrics metrics_;
  int64_t start_ns_ = 0;
  int64_t stop_ns_ = 0;
  bool running_ = false;
};

std::string metrics_json(const Metrics& metrics);

} // namespace marketstate
