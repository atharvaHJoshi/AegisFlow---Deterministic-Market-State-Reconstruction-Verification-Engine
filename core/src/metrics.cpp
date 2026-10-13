#include "marketstate/metrics.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <sstream>

namespace marketstate {

namespace {
int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
} // namespace

int LatencyHistogram::index_of(int64_t value) {
  if (value <= 0) return 0;
  int msb = 63 - __builtin_clzll(static_cast<unsigned long long>(value));
  unsigned long long base = 1ULL << msb;
  unsigned long long sub = ((static_cast<unsigned long long>(value) - base) * kSubCount) >> msb;
  if (sub >= kSubCount) sub = kSubCount - 1;
  int idx = msb * kSubCount + static_cast<int>(sub);
  if (idx >= kBuckets) idx = kBuckets - 1;
  return idx;
}

int64_t LatencyHistogram::lower_bound_of(int index) {
  if (index <= 0) return 0;
  int msb = index / kSubCount;
  int sub = index % kSubCount;
  unsigned long long base = 1ULL << msb;
  return static_cast<int64_t>(base + (base * sub) / kSubCount);
}

void LatencyHistogram::record(int64_t value_ns) {
  counts_[index_of(value_ns)]++;
  count_++;
  sum_ += value_ns;
  if (count_ == 1 || value_ns < min_) min_ = value_ns;
  if (count_ == 1 || value_ns > max_) max_ = value_ns;
}

void LatencyHistogram::reset() {
  std::fill(counts_.begin(), counts_.end(), 0);
  count_ = 0;
  sum_ = 0;
  min_ = 0;
  max_ = 0;
}

int64_t LatencyHistogram::min_ns() const { return min_; }
int64_t LatencyHistogram::max_ns() const { return max_; }
double LatencyHistogram::mean_ns() const {
  return count_ == 0 ? 0.0 : static_cast<double>(sum_) / static_cast<double>(count_);
}

int64_t LatencyHistogram::percentile_ns(double percentile) const {
  if (count_ == 0) return 0;
  if (percentile <= 0.0) return min_;
  if (percentile >= 100.0) return max_;
  return value_at_quantile(percentile / 100.0);
}

int64_t LatencyHistogram::value_at_quantile(double q) const {
  uint64_t target = static_cast<uint64_t>(std::ceil(q * static_cast<double>(count_)));
  if (target == 0) target = 1;
  uint64_t cumulative = 0;
  for (int i = 0; i < kBuckets; ++i) {
    cumulative += counts_[i];
    if (cumulative >= target) return lower_bound_of(i);
  }
  return max_;
}

void MetricsCollector::start() {
  metrics_.reset();
  running_ = true;
  start_ns_ = now_ns();
  stop_ns_ = 0;
}

void MetricsCollector::stop() {
  running_ = false;
  stop_ns_ = now_ns();
  int64_t elapsed = stop_ns_ - start_ns_;
  if (elapsed > 0) {
    metrics_.throughput_eps =
        static_cast<double>(metrics_.events_applied) * 1e9 / static_cast<double>(elapsed);
  }
}

int64_t MetricsCollector::elapsed_ns() const {
  int64_t end = running_ ? now_ns() : stop_ns_;
  return end - start_ns_;
}

void MetricsCollector::record_received() { metrics_.events_received++; }
void MetricsCollector::record_applied(int64_t latency_ns) {
  metrics_.events_applied++;
  metrics_.latency.record(latency_ns);
}
void MetricsCollector::record_applied_count(uint64_t n) { metrics_.events_applied += n; }
void MetricsCollector::record_rejected() { metrics_.events_rejected++; }
void MetricsCollector::record_duplicate() { metrics_.duplicates_detected++; }
void MetricsCollector::record_stale() { metrics_.stale_detected++; }
void MetricsCollector::record_buffered() { metrics_.buffered++; }
void MetricsCollector::record_buffer_full() { metrics_.buffer_full++; }
void MetricsCollector::record_gap() { metrics_.gaps_detected++; }

void MetricsCollector::record_recovery_attempt(bool success, uint64_t recovered,
                                               uint64_t unrecovered) {
  metrics_.recoveries_attempted++;
  if (success) metrics_.recoveries_succeeded++;
  metrics_.events_recovered += recovered;
  metrics_.events_unrecovered += unrecovered;
}

std::string metrics_json(const Metrics& m) {
  std::ostringstream oss;
  oss << "{";
  oss << "\"events_received\":" << m.events_received << ",";
  oss << "\"events_applied\":" << m.events_applied << ",";
  oss << "\"events_rejected\":" << m.events_rejected << ",";
  oss << "\"duplicates_detected\":" << m.duplicates_detected << ",";
  oss << "\"stale_detected\":" << m.stale_detected << ",";
  oss << "\"buffered\":" << m.buffered << ",";
  oss << "\"buffer_full\":" << m.buffer_full << ",";
  oss << "\"gaps_detected\":" << m.gaps_detected << ",";
  oss << "\"recoveries_attempted\":" << m.recoveries_attempted << ",";
  oss << "\"recoveries_succeeded\":" << m.recoveries_succeeded << ",";
  oss << "\"events_recovered\":" << m.events_recovered << ",";
  oss << "\"events_unrecovered\":" << m.events_unrecovered << ",";
  oss << "\"throughput_eps\":" << m.throughput_eps << ",";
  oss << "\"latency_ns\":{";
  oss << "\"count\":" << m.latency.count() << ",";
  oss << "\"min\":" << m.latency.min_ns() << ",";
  oss << "\"mean\":" << m.latency.mean_ns() << ",";
  oss << "\"p50\":" << m.latency.percentile_ns(50) << ",";
  oss << "\"p95\":" << m.latency.percentile_ns(95) << ",";
  oss << "\"p99\":" << m.latency.percentile_ns(99) << ",";
  oss << "\"p999\":" << m.latency.percentile_ns(99.9) << ",";
  oss << "\"max\":" << m.latency.max_ns();
  oss << "}}";
  return oss.str();
}

} // namespace marketstate
