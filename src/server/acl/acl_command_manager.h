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
#include <map>
#include <optional>
#include <shared_mutex>
#include <string>
#include <vector>

#include "common/status.h"

namespace redis {

enum class CommandCategory : uint8_t;

class AclCommandManager {
 public:
  static AclCommandManager &Instance();

  size_t RegisterCommand(const std::string &name, CommandCategory category);
  std::optional<size_t> GetCommandBit(const std::string &name) const;
  StatusOr<std::vector<uint64_t>> BuildBitmapForCommands(const std::vector<std::string> &commands) const;
  std::vector<std::string> CommandsFromBitmap(const std::vector<uint64_t> &bitmap) const;
  std::vector<uint64_t> BuildBitmapForAllCommands() const;
  bool IsCommandAllowed(const std::vector<uint64_t> &bitmap, const std::string &command) const;
  void Seal();

  AclCommandManager(const AclCommandManager &) = delete;
  AclCommandManager &operator=(const AclCommandManager &) = delete;
  ~AclCommandManager() = default;

 private:
  AclCommandManager() = default;

  mutable std::shared_mutex mu_;
  std::map<std::string, size_t> command_bits_;
  size_t next_bit_ = 0;
  std::atomic<bool> sealed_{false};
};

}  // namespace redis
