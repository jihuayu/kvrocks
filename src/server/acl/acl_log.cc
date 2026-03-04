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

#include "acl_log.h"

#include <algorithm>
#include <chrono>

namespace redis {

namespace {
int64_t NowMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}
}  // namespace

void AclLog::AddEntry(AclDenyReason reason, const std::string &context, const std::string &object,
                      const std::string &username, const std::string &client_info) {
  int64_t now_ms = NowMillis();
  std::lock_guard<std::mutex> lock(mu_);

  // Aggregate with the first (newest) matching entry
  for (auto &entry : log_) {
    if (entry.reason == reason && entry.context == context && entry.object == object && entry.username == username) {
      entry.count++;
      entry.timestamp_last_updated_ms = now_ms;
      return;
    }
  }

  // New entry
  AclLogEntry entry;
  entry.reason = reason;
  entry.context = context;
  entry.object = object;
  entry.username = username;
  entry.client_info = client_info;
  entry.entry_id = next_entry_id_.fetch_add(1, std::memory_order_relaxed);
  entry.timestamp_created_ms = now_ms;
  entry.timestamp_last_updated_ms = now_ms;
  entry.count = 1;

  if (log_.size() >= max_len_) {
    log_.pop_back();
  }
  log_.push_front(std::move(entry));
}

std::vector<AclLogEntry> AclLog::GetEntries(int count) const {
  std::lock_guard<std::mutex> lock(mu_);

  size_t n = (count <= 0) ? log_.size() : std::min(static_cast<size_t>(count), log_.size());
  return std::vector<AclLogEntry>(log_.begin(), log_.begin() + static_cast<std::ptrdiff_t>(n));
}

void AclLog::Reset() {
  std::lock_guard<std::mutex> lock(mu_);
  log_.clear();
}

size_t AclLog::Size() const {
  std::lock_guard<std::mutex> lock(mu_);
  return log_.size();
}

}  // namespace redis
