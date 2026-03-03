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

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <shared_mutex>
#include <string>
#include <vector>

#include "common/status.h"
#include "jsoncons/json.hpp"

namespace redis {

// NOTE: The ACL (Access Control List) design and implementation in this class follows the guidelines and semantics
// described in the official Redis documentation:
//   https://redis.io/docs/latest/operate/oss_and_stack/management/security/acl/
// Please refer to the above link for detailed behavior, rules, and compatibility expectations.

// Key permission flags for ACL key patterns (similar to Redis ACL_READ_PERMISSION, ACL_WRITE_PERMISSION)
constexpr uint32_t kAclKeyRead = 1 << 0;
constexpr uint32_t kAclKeyWrite = 1 << 1;
constexpr uint32_t kAclKeyAll = kAclKeyRead | kAclKeyWrite;

// Selector flags. Root selector always exists; all* flags represent wildcard grants.
constexpr uint32_t kAclSelectorRoot = 1 << 0;
constexpr uint32_t kAclSelectorAllKeys = 1 << 1;
constexpr uint32_t kAclSelectorAllChannels = 1 << 2;
constexpr uint32_t kAclSelectorAllCommands = 1 << 3;

// Key pattern with permissions (%R~pattern, %W~pattern, ~pattern)
struct AclKeyPattern {
  std::string pattern;
  uint32_t flags;  // kAclKeyRead, kAclKeyWrite, or kAclKeyAll

  AclKeyPattern() : flags(kAclKeyAll) {}
  AclKeyPattern(std::string p, uint32_t f) : pattern(std::move(p)), flags(f) {}

  bool operator==(const AclKeyPattern &other) const { return pattern == other.pattern && flags == other.flags; }
};

class AclSelector {
 public:
  uint32_t flags = 0;                       // SELECTOR_FLAG_ALLKEYS, ALLCHANNELS, ALLCOMMANDS, etc.
  std::vector<uint64_t> allowed_commands;   // Command permission bitmap, size = USER_COMMAND_BITS_COUNT / 64
  std::vector<uint32_t> allowed_category;   // Command category permission bitmap, size = USER_CATEGORY_BITS_COUNT / 32
  std::vector<AclKeyPattern> key_patterns;  // List of key patterns with permissions
  std::vector<std::string> channels;        // List of channel patterns
};

class AclUser {
 public:
  bool enabled = false;                       // Whether the user is enabled
  bool nopass = false;                        // Whether any password is accepted (Redis "nopass")
  bool sanitize_payload = true;               // Whether command payload should be sanitized in ACL logs
  std::string ns;                             // Namespace of the user
  std::vector<AclSelector> allowed_commands;  // The first is the root selector, the rest are regular selectors
  std::set<std::string> passwords;            // Set of passwords, stored as sha256 hashes

  jsoncons::json ToJson() const;
  static StatusOr<AclUser> FromJson(const jsoncons::json &json);
};

// Helper functions for user state management
void ResetUserState(AclUser &user);
AclSelector &EnsureRootSelector(AclUser &user);

class AclUserManager {
 public:
  AclUserManager();

  std::shared_ptr<const AclUser> GetUserByIndex(size_t index);
  std::shared_ptr<const AclUser> GetUserByUserName(const std::string &username);
  std::optional<size_t> GetUserIndex(const std::string &username) const;
  bool SetUser(const std::string &username, std::shared_ptr<const AclUser> user);
  bool AddUser(const std::string &username, std::shared_ptr<const AclUser> user);
  bool DeleteUser(const std::string &username);
  void Reset();
  std::vector<std::string> ListUsernames() const;
  std::optional<std::string> GetUsernameByIndex(size_t index) const;

 private:
  std::optional<size_t> findFreeSlotLocked() const;
  mutable std::shared_mutex mu_;
  // username to user_array_ index mapping
  std::map<std::string, size_t> username_index_;
  // lock free fixed-size array to store users
  std::array<std::shared_ptr<const AclUser>, 256> user_array_;
};

}  // namespace redis
