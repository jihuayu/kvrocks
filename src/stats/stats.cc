/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 *
 */

#include "stats.h"

#include <chrono>
#include <mutex>

#include "fmt/format.h"
#include "time_util.h"

Stats::Stats(std::vector<double> histogram_bucket_boundaries)
    : bucket_boundaries(std::move(histogram_bucket_boundaries)) {
  for (int i = 0; i < STATS_METRIC_COUNT; i++) {
    InstMetric im;
    im.last_sample_time_ms = 0;
    im.last_sample_count = 0;
    im.idx = 0;
    for (uint64_t &sample : im.samples) {
      sample = 0;
    }
    inst_metrics.push_back(im);
  }
}

void Stats::InitCommandStats(const std::vector<std::string> &commands) {
  for (const auto &command : commands) {
    commands_stats[command].calls.store(0, std::memory_order_relaxed);
    commands_stats[command].latency.store(0, std::memory_order_relaxed);

    if (bucket_boundaries.size() > 0) {
      auto &hist = commands_histogram[command];
      hist.calls.store(0, std::memory_order_relaxed);
      hist.sum.store(0, std::memory_order_relaxed);
      hist.buckets.clear();
      for (std::size_t i{0}; i <= bucket_boundaries.size(); ++i) {
        hist.buckets.push_back(std::make_unique<std::atomic<uint64_t>>(0));
      }
    }
  }
}

void Stats::IncrCalls(const std::string &command_name) {
  total_calls.fetch_add(1, std::memory_order_relaxed);
  commands_stats.at(command_name).calls.fetch_add(1, std::memory_order_relaxed);

  if (bucket_boundaries.size() > 0) {
    commands_histogram.at(command_name).calls.fetch_add(1, std::memory_order_relaxed);
  }
}

void Stats::IncrLatency(uint64_t latency, const std::string &command_name) {
  commands_stats.at(command_name).latency.fetch_add(latency, std::memory_order_relaxed);

  if (bucket_boundaries.size() > 0) {
    commands_histogram.at(command_name).sum.fetch_add(latency, std::memory_order_relaxed);

    const auto bucket_index = static_cast<std::size_t>(std::distance(
        bucket_boundaries.begin(), std::lower_bound(bucket_boundaries.begin(), bucket_boundaries.end(), latency)));
    commands_histogram.at(command_name).buckets[bucket_index]->fetch_add(1, std::memory_order_relaxed);
  }
}

#if defined(__APPLE__)
#include <mach/mach_init.h>
#include <mach/task.h>

int64_t Stats::GetMemoryRSS() {
  task_t task = MACH_PORT_NULL;
  task_basic_info t_info;
  mach_msg_type_number_t t_info_count = TASK_BASIC_INFO_COUNT;
  if (task_for_pid(current_task(), getpid(), &task) != KERN_SUCCESS) return 0;
  task_info(task, TASK_BASIC_INFO, (task_info_t)&t_info, &t_info_count);
  return static_cast<int64_t>(t_info.resident_size);
}
#else
#include <fcntl.h>

#include <cstring>
#include <string>

#include "unique_fd.h"

int64_t Stats::GetMemoryRSS() {
  char buf[4096];
  auto fd = UniqueFD(open(fmt::format("/proc/{}/stat", getpid()).c_str(), O_RDONLY));
  if (!fd) return 0;
  if (read(*fd, buf, sizeof(buf)) <= 0) {
    return 0;
  }
  fd.Close();

  char *start = buf;
  int count = 23;  // RSS is the 24th field in /proc/<pid>/stat
  while (start && count--) {
    start = strchr(start, ' ');
    if (start) start++;
  }
  if (!start) return 0;
  char *stop = strchr(start, ' ');
  if (!stop) return 0;
  *stop = '\0';
  int rss = std::atoi(start);
  return static_cast<int64_t>(rss * sysconf(_SC_PAGESIZE));
}
#endif

void Stats::TrackInstantaneousMetric(int metric, uint64_t current_reading) {
  uint64_t curr_time_ms = util::GetTimeStampMS();
  std::unique_lock<std::shared_mutex> lock(inst_metrics_mutex);
  uint64_t t = curr_time_ms - inst_metrics[metric].last_sample_time_ms;
  uint64_t ops = current_reading - inst_metrics[metric].last_sample_count;
  uint64_t ops_sec = t > 0 ? (ops * 1000 / t) : 0;
  inst_metrics[metric].samples[inst_metrics[metric].idx] = ops_sec;
  inst_metrics[metric].idx++;
  inst_metrics[metric].idx %= STATS_METRIC_SAMPLES;
  inst_metrics[metric].last_sample_time_ms = curr_time_ms;
  inst_metrics[metric].last_sample_count = current_reading;
}

uint64_t Stats::GetInstantaneousMetric(int metric) const {
  std::shared_lock<std::shared_mutex> lock(inst_metrics_mutex);
  uint64_t sum = 0;
  for (uint64_t sample : inst_metrics[metric].samples) sum += sample;
  return sum / STATS_METRIC_SAMPLES;
}

