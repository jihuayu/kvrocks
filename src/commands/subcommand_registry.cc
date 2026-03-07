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

#include "commands/subcommand_registry.h"

#include <cstdlib>

#include "commands/commander.h"
#include "string_util.h"

namespace redis {

namespace {

std::map<std::string, SubcommandFamily> &GetSubcommandFamilies() {
  static auto *subcommand_families = new std::map<std::string, SubcommandFamily>();
  return *subcommand_families;
}

std::deque<CommandAttributes> &GetSubcommandTable() {
  static auto *subcommand_table = new std::deque<CommandAttributes>();
  return *subcommand_table;
}

std::deque<CommandAttributes> &GetSubcommandFallbackTable() {
  static auto *subcommand_fallback_table = new std::deque<CommandAttributes>();
  return *subcommand_fallback_table;
}

}  // namespace

std::optional<std::string> ResolveSubcommandByArgIndex(const std::vector<std::string> &args, size_t index) {
  if (args.size() <= index) {
    return std::nullopt;
  }

  return util::ToLower(args[index]);
}

SubcommandResolver MakeArgIndexSubcommandResolver(size_t index) {
  return [index](const std::vector<std::string> &args) { return ResolveSubcommandByArgIndex(args, index); };
}

void SubcommandRegistry::RegisterFamily(const std::string &parent, SubcommandResolver resolver,
                                        const CommandAttributes *fallback_attributes) {
  auto &family = GetSubcommandFamilies()[util::ToLower(parent)];
  family.resolver = std::move(resolver);
  family.fallback_attributes = fallback_attributes;
}

void SubcommandRegistry::RegisterSubcommand(const std::string &parent, const std::string &sub,
                                            const CommandAttributes *attributes) {
  GetSubcommandFamilies()[util::ToLower(parent)].subcommands[util::ToLower(sub)] = attributes;
}

const SubcommandFamily *SubcommandRegistry::GetFamily(const std::string &parent) {
  auto &subcommand_families = GetSubcommandFamilies();
  auto it = subcommand_families.find(util::ToLower(parent));
  if (it == subcommand_families.end()) {
    return nullptr;
  }

  return &it->second;
}

const CommandAttributes *SubcommandRegistry::GetFallback(const std::string &parent) {
  auto family = GetFamily(parent);
  if (family == nullptr) {
    return nullptr;
  }

  return family->fallback_attributes;
}

const CommandAttributes *SubcommandRegistry::LookupSubcommand(const std::string &parent, const std::string &sub) {
  auto family = GetFamily(parent);
  if (family == nullptr) {
    return nullptr;
  }

  auto it = family->subcommands.find(util::ToLower(sub));
  if (it == family->subcommands.end()) {
    return nullptr;
  }

  return it->second;
}

RegisterToSubcommandTable::RegisterToSubcommandTable(CommandCategory category, CommandAttributes parent_attributes,
                                                     SubcommandResolver resolver,
                                                     std::optional<CommandAttributes> fallback_attributes,
                                                     std::initializer_list<CommandAttributes> list) {
  if (category == CommandCategory::Disabled) {
    return;
  }

  auto normalized_parent = util::ToLower(parent_attributes.name);
  if (parent_attributes.name.find('|') != std::string::npos) {
    std::cerr << fmt::format("Encountered invalid subcommand parent '{}'", parent_attributes.name) << std::endl;
    std::abort();
  }

  if (CommandTable::GetOriginal()->find(normalized_parent) != CommandTable::GetOriginal()->end()) {
    std::cerr << fmt::format("Encountered duplicated command registration '{}'", normalized_parent) << std::endl;
    std::abort();
  }

  parent_attributes.name = normalized_parent;
  parent_attributes.category = category;
  CommandTable::redis_command_table.emplace_back(std::move(parent_attributes));
  const auto *registered_parent_attr = &CommandTable::redis_command_table.back();
  CommandTable::original_commands[registered_parent_attr->name] = registered_parent_attr;
  CommandTable::commands[registered_parent_attr->name] = registered_parent_attr;

  const CommandAttributes *registered_fallback_attr = registered_parent_attr;
  if (fallback_attributes) {
    auto &subcommand_fallback_table = GetSubcommandFallbackTable();
    fallback_attributes->category = category;
    subcommand_fallback_table.emplace_back(std::move(*fallback_attributes));
    registered_fallback_attr = &subcommand_fallback_table.back();
  }

  SubcommandRegistry::RegisterFamily(normalized_parent, std::move(resolver), registered_fallback_attr);
  auto &subcommand_table = GetSubcommandTable();

  for (auto attr : list) {
    attr.category = category;
    subcommand_table.emplace_back(attr);

    auto *registered_attr = &subcommand_table.back();
    auto delimiter = registered_attr->name.find('|');
    if (delimiter == std::string::npos) {
      std::cerr << fmt::format("Encountered invalid subcommand name '{}'", registered_attr->name) << std::endl;
      std::abort();
    }

    auto registered_parent = registered_attr->name.substr(0, delimiter);
    if (registered_parent != normalized_parent) {
      std::cerr << fmt::format("Encountered mismatched subcommand parent '{}', expected '{}'", registered_parent,
                               normalized_parent)
                << std::endl;
      std::abort();
    }

    auto registered_subcommand = registered_attr->name.substr(delimiter + 1);
    if (SubcommandRegistry::LookupSubcommand(registered_parent, registered_subcommand) != nullptr) {
      std::cerr << fmt::format("Encountered duplicated subcommand registration '{}|{}'", registered_parent,
                               registered_subcommand)
                << std::endl;
      std::abort();
    }

    SubcommandRegistry::RegisterSubcommand(registered_parent, registered_subcommand, registered_attr);
  }
}

}  // namespace redis
