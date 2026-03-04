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

#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace redis {

enum class AclDenyReason { None, Auth, Command, Key, Channel };

inline const char *AclDenyReasonString(AclDenyReason reason) {
  switch (reason) {
    case AclDenyReason::Auth:
      return "auth";
    case AclDenyReason::Command:
      return "command";
    case AclDenyReason::Key:
      return "key";
    case AclDenyReason::Channel:
      return "channel";
    default:
      return "unknown";
  }
}

struct AclLogEntry {
  uint64_t count = 1;
  AclDenyReason reason = AclDenyReason::Command;
  std::string context;  // toplevel / multi / lua
  std::string object;   // command / key / channel
  std::string username;
  std::string client_info;
  uint64_t entry_id = 0;
  int64_t timestamp_created_ms = 0;
  int64_t timestamp_last_updated_ms = 0;
};

// Thread-safe, bounded log of ACL access denial events.
// Newer entries appear at the front. Entries with identical (reason, context,
// object, username) are aggregated by incrementing the count and updating
// timestamp_last_updated_ms.
class AclLog {
 public:
  static constexpr size_t kDefaultMaxLen = 128;

  explicit AclLog(size_t max_len = kDefaultMaxLen) : max_len_(max_len) {}

  // Record a denial event. Aggregates with the most recent matching entry when
  // one exists; otherwise prepends a new entry (evicting the oldest when full).
  void AddEntry(AclDenyReason reason, const std::string &context, const std::string &object,
                const std::string &username, const std::string &client_info);

  // Return up to |count| entries, newest first.  count == 0 means "all".
  std::vector<AclLogEntry> GetEntries(int count) const;

  void Reset();

  size_t Size() const;

 private:
  mutable std::mutex mu_;
  std::deque<AclLogEntry> log_;
  std::atomic<uint64_t> next_entry_id_{1};
  size_t max_len_;
};

}  // namespace redis
