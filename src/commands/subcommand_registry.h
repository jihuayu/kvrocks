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

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace redis {

enum class CommandCategory : uint8_t;
struct CommandAttributes;

using SubcommandResolver = std::function<std::optional<std::string>(const std::vector<std::string> &)>;

std::optional<std::string> ResolveSubcommandByArgIndex(const std::vector<std::string> &args, size_t index);
SubcommandResolver MakeArgIndexSubcommandResolver(size_t index);

struct SubcommandFamily {
  SubcommandResolver resolver;
  std::map<std::string, const CommandAttributes *> subcommands;
};

class SubcommandRegistry {
 public:
  SubcommandRegistry() = delete;

  static void RegisterFamily(const std::string &parent, SubcommandResolver resolver);
  static void RegisterSubcommand(const std::string &parent, const std::string &sub,
                                 const CommandAttributes *attributes);
  static const SubcommandFamily *GetFamily(const std::string &parent);
  static const CommandAttributes *LookupSubcommand(const std::string &parent, const std::string &sub);
  static std::vector<const CommandAttributes *> GetAll();
};

}  // namespace redis
