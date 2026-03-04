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

#include "acl_command_manager.h"

#include <algorithm>
#include <shared_mutex>
#include <string_view>
#include <utility>

#include "commands/commander.h"
#include "common/logging.h"
#include "common/string_util.h"

namespace redis {

namespace {

std::optional<CommandCategory> ParseCategoryName(std::string_view category) {
  if (category == "unknown") return CommandCategory::Unknown;
  if (category == "bit") return CommandCategory::Bit;
  if (category == "bloomfilter") return CommandCategory::BloomFilter;
  if (category == "cluster") return CommandCategory::Cluster;
  if (category == "function") return CommandCategory::Function;
  if (category == "geo") return CommandCategory::Geo;
  if (category == "hash") return CommandCategory::Hash;
  if (category == "hll") return CommandCategory::HLL;
  if (category == "json") return CommandCategory::JSON;
  if (category == "key") return CommandCategory::Key;
  if (category == "list") return CommandCategory::List;
  if (category == "pubsub") return CommandCategory::Pubsub;
  if (category == "replication") return CommandCategory::Replication;
  if (category == "script") return CommandCategory::Script;
  if (category == "search") return CommandCategory::Search;
  if (category == "server") return CommandCategory::Server;
  if (category == "set") return CommandCategory::Set;
  if (category == "sortedint") return CommandCategory::SortedInt;
  if (category == "stream") return CommandCategory::Stream;
  if (category == "string") return CommandCategory::String;
  if (category == "tdigest") return CommandCategory::TDigest;
  if (category == "txn") return CommandCategory::Txn;
  if (category == "zset") return CommandCategory::ZSet;
  if (category == "timeseries") return CommandCategory::Timeseries;
  return std::nullopt;
}

}  // namespace

AclCommandManager &AclCommandManager::Instance() {
  static AclCommandManager instance;
  return instance;
}

size_t AclCommandManager::RegisterCommand(const std::string &name, [[maybe_unused]] CommandCategory category) {
  const std::string key = util::ToLower(name);

  // Registration happens during server startup on a single thread before ACL is sealed,
  // so we intentionally skip locking here to avoid unnecessary contention.
  auto iter = command_bits_.find(key);
  if (iter != command_bits_.end()) {
    return iter->second;
  }

  if (sealed_.load(std::memory_order_acquire)) {
    FATAL("Attempt to register ACL command `{}` after manager sealed.", name);
  }

  auto bit = next_bit_++;
  command_bits_.emplace(key, bit);
  command_categories_.emplace(key, category);
  return bit;
}

std::optional<size_t> AclCommandManager::GetCommandBit(const std::string &name) const {
  const std::string key = util::ToLower(name);
  std::shared_lock<std::shared_mutex> lock(mu_);
  auto iter = command_bits_.find(key);
  if (iter == command_bits_.end()) {
    return std::nullopt;
  }
  return iter->second;
}

StatusOr<std::vector<uint64_t>> AclCommandManager::BuildBitmapForCommands(
    const std::vector<std::string> &commands) const {
  std::shared_lock<std::shared_mutex> lock(mu_);
  std::vector<uint64_t> bitmap;
  for (const auto &command : commands) {
    const std::string key = util::ToLower(command);
    auto iter = command_bits_.find(key);
    if (iter == command_bits_.end()) {
      return Status{Status::NotOK, "unknown ACL command: " + command};
    }
    const size_t bit = iter->second;
    const size_t index = bit / 64;
    if (bitmap.size() <= index) {
      bitmap.resize(index + 1, 0);
    }
    bitmap[index] |= (UINT64_C(1) << (bit % 64));
  }
  return bitmap;
}

StatusOr<std::vector<uint64_t>> AclCommandManager::BuildBitmapForCategory(const std::string &category) const {
  const std::string key = util::ToLower(category);
  auto category_enum = ParseCategoryName(key);
  if (!category_enum.has_value()) {
    return {Status::RedisParseErr, "unknown ACL category: " + category};
  }

  std::shared_lock<std::shared_mutex> lock(mu_);
  std::vector<uint64_t> bitmap;
  for (const auto &[name, bit] : command_bits_) {
    auto category_iter = command_categories_.find(name);
    if (category_iter == command_categories_.end() || category_iter->second != category_enum.value()) {
      continue;
    }
    const size_t index = bit / 64;
    if (bitmap.size() <= index) {
      bitmap.resize(index + 1, 0);
    }
    bitmap[index] |= (UINT64_C(1) << (bit % 64));
  }
  return bitmap;
}

std::vector<std::string> AclCommandManager::CommandsFromBitmap(const std::vector<uint64_t> &bitmap) const {
  std::vector<std::string> names;
  std::shared_lock<std::shared_mutex> lock(mu_);
  for (const auto &[name, bit] : command_bits_) {
    const size_t index = bit / 64;
    if (index >= bitmap.size()) {
      continue;
    }
    if ((bitmap[index] & (UINT64_C(1) << (bit % 64))) != 0) {
      names.push_back(name);
    }
  }
  return names;
}

std::vector<uint64_t> AclCommandManager::BuildBitmapForAllCommands() const {
  std::shared_lock<std::shared_mutex> lock(mu_);
  if (command_bits_.empty()) {
    return {};
  }

  size_t max_bit = 0;
  for (const auto &[_, bit] : command_bits_) {
    max_bit = std::max(max_bit, bit);
  }

  const size_t chunk_count = max_bit / 64 + 1;
  std::vector<uint64_t> bitmap(chunk_count, 0);
  for (const auto &[_, bit] : command_bits_) {
    const size_t index = bit / 64;
    bitmap[index] |= (UINT64_C(1) << (bit % 64));
  }
  return bitmap;
}

bool AclCommandManager::IsCommandAllowed(const std::vector<uint64_t> &bitmap, const std::string &command) const {
  auto bit = GetCommandBit(command);
  if (!bit.has_value()) {
    return true;
  }
  return IsCommandAllowedByBit(bitmap, bit.value());
}

bool AclCommandManager::IsCommandAllowedByBit(const std::vector<uint64_t> &bitmap, size_t bit) const {
  const size_t index = bit / 64;
  if (bitmap.size() <= index) {
    return false;
  }
  return (bitmap[index] & (UINT64_C(1) << (bit % 64))) != 0;
}

void AclCommandManager::Seal() {
  std::unique_lock<std::shared_mutex> lock(mu_);
  sealed_ = true;
}

}  // namespace redis