NamespaceStatsRegistry::NamespaceStatsRegistry(std::vector<std::string> commands, std::vector<double> bucket_boundaries)
    : commands_(std::move(commands)),
      bucket_boundaries_(std::move(bucket_boundaries)),
      snapshot_(std::make_shared<const Table>()) {}

std::shared_ptr<Stats> NamespaceStatsRegistry::Find(const std::string &ns) const {
  auto snapshot = GetSnapshot();
  auto iter = snapshot->find(ns);
  if (iter == snapshot->end()) {
    return nullptr;
  }
  return iter->second;
}

std::shared_ptr<Stats> NamespaceStatsRegistry::GetOrCreate(const std::string &ns) {
  if (auto stats = Find(ns); stats != nullptr) {
    return stats;
  }

  std::lock_guard lock(writer_mu_);

  auto current = GetSnapshot();
  if (auto iter = current->find(ns); iter != current->end()) {
    return iter->second;
  }

  auto stats = std::make_shared<Stats>(bucket_boundaries_);
  stats->InitCommandStats(commands_);

  auto next = std::make_shared<Table>(*current);
  next->emplace(ns, stats);

  std::shared_ptr<const Table> published = next;
  std::atomic_store_explicit(&snapshot_, std::move(published), std::memory_order_release);

  return stats;
}

NamespaceStatsRegistry::Snapshot NamespaceStatsRegistry::GetSnapshot() const {
  return std::atomic_load_explicit(&snapshot_, std::memory_order_acquire);
}

uint64_t NamespaceStatsRegistry::AggregateTotalCalls() const {
  uint64_t total = 0;
  auto snapshot = GetSnapshot();
  for (const auto &[ns, stats] : *snapshot) {
    total += stats->total_calls.load(std::memory_order_relaxed);
  }
  return total;
}

uint64_t NamespaceStatsRegistry::AggregateInstantaneousOps() const {
  uint64_t total = 0;
  auto snapshot = GetSnapshot();
  for (const auto &[ns, stats] : *snapshot) {
    total += stats->GetInstantaneousMetric(STATS_METRIC_COMMAND);
  }
  return total;
}

AggregatedCommandInfo NamespaceStatsRegistry::AggregateCommandInfo(const std::string &command) const {
  AggregatedCommandInfo info;
  if (!bucket_boundaries_.empty()) {
    info.buckets.resize(bucket_boundaries_.size() + 1);
  }

  auto snapshot = GetSnapshot();
  for (const auto &[ns, stats] : *snapshot) {
    auto stat_it = stats->commands_stats.find(command);
    if (stat_it != stats->commands_stats.end()) {
      info.calls += stat_it->second.calls.load(std::memory_order_relaxed);
      info.latency += stat_it->second.latency.load(std::memory_order_relaxed);
    }

    if (bucket_boundaries_.empty()) continue;

    auto hist_it = stats->commands_histogram.find(command);
    if (hist_it != stats->commands_histogram.end()) {
      info.hist_calls += hist_it->second.calls.load(std::memory_order_relaxed);
      info.hist_sum += hist_it->second.sum.load(std::memory_order_relaxed);
      for (std::size_t i = 0; i < hist_it->second.buckets.size(); ++i) {
        info.buckets[i] += hist_it->second.buckets[i]->load(std::memory_order_relaxed);
      }
    }
  }
  return info;
}

std::map<std::string, AggregatedCommandInfo> NamespaceStatsRegistry::AggregateCommandInfo() const {
  std::map<std::string, AggregatedCommandInfo> result;
  auto snapshot = GetSnapshot();
  for (const auto &[ns, stats] : *snapshot) {
    for (const auto &cmd_stat : stats->commands_stats) {
      auto &info = result[cmd_stat.first];
      info.calls += cmd_stat.second.calls.load(std::memory_order_relaxed);
      info.latency += cmd_stat.second.latency.load(std::memory_order_relaxed);
    }

    if (bucket_boundaries_.empty()) continue;

    for (const auto &cmd_hist : stats->commands_histogram) {
      auto &info = result[cmd_hist.first];
      info.hist_calls += cmd_hist.second.calls.load(std::memory_order_relaxed);
      info.hist_sum += cmd_hist.second.sum.load(std::memory_order_relaxed);

      if (info.buckets.empty()) {
        info.buckets.resize(bucket_boundaries_.size() + 1);
      }
      for (std::size_t i = 0; i < cmd_hist.second.buckets.size(); ++i) {
        info.buckets[i] += cmd_hist.second.buckets[i]->load(std::memory_order_relaxed);
      }
    }
  }
  return result;
}

void NamespaceStatsRegistry::TrackInstantaneousMetrics() const {
  auto snapshot = GetSnapshot();
  for (const auto &[ns, stats] : *snapshot) {
    auto calls = stats->total_calls.load(std::memory_order_relaxed);
    stats->TrackInstantaneousMetric(STATS_METRIC_COMMAND, calls);
  }
}
