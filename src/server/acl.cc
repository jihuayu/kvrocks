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

#include "server/acl.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <exception>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "common/db_util.h"
#include "common/logging.h"
#include "common/parse_util.h"
#include "common/sha256.h"
#include "common/status.h"
#include "common/string_util.h"
#include "config/config.h"
#include "server/namespace.h"
#include "server/redis_connection.h"
#include "server/redis_reply.h"

namespace redis {

namespace {

constexpr const char *kJsonFieldEnabled = "enabled";
constexpr const char *kJsonFieldNamespace = "namespace";
constexpr const char *kJsonFieldPasswords = "passwords";
constexpr const char *kJsonFieldSelectors = "selectors";
constexpr const char *kJsonFieldFlags = "flags";
constexpr const char *kJsonFieldAllowedCommands = "allowed_commands";
constexpr const char *kJsonFieldAllowedCategories = "allowed_categories";
constexpr const char *kJsonFieldKeyPatterns = "key_patterns";
constexpr const char *kJsonFieldChannels = "channels";
std::string MakeAclStorageKey(const std::string &username) { return std::string(kAclStoragePrefix) + username; }

Status PersistAclUser(engine::Storage *storage, const std::string &username, const AclUser &user) {
  engine::Context ctx(storage);
  auto key = MakeAclStorageKey(username);
  return storage->WriteToPropagateCF(ctx, key, user.ToJson().to_string());
}

Status RemoveAclUser(engine::Storage *storage, const std::string &username) {
  engine::Context ctx(storage);
  auto key = MakeAclStorageKey(username);
  auto cf = storage->GetCFHandle(ColumnFamilyID::Propagate);
  auto s = storage->Delete(ctx, storage->DefaultWriteOptions(), cf, key);
  if (s.IsNotFound()) {
    return Status::OK();
  }
  if (!s.ok()) {
    return {Status::NotOK, s.ToString()};
  }
  return Status::OK();
}

StatusOr<AclSelector> SelectorFromJson(const jsoncons::json &json) {
  if (!json.is_object()) {
    return {Status::NotOK, "selector must be an object"};
  }

  AclSelector selector;
  if (json.contains(kJsonFieldFlags)) {
    if (!json[kJsonFieldFlags].is_number()) {
      return {Status::NotOK, "selector flags must be numeric"};
    }
    selector.flags = json[kJsonFieldFlags].as<uint32_t>();
  } else {
    selector.flags = 0;
  }

  if (json.contains(kJsonFieldAllowedCommands)) {
    const auto &cmds = json[kJsonFieldAllowedCommands];
    if (!cmds.is_array()) {
      return {Status::NotOK, "allowed_commands must be an array"};
    }

    std::vector<std::string> command_names;
    for (size_t i = 0; i < cmds.size(); ++i) {
      const auto &val = cmds[i];
      if (!val.is_string()) {
        return {Status::NotOK, "allowed_commands entries must be strings"};
      }
      command_names.push_back(val.as_string());
    }

    auto bitmap_or = AclCommandManager::Instance().BuildBitmapForCommands(command_names);
    if (!bitmap_or.IsOK()) {
      return bitmap_or.ToStatus().Prefixed("failed to parse allowed_commands");
    }
    selector.allowed_commands = std::move(bitmap_or.GetValue());
  }

  if (json.contains(kJsonFieldAllowedCategories)) {
    const auto &cats = json[kJsonFieldAllowedCategories];
    if (!cats.is_array()) {
      return {Status::NotOK, "allowed_categories must be an array"};
    }
    selector.allowed_category.reserve(cats.size());
    for (size_t i = 0; i < cats.size(); ++i) {
      const auto &val = cats[i];
      if (!val.is_number()) {
        return {Status::NotOK, "allowed_categories entries must be numeric"};
      }
      selector.allowed_category.push_back(val.as<uint32_t>());
    }
  }

  if (json.contains(kJsonFieldKeyPatterns)) {
    const auto &key_patterns = json[kJsonFieldKeyPatterns];
    if (!key_patterns.is_array()) {
      return {Status::NotOK, "key_patterns must be an array"};
    }
    selector.key_patterns.reserve(key_patterns.size());
    for (size_t i = 0; i < key_patterns.size(); ++i) {
      const auto &val = key_patterns[i];
      if (!val.is_object()) {
        return {Status::NotOK, "key_patterns entries must be objects"};
      }
      if (!val.contains("pattern") || !val["pattern"].is_string()) {
        return {Status::NotOK, "key_patterns entry must have 'pattern' string field"};
      }
      uint32_t flags = kAclKeyAll;
      if (val.contains("flags") && val["flags"].is_number()) {
        flags = val["flags"].as<uint32_t>();
      }
      selector.key_patterns.emplace_back(val["pattern"].as_string(), flags);
    }
  }

  if (json.contains(kJsonFieldChannels)) {
    const auto &channels = json[kJsonFieldChannels];
    if (!channels.is_array()) {
      return {Status::NotOK, "channels must be an array"};
    }
    selector.channels.reserve(channels.size());
    for (size_t i = 0; i < channels.size(); ++i) {
      const auto &val = channels[i];
      if (!val.is_string()) {
        return {Status::NotOK, "channels entries must be strings"};
      }
      selector.channels.push_back(val.as_string());
    }
  }

  return selector;
}

jsoncons::json SelectorToJson(const AclSelector &selector) {
  jsoncons::json json;
  json[kJsonFieldFlags] = selector.flags;

  auto command_names = AclCommandManager::Instance().CommandsFromBitmap(selector.allowed_commands);
  jsoncons::json cmd_array(jsoncons::json_array_arg);
  for (const auto &name : command_names) {
    cmd_array.push_back(name);
  }
  json[kJsonFieldAllowedCommands] = std::move(cmd_array);

  jsoncons::json category_array(jsoncons::json_array_arg);
  for (auto value : selector.allowed_category) {
    category_array.push_back(value);
  }
  json[kJsonFieldAllowedCategories] = std::move(category_array);

  // Serialize key_patterns with full permission information
  jsoncons::json key_pattern_array(jsoncons::json_array_arg);
  for (const auto &kp : selector.key_patterns) {
    jsoncons::json pattern_obj;
    pattern_obj["pattern"] = kp.pattern;
    pattern_obj["flags"] = kp.flags;
    key_pattern_array.push_back(std::move(pattern_obj));
  }
  json[kJsonFieldKeyPatterns] = std::move(key_pattern_array);

  jsoncons::json channel_array(jsoncons::json_array_arg);
  for (const auto &channel : selector.channels) {
    channel_array.push_back(channel);
  }
  json[kJsonFieldChannels] = std::move(channel_array);

  return json;
}

template <typename... Ts>
struct Overloaded : Ts... {
  using Ts::operator()...;
};
template <typename... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

// Generic template for trimming trailing zeros from a bitmap vector
template <typename T>
void TrimBitmap(std::vector<T> &bitmap) {
  while (!bitmap.empty() && bitmap.back() == 0) {
    bitmap.pop_back();
  }
}

// Generic template for normalizing a bitmap (copy + trim)
template <typename T>
std::vector<T> NormalizeBitmap(const std::vector<T> &bitmap) {
  auto normalized = bitmap;
  TrimBitmap(normalized);
  return normalized;
}

struct EnableAction {};
struct DisableAction {};
struct ResetUserAction {};
struct ResetPassAction {};
struct NoPassAction {};
struct ClearSelectorsAction {};

struct PasswordAction {
  enum class Kind { kAddPlain, kRemovePlain, kAddHashed, kRemoveHashed };
  Kind kind;
  std::string value;
  std::string original;
};

struct CommandToggleAction {
  bool allow;
  bool all;
  std::string command;
  std::string original;
};

struct CategoryToggleAction {
  bool allow;
  bool all;
  std::string category;
  std::string original;
};

struct KeyPatternAction {
  enum class Kind { kReset, kAll, kAdd };
  Kind kind;
  std::string pattern;
  uint32_t flags;  // kAclKeyRead, kAclKeyWrite, or kAclKeyAll
};

struct ChannelPatternAction {
  enum class Kind { kReset, kAll, kAdd };
  Kind kind;
  std::string pattern;
};

// Selector action for (...) syntax - creates a new selector
struct SelectorAction {
  std::vector<std::string> tokens;  // The tokens inside the parentheses
};

using SetUserAction = std::variant<EnableAction, DisableAction, ResetUserAction, ResetPassAction, NoPassAction,
                                   PasswordAction, CommandToggleAction, CategoryToggleAction, KeyPatternAction,
                                   ChannelPatternAction, ClearSelectorsAction, SelectorAction>;

// Forward declarations for helper functions used by action handlers
std::vector<uint32_t> BuildAllCategoryBitmap();
redis::AclSelector &EnsureRootSelector(redis::AclUser &user);
void ResetUserState(redis::AclUser &user);
void TrimCommandBitmap(std::vector<uint64_t> &bitmap);

// Action application functions - extracted for better readability and testability
Status ApplyPasswordAction(redis::AclUser &user, const PasswordAction &action) {
  switch (action.kind) {
    case PasswordAction::Kind::kAddPlain: {
      auto digest = util::Sha256Hex(action.value);
      user.passwords.insert(digest);
      break;
    }
    case PasswordAction::Kind::kRemovePlain: {
      auto digest = util::Sha256Hex(action.value);
      user.passwords.erase(digest);
      break;
    }
    case PasswordAction::Kind::kAddHashed:
      user.passwords.insert(util::ToLower(action.value));
      break;
    case PasswordAction::Kind::kRemoveHashed:
      user.passwords.erase(util::ToLower(action.value));
      break;
  }
  return Status::OK();
}

Status ApplyCommandToggleAction(redis::AclUser &user, const CommandToggleAction &toggle) {
  auto &root = EnsureRootSelector(user);
  auto &command_manager = redis::AclCommandManager::Instance();

  if (toggle.all) {
    root.allowed_commands = toggle.allow ? command_manager.BuildBitmapForAllCommands() : std::vector<uint64_t>{};
    return Status::OK();
  }

  auto bit = command_manager.GetCommandBit(toggle.command);
  if (!bit.has_value()) {
    return {Status::RedisParseErr, "unknown ACL command modifier: " + toggle.original};
  }

  const size_t index = bit.value() / 64;
  const uint64_t mask = UINT64_C(1) << (bit.value() % 64);

  if (toggle.allow) {
    if (root.allowed_commands.size() <= index) {
      root.allowed_commands.resize(index + 1, 0);
    }
    root.allowed_commands[index] |= mask;
  } else if (root.allowed_commands.size() > index) {
    root.allowed_commands[index] &= ~mask;
    TrimCommandBitmap(root.allowed_commands);
  }
  return Status::OK();
}

Status ApplyCategoryToggleAction(redis::AclUser &user, const CategoryToggleAction &toggle) {
  (void)user;
  (void)toggle;
  return {Status::NotSupported, "category is not supported now"};
}

Status ApplyKeyPatternAction(redis::AclUser &user, const KeyPatternAction &action) {
  (void)user;
  (void)action;
  return {Status::NotSupported, "key pattern is not supported now"};
}

Status ApplyChannelPatternAction(redis::AclUser &user, const ChannelPatternAction &action) {
  (void)user;
  (void)action;
  return {Status::NotSupported, "channel pattern is not supported now"};
}

// Forward declaration for ParseSetUserToken
Status ParseSetUserToken(const std::string &token, std::vector<SetUserAction> *actions);

// Apply a SelectorAction by parsing tokens and applying them to a new selector
Status ApplySelectorAction(redis::AclUser &user, const SelectorAction &action) {
  (void)user;
  (void)action;
  return {Status::NotSupported, "selector is not supported now"};
}

// Unified action visitor for applying SetUserAction to AclUser
Status ApplySetUserAction(redis::AclUser &user, const SetUserAction &action) {
  return std::visit(
      Overloaded{[&](const EnableAction &) -> Status {
                   user.enabled = true;
                   return Status::OK();
                 },
                 [&](const DisableAction &) -> Status {
                   user.enabled = false;
                   return Status::OK();
                 },
                 [&](const ResetUserAction &) -> Status {
                   ResetUserState(user);
                   return Status::OK();
                 },
                 [&](const ResetPassAction &) -> Status {
                   user.passwords.clear();
                   return Status::OK();
                 },
                 [&](const NoPassAction &) -> Status {
                   user.passwords.clear();
                   return Status::OK();
                 },
                 [&](const ClearSelectorsAction &) -> Status {
                   if (user.allowed_commands.size() > 1) {
                     user.allowed_commands.erase(user.allowed_commands.begin() + 1, user.allowed_commands.end());
                   }
                   return Status::OK();
                 },
                 [&](const PasswordAction &pwd) { return ApplyPasswordAction(user, pwd); },
                 [&](const CommandToggleAction &toggle) { return ApplyCommandToggleAction(user, toggle); },
                 [&](const CategoryToggleAction &toggle) { return ApplyCategoryToggleAction(user, toggle); },
                 [&](const KeyPatternAction &pattern) { return ApplyKeyPatternAction(user, pattern); },
                 [&](const ChannelPatternAction &pattern) { return ApplyChannelPatternAction(user, pattern); },
                 [&](const SelectorAction &sel) { return ApplySelectorAction(user, sel); }},
      action);
}

bool IsValidSha256Hex(std::string_view value) {
  if (value.size() != 64) {
    return false;
  }
  for (char ch : value) {
    if (!std::isxdigit(static_cast<unsigned char>(ch))) {
      return false;
    }
  }
  return true;
}

void ResetUserState(redis::AclUser &user) {
  user.enabled = false;
  user.passwords.clear();
  user.allowed_commands.clear();
  redis::AclSelector root{};
  root.flags = 0;
  user.allowed_commands.emplace_back(std::move(root));
}

redis::AclSelector &EnsureRootSelector(redis::AclUser &user) {
  if (user.allowed_commands.empty()) {
    redis::AclSelector root{};
    root.flags = 0;
    user.allowed_commands.emplace_back(std::move(root));
  }
  return user.allowed_commands.front();
}

// Type aliases for clarity
inline void TrimCommandBitmap(std::vector<uint64_t> &bitmap) { TrimBitmap(bitmap); }

bool CommandBitmapIsAll(const std::vector<uint64_t> &bitmap) {
  auto normalized = NormalizeBitmap(bitmap);
  auto all_bitmap = NormalizeBitmap(AclCommandManager::Instance().BuildBitmapForAllCommands());
  if (normalized.empty() || all_bitmap.empty()) {
    return false;
  }
  return normalized == all_bitmap;
}

std::vector<std::string> BuildCommandRules(const std::vector<uint64_t> &bitmap) {
  auto normalized = NormalizeBitmap(bitmap);
  if (normalized.empty()) {
    return {"-@all"};
  }

  auto all_bitmap = NormalizeBitmap(AclCommandManager::Instance().BuildBitmapForAllCommands());
  if (!all_bitmap.empty() && normalized == all_bitmap) {
    return {"+@all"};
  }

  auto commands = AclCommandManager::Instance().CommandsFromBitmap(normalized);
  std::vector<std::string> result;
  result.reserve(commands.size());
  for (const auto &cmd : commands) {
    result.emplace_back("+" + cmd);
  }
  return result;
}

const std::vector<std::string_view> &AclCategoryNames() {
  static const std::vector<std::string_view> names = {
      "@unknown", "@bit",       "@bloomfilter", "@cluster", "@function",    "@geo",    "@hash",   "@hll",
      "@json",    "@key",       "@list",        "@pubsub",  "@replication", "@script", "@search", "@server",
      "@set",     "@sortedint", "@stream",      "@string",  "@tdigest",     "@txn",    "@zset",   "@timeseries"};
  return names;
}

std::vector<uint32_t> BuildAllCategoryBitmap() {
  const auto &names = AclCategoryNames();
  if (names.empty()) {
    return {};
  }
  const size_t chunk_count = (names.size() + 31) / 32;
  std::vector<uint32_t> bitmap(chunk_count, 0);
  for (size_t index = 0; index < names.size(); ++index) {
    const size_t chunk = index / 32;
    const size_t bit = index % 32;
    bitmap[chunk] |= (1U << bit);
  }
  return bitmap;
}

std::vector<std::string> BuildCategoryRules(const std::vector<uint32_t> &bitmap) {
  auto normalized = NormalizeBitmap(bitmap);
  std::vector<std::string> result;
  if (normalized.empty()) {
    return result;
  }

  auto all_bitmap = NormalizeBitmap(BuildAllCategoryBitmap());
  if (!all_bitmap.empty() && normalized == all_bitmap) {
    result.emplace_back("+@all");
    return result;
  }

  const auto &names = AclCategoryNames();
  for (size_t chunk_index = 0; chunk_index < normalized.size(); ++chunk_index) {
    uint32_t chunk = normalized[chunk_index];
    if (chunk == 0) continue;
    for (uint32_t bit = 0; bit < 32; ++bit) {
      if ((chunk & (1U << bit)) == 0) continue;
      size_t category_index = chunk_index * 32 + bit;
      if (category_index < names.size()) {
        result.emplace_back("+" + std::string(names[category_index]));
      } else {
        result.emplace_back("+@category" + std::to_string(category_index));
      }
    }
  }

  return result;
}

std::vector<std::string> BuildSelectorFlags(const AclSelector &selector) {
  std::vector<std::string> flags;
  if (selector.key_patterns.empty()) {
    flags.emplace_back("allkeys");
  }
  if (selector.channels.empty()) {
    flags.emplace_back("allchannels");
  }
  if (CommandBitmapIsAll(selector.allowed_commands)) {
    flags.emplace_back("allcommands");
  }
  return flags;
}

// Format key pattern with permission prefix for display
std::string FormatKeyPattern(const AclKeyPattern &kp) {
  if (kp.flags == kAclKeyAll) {
    return "~" + kp.pattern;
  }
  std::string result = "%";
  if (kp.flags & kAclKeyRead) {
    result += "R";
  }
  if (kp.flags & kAclKeyWrite) {
    result += "W";
  }
  result += "~" + kp.pattern;
  return result;
}

std::vector<std::string> BuildSelectorKeys(const AclSelector &selector) {
  if (selector.key_patterns.empty()) {
    return {"*"};
  }
  std::vector<std::string> result;
  result.reserve(selector.key_patterns.size());
  for (const auto &kp : selector.key_patterns) {
    result.push_back(FormatKeyPattern(kp));
  }
  return result;
}

std::vector<std::string> BuildSelectorChannels(const AclSelector &selector) {
  if (selector.channels.empty()) {
    return {"*"};
  }
  return selector.channels;
}

std::vector<std::string> BuildUserFlags(const AclUser &user, const AclSelector *root_selector) {
  std::vector<std::string> flags;
  flags.emplace_back(user.enabled ? "on" : "off");
  flags.emplace_back(user.passwords.empty() ? "nopass" : "hashed");

  if (root_selector != nullptr) {
    auto selector_flags = BuildSelectorFlags(*root_selector);
    for (const auto &flag : selector_flags) {
      if (std::find(flags.begin(), flags.end(), flag) == flags.end()) {
        flags.push_back(flag);
      }
    }
  }

  return flags;
}

std::vector<std::string> BuildPasswordArray(const AclUser &user) {
  std::vector<std::string> passwords;
  passwords.reserve(user.passwords.size());
  for (const auto &pwd : user.passwords) {
    passwords.push_back(pwd);
  }
  return passwords;
}

std::string BuildMapReply(Connection *conn, const std::vector<std::pair<std::string, std::string>> &entries) {
  std::string result = conn->HeaderOfMap(static_cast<int64_t>(entries.size()));
  for (const auto &entry : entries) {
    result += redis::BulkString(entry.first);
    result += entry.second;
  }
  return result;
}

std::string FormatSelector(Connection *conn, const AclSelector &selector) {
  std::vector<std::pair<std::string, std::string>> entries;
  entries.emplace_back("flags", redis::ArrayOfBulkStrings(BuildSelectorFlags(selector)));
  entries.emplace_back("commands", redis::ArrayOfBulkStrings(BuildCommandRules(selector.allowed_commands)));
  entries.emplace_back("keys", redis::ArrayOfBulkStrings(BuildSelectorKeys(selector)));
  entries.emplace_back("channels", redis::ArrayOfBulkStrings(BuildSelectorChannels(selector)));
  auto categories = BuildCategoryRules(selector.allowed_category);
  if (!categories.empty()) {
    entries.emplace_back("categories", redis::ArrayOfBulkStrings(categories));
  }
  return BuildMapReply(conn, entries);
}

std::string FormatAclUser(Connection *conn, const AclUser &user) {
  const AclSelector *root_selector = user.allowed_commands.empty() ? nullptr : &user.allowed_commands.front();

  std::vector<std::pair<std::string, std::string>> entries;
  entries.emplace_back("flags", redis::ArrayOfBulkStrings(BuildUserFlags(user, root_selector)));
  entries.emplace_back("passwords", redis::ArrayOfBulkStrings(BuildPasswordArray(user)));

  const AclSelector default_selector{};

  const auto &selector_ref = root_selector ? *root_selector : default_selector;
  entries.emplace_back("commands", redis::ArrayOfBulkStrings(BuildCommandRules(selector_ref.allowed_commands)));
  entries.emplace_back("keys", redis::ArrayOfBulkStrings(BuildSelectorKeys(selector_ref)));
  entries.emplace_back("channels", redis::ArrayOfBulkStrings(BuildSelectorChannels(selector_ref)));
  auto categories = BuildCategoryRules(selector_ref.allowed_category);
  if (!categories.empty()) {
    entries.emplace_back("categories", redis::ArrayOfBulkStrings(categories));
  }

  std::vector<std::string> selector_payloads;
  if (user.allowed_commands.size() > 1) {
    selector_payloads.reserve(user.allowed_commands.size() - 1);
    for (size_t i = 1; i < user.allowed_commands.size(); ++i) {
      selector_payloads.emplace_back(FormatSelector(conn, user.allowed_commands[i]));
    }
  }
  entries.emplace_back("selectors", redis::Array(selector_payloads));
  entries.emplace_back("namespace", redis::BulkString(user.ns));

  return BuildMapReply(conn, entries);
}

Status ParseSetUserToken(const std::string &token, std::vector<SetUserAction> *actions) {
  if (token.empty()) {
    return {Status::RedisParseErr, "ACL SETUSER modifier must not be empty"};
  }

  auto lowered = util::ToLower(token);
  if (lowered == "on") {
    actions->emplace_back(EnableAction{});
    return Status::OK();
  }
  if (lowered == "off") {
    actions->emplace_back(DisableAction{});
    return Status::OK();
  }
  if (lowered == "reset") {
    actions->emplace_back(ResetUserAction{});
    return Status::OK();
  }
  if (lowered == "resetpass") {
    actions->emplace_back(ResetPassAction{});
    return Status::OK();
  }
  if (lowered == "nopass") {
    actions->emplace_back(NoPassAction{});
    return Status::OK();
  }
  if (lowered == "clearselectors") {
    actions->emplace_back(ClearSelectorsAction{});
    return Status::OK();
  }
  if (lowered == "allcommands" || lowered == "+@all") {
    actions->emplace_back(CommandToggleAction{true, true, {}, token});
    return Status::OK();
  }
  if (lowered == "nocommands" || lowered == "-@all") {
    actions->emplace_back(CommandToggleAction{false, true, {}, token});
    return Status::OK();
  }
  if (lowered == "allkeys") {
    actions->emplace_back(KeyPatternAction{KeyPatternAction::Kind::kAll, "", kAclKeyAll});
    return Status::OK();
  }
  if (lowered == "resetkeys") {
    actions->emplace_back(KeyPatternAction{KeyPatternAction::Kind::kReset, "", kAclKeyAll});
    return Status::OK();
  }
  if (lowered == "allchannels") {
    actions->emplace_back(ChannelPatternAction{ChannelPatternAction::Kind::kAll, ""});
    return Status::OK();
  }
  if (lowered == "resetchannels") {
    actions->emplace_back(ChannelPatternAction{ChannelPatternAction::Kind::kReset, ""});
    return Status::OK();
  }

  switch (token.front()) {
    case '~': {
      if (token.size() == 1) {
        return {Status::RedisParseErr, "ACL SETUSER key pattern modifier requires a pattern"};
      }
      actions->emplace_back(KeyPatternAction{KeyPatternAction::Kind::kAdd, token.substr(1), kAclKeyAll});
      return Status::OK();
    }
    case '%': {
      // Parse key permission prefix: %R~pattern, %W~pattern, %RW~pattern
      if (token.size() < 3) {
        return {Status::RedisParseErr, "Syntax error"};
      }
      uint32_t flags = 0;
      size_t offset = 1;
      while (offset < token.size() && token[offset] != '~') {
        int ch = std::toupper(static_cast<unsigned char>(token[offset]));
        if (ch == 'R' && !(flags & kAclKeyRead)) {
          flags |= kAclKeyRead;
        } else if (ch == 'W' && !(flags & kAclKeyWrite)) {
          flags |= kAclKeyWrite;
        } else {
          return {Status::RedisParseErr, "Syntax error"};
        }
        ++offset;
      }
      if (flags == 0 || offset >= token.size() || token[offset] != '~') {
        return {Status::RedisParseErr, "Syntax error"};
      }
      std::string pattern = token.substr(offset + 1);
      actions->emplace_back(KeyPatternAction{KeyPatternAction::Kind::kAdd, pattern, flags});
      return Status::OK();
    }
    case '&': {
      if (token.size() == 1) {
        return {Status::RedisParseErr, "ACL SETUSER channel pattern modifier requires a pattern"};
      }
      actions->emplace_back(ChannelPatternAction{ChannelPatternAction::Kind::kAdd, token.substr(1)});
      return Status::OK();
    }
    case '+':
    case '-': {
      if (token.size() < 2) {
        return {Status::RedisParseErr, "ACL SETUSER modifier is missing a payload"};
      }
      const bool allow = token.front() == '+';
      if (token[1] == '@') {
        if (token.size() == 2) {
          return {Status::RedisParseErr, "ACL SETUSER category modifier requires a category"};
        }
        std::string category = util::ToLower(token.substr(2));
        bool is_all = category == "all";
        actions->emplace_back(CategoryToggleAction{allow, is_all, category, token});
      } else {
        std::string command = util::ToLower(token.substr(1));
        actions->emplace_back(CommandToggleAction{allow, false, command, token});
      }
      return Status::OK();
    }
    case '>': {
      if (token.size() == 1) {
        return {Status::RedisParseErr, "ACL SETUSER password modifier requires a value"};
      }
      actions->emplace_back(PasswordAction{PasswordAction::Kind::kAddPlain, token.substr(1), token});
      return Status::OK();
    }
    case '<': {
      if (token.size() == 1) {
        return {Status::RedisParseErr, "ACL SETUSER password modifier requires a value"};
      }
      actions->emplace_back(PasswordAction{PasswordAction::Kind::kRemovePlain, token.substr(1), token});
      return Status::OK();
    }
    case '#': {
      if (token.size() == 1 || !IsValidSha256Hex(token.substr(1))) {
        return {Status::RedisParseErr, "invalid hashed password"};
      }
      actions->emplace_back(PasswordAction{PasswordAction::Kind::kAddHashed, token.substr(1), token});
      return Status::OK();
    }
    case '!': {
      if (token.size() == 1 || !IsValidSha256Hex(token.substr(1))) {
        return {Status::RedisParseErr, "invalid hashed password"};
      }
      actions->emplace_back(PasswordAction{PasswordAction::Kind::kRemoveHashed, token.substr(1), token});
      return Status::OK();
    }
    case '(': {
      // Selector syntax: (options...)
      // The token should start with '(' and end with ')'
      if (token.size() < 2 || token.back() != ')') {
        return {Status::RedisParseErr, "Unmatched parenthesis in ACL selector"};
      }
      // Extract content between parentheses
      std::string content = token.substr(1, token.size() - 2);
      // Trim leading/trailing whitespace
      auto start = content.find_first_not_of(" \t");
      auto end = content.find_last_not_of(" \t");
      if (start == std::string::npos) {
        // Empty selector is valid - creates selector with no permissions
        actions->emplace_back(SelectorAction{{}});
        return Status::OK();
      }
      content = content.substr(start, end - start + 1);
      // Split content by spaces (simple tokenization)
      std::vector<std::string> selector_tokens;
      std::string current;
      for (char ch : content) {
        if (ch == ' ' || ch == '\t') {
          if (!current.empty()) {
            selector_tokens.push_back(current);
            current.clear();
          }
        } else {
          current += ch;
        }
      }
      if (!current.empty()) {
        selector_tokens.push_back(current);
      }
      actions->emplace_back(SelectorAction{std::move(selector_tokens)});
      return Status::OK();
    }
    default:
      break;
  }

  return {Status::RedisParseErr, fmt::format("ACL SETUSER modifier '{}' is not supported", token)};
}

std::string DetermineNamespace(Namespace *ns_mgr, const std::string &username) {
  auto delimiter = username.find('#');
  if (delimiter == std::string::npos || delimiter + 1 >= username.size()) {
    return kDefaultNamespace;
  }
  auto ns = username.substr(delimiter + 1);
  if (ns.empty() || ns == kDefaultNamespace) {
    return ns.empty() ? kDefaultNamespace : ns;
  }
  if (ns_mgr == nullptr) {
    return kDefaultNamespace;
  }
  auto token_or = ns_mgr->Get(ns);
  if (!token_or.IsOK()) {
    return kDefaultNamespace;
  }
  return ns;
}

}  // namespace

jsoncons::json AclUser::ToJson() const {
  jsoncons::json json;
  json[kJsonFieldEnabled] = enabled;
  json[kJsonFieldNamespace] = ns;

  jsoncons::json passwords_array(jsoncons::json_array_arg);
  for (const auto &password : passwords) {
    passwords_array.push_back(password);
  }
  json[kJsonFieldPasswords] = std::move(passwords_array);

  jsoncons::json selectors_array(jsoncons::json_array_arg);
  for (const auto &selector : allowed_commands) {
    selectors_array.push_back(SelectorToJson(selector));
  }
  json[kJsonFieldSelectors] = std::move(selectors_array);

  return json;
}

StatusOr<AclUser> AclUser::FromJson(const jsoncons::json &json) {
  if (!json.is_object()) {
    return {Status::NotOK, "ACL user JSON must be an object"};
  }

  if (!json.contains(kJsonFieldEnabled) || !json.contains(kJsonFieldNamespace) || !json.contains(kJsonFieldSelectors) ||
      !json.contains(kJsonFieldPasswords)) {
    return {Status::NotOK, "invalid ACL user JSON"};
  }

  AclUser user;
  if (!json[kJsonFieldEnabled].is_bool()) {
    return {Status::NotOK, "enabled must be a boolean"};
  }
  user.enabled = json[kJsonFieldEnabled].as<bool>();

  if (!json[kJsonFieldNamespace].is_string()) {
    return {Status::NotOK, "namespace must be a string"};
  }
  user.ns = json[kJsonFieldNamespace].as_string();

  const auto &passwords_json = json[kJsonFieldPasswords];
  if (!passwords_json.is_array()) {
    return {Status::NotOK, "passwords must be an array"};
  }
  for (size_t i = 0; i < passwords_json.size(); ++i) {
    const auto &password = passwords_json[i];
    if (!password.is_string()) {
      return {Status::NotOK, "passwords entries must be strings"};
    }
    user.passwords.insert(password.as_string());
  }

  const auto &selectors_json = json[kJsonFieldSelectors];
  if (!selectors_json.is_array()) {
    return {Status::NotOK, "selectors must be an array"};
  }
  for (size_t i = 0; i < selectors_json.size(); ++i) {
    const auto &selector_json = selectors_json[i];
    auto selector = GET_OR_RET(SelectorFromJson(selector_json));
    user.allowed_commands.push_back(std::move(selector));
  }

  return user;
}

AclUserManager::AclUserManager() {
  user_array_.fill(nullptr);
  // TODO: Load persisted ACL users and populate username_index_.
}

size_t AclUserManager::findFreeSlotLocked() const {
  for (size_t i = 0; i < user_array_.size(); ++i) {
    if (!std::atomic_load(&user_array_[i])) {
      return i;
    }
  }
  return -1;
}

std::shared_ptr<const AclUser> AclUserManager::GetUserByIndex(size_t index) {
  if (index >= user_array_.size()) {
    return nullptr;
  }
  return std::atomic_load(&user_array_[index]);
}

std::shared_ptr<const AclUser> AclUserManager::GetUserByUserName(const std::string &username) {
  std::shared_lock<std::shared_mutex> lock(mu_);
  auto iter = username_index_.find(username);
  if (iter == username_index_.end()) {
    return nullptr;
  }
  const auto slot = static_cast<size_t>(iter->second);
  lock.unlock();
  return GetUserByIndex(slot);
}

std::optional<size_t> AclUserManager::GetUserIndex(const std::string &username) const {
  std::shared_lock<std::shared_mutex> lock(mu_);
  auto iter = username_index_.find(username);
  if (iter == username_index_.end()) {
    return std::nullopt;
  }
  return iter->second;
}

std::shared_ptr<const AclUser> AclUserManager::AuthenticateUser(const std::string &username,
                                                                const std::string &password) {
  auto user = GetUserByUserName(username);
  if (!user || !user->enabled) {
    return nullptr;
  }

  if (user->passwords.empty()) {
    // Empty password set means NOPASS.
    return user;
  }

  if (password.empty()) {
    return nullptr;
  }

  auto digest = util::Sha256Hex(password);
  return user->passwords.find(digest) != user->passwords.end() ? user : nullptr;
}

bool AclUserManager::UpdateUser(const std::string &username, std::shared_ptr<const AclUser> user) {
  std::shared_lock<std::shared_mutex> lock(mu_);
  auto iter = username_index_.find(username);
  if (iter == username_index_.end()) {
    return false;
  }
  const auto slot = static_cast<size_t>(iter->second);
  lock.unlock();

  std::atomic_store(&user_array_[slot], std::move(user));
  return true;
}

bool AclUserManager::SetUser(const std::string &username, std::shared_ptr<const AclUser> user) {
  std::unique_lock<std::shared_mutex> lock(mu_);
  auto iter = username_index_.find(username);
  size_t slot = 0;
  if (iter == username_index_.end()) {
    slot = findFreeSlotLocked();
    if (slot < 0) {
      return false;
    }
    username_index_[username] = slot;
  } else {
    slot = iter->second;
  }

  std::atomic_store(&user_array_[slot], std::move(user));
  return true;
}

bool AclUserManager::AddUser(const std::string &username, std::shared_ptr<const AclUser> user) {
  std::unique_lock<std::shared_mutex> lock(mu_);
  if (username_index_.find(username) != username_index_.end()) {
    return false;
  }

  const size_t slot = findFreeSlotLocked();

  username_index_.emplace(username, slot);
  std::atomic_store(&user_array_[slot], std::move(user));
  return true;
}

bool AclUserManager::DeleteUser(const std::string &username) {
  std::unique_lock<std::shared_mutex> lock(mu_);
  auto iter = username_index_.find(username);
  if (iter == username_index_.end()) {
    return false;
  }

  const auto slot = static_cast<size_t>(iter->second);
  username_index_.erase(iter);
  std::atomic_store(&user_array_[slot], std::shared_ptr<const AclUser>{});
  return true;
}

void AclUserManager::Reset() {
  std::unique_lock<std::shared_mutex> lock(mu_);
  username_index_.clear();
  for (auto &slot : user_array_) {
    std::atomic_store(&slot, std::shared_ptr<const AclUser>{});
  }
}

std::vector<std::string> AclUserManager::ListUsernames() const {
  std::shared_lock<std::shared_mutex> lock(mu_);
  std::vector<std::string> names;
  names.reserve(username_index_.size());
  for (const auto &entry : username_index_) {
    names.emplace_back(entry.first);
  }
  return names;
}

std::optional<std::string> AclUserManager::GetUsernameByIndex(size_t index) const {
  std::shared_lock<std::shared_mutex> lock(mu_);
  for (const auto &entry : username_index_) {
    if (entry.second == index) {
      return entry.first;
    }
  }
  return std::nullopt;
}

StatusOr<AclUser> Acl::Get(const std::string &username) {
  auto cached_user = user_manager_->GetUserByUserName(username);
  if (cached_user) {
    return *cached_user;
  }

  std::string value;
  engine::Context ctx(storage_);
  auto cf = storage_->GetCFHandle(ColumnFamilyID::Propagate);
  auto storage_key = MakeAclStorageKey(username);
  auto s = storage_->Get(ctx, ctx.GetReadOptions(), cf, storage_key, &value);
  if (!s.ok()) {
    if (s.IsNotFound()) {
      return {Status::NotFound};
    }
    return {Status::NotOK, s.ToString()};
  }

  jsoncons::json parsed;
  try {
    parsed = jsoncons::json::parse(value);
  } catch (const std::exception &e) {
    return {Status::NotOK, std::string("failed to parse ACL user JSON: ") + e.what()};
  }

  auto user_or = AclUser::FromJson(parsed);
  if (!user_or.IsOK()) {
    return user_or;
  }

  AclUser result = user_or.GetValue();
  auto cache_entry = std::make_shared<const AclUser>(result);
  if (!user_manager_->AddUser(username, cache_entry)) {
    user_manager_->SetUser(username, cache_entry);
  }

  return result;
}

Status Acl::Set(const std::string &username, const AclUser &user) {
  auto previous = user_manager_->GetUserByUserName(username);
  auto new_entry = std::make_shared<const AclUser>(user);
  if (!user_manager_->SetUser(username, new_entry)) {
    return {Status::NotOK, "maximum number of ACL users reached"};
  }

  auto status = PersistAclUser(storage_, username, user);
  if (!status.IsOK()) {
    if (previous) {
      user_manager_->SetUser(username, previous);
    } else {
      user_manager_->DeleteUser(username);
    }
    return status;
  }

  return Status::OK();
}

Status Acl::Del(const std::string &username) {
  auto existing = user_manager_->GetUserByUserName(username);
  if (!existing) {
    return {Status::NotOK, "the ACL user was not found"};
  }

  if (!user_manager_->DeleteUser(username)) {
    return {Status::NotOK, "the ACL user was not found"};
  }

  auto status = RemoveAclUser(storage_, username);
  if (!status.IsOK()) {
    user_manager_->SetUser(username, existing);
    return status;
  }

  return Status::OK();
}

Status Acl::LoadAcl() {
  AclCommandManager::Instance().Seal();

  std::vector<std::pair<std::string, std::shared_ptr<const AclUser>>> loaded_users;

  engine::Context ctx(storage_);
  auto cf = storage_->GetCFHandle(ColumnFamilyID::Propagate);
  auto read_options = ctx.DefaultScanOptions();
  util::UniqueIterator iter(ctx, read_options, cf);
  if (!iter) {
    return {Status::NotOK, "failed to create iterator for ACL loading"};
  }

  const std::string prefix(kAclStoragePrefix);
  iter->Seek(prefix);
  for (; iter->Valid() && iter->key().starts_with(prefix); iter->Next()) {
    std::string username = iter->key().ToString();
    username.erase(0, prefix.size());

    std::string value = iter->value().ToString();
    jsoncons::json parsed;
    try {
      parsed = jsoncons::json::parse(value);
    } catch (const std::exception &e) {
      return {Status::NotOK, "failed to parse ACL user " + username + ": " + e.what()};
    }

    auto user_or = AclUser::FromJson(parsed);
    if (!user_or.IsOK()) {
      auto status = user_or.ToStatus();
      return status.Prefixed("failed to load ACL user " + username);
    }

    loaded_users.emplace_back(username, std::make_shared<const AclUser>(user_or.GetValue()));
  }

  if (!iter->status().ok()) {
    return {Status::NotOK, iter->status().ToString()};
  }

  auto new_manager = std::make_unique<AclUserManager>();
  for (auto &entry : loaded_users) {
    if (!new_manager->AddUser(entry.first, entry.second)) {
      return {Status::NotOK, "maximum number of ACL users reached"};
    }
  }

  user_manager_ = std::move(new_manager);
  return Status::OK();
}

std::optional<size_t> Acl::GetUserIndex(const std::string &username) {
  if (!user_manager_) {
    return std::nullopt;
  }
  return user_manager_->GetUserIndex(username);
}

std::shared_ptr<const AclUser> Acl::GetCachedUserByIndex(size_t index) {
  if (!user_manager_) {
    return nullptr;
  }
  return user_manager_->GetUserByIndex(index);
}

std::vector<std::string> Acl::ListUsers() const {
  if (!user_manager_) {
    return {};
  }
  return user_manager_->ListUsernames();
}

std::optional<std::string> Acl::GetUsernameByIndex(size_t index) const {
  if (!user_manager_) {
    return std::nullopt;
  }
  return user_manager_->GetUsernameByIndex(index);
}

Status Acl::HandleSetUser(Namespace *ns_mgr, const std::string &username, const std::vector<std::string> &modifiers,
                          std::string *output) {
  if (username.empty()) {
    return {Status::RedisParseErr, errWrongNumOfArguments};
  }

  std::vector<SetUserAction> actions;
  actions.reserve(modifiers.size());
  for (const auto &token : modifiers) {
    auto status = ParseSetUserToken(token, &actions);
    if (!status.IsOK()) {
      return status;
    }
  }

  const std::string user_namespace = DetermineNamespace(ns_mgr, username);
  auto user_or = Get(username);
  AclUser user;
  if (user_or.Is<Status::NotFound>()) {
    user.enabled = false;
    user.ns = user_namespace;
    ResetUserState(user);
  } else if (!user_or.IsOK()) {
    return user_or.ToStatus();
  } else {
    user = user_or.GetValue();
  }

  if (user.allowed_commands.empty()) {
    ResetUserState(user);
    user.ns = user_or.Is<Status::NotFound>() ? user_namespace : user.ns;
  }

  // Apply all actions using the unified visitor
  for (const auto &action : actions) {
    auto status = ApplySetUserAction(user, action);
    if (!status.IsOK()) {
      return status;
    }
  }

  auto status = Set(username, user);
  if (!status.IsOK()) {
    return status;
  }

  *output = redis::RESP_OK;
  return Status::OK();
}

Status Acl::HandleGetUser(Connection *conn, const std::string &username, std::string *output) {
  auto user_or = Get(username);
  if (user_or.Is<Status::NotFound>()) {
    *output = conn->NilArray();
    return Status::OK();
  }
  if (!user_or.IsOK()) {
    return user_or.ToStatus();
  }

  *output = FormatAclUser(conn, user_or.GetValue());
  return Status::OK();
}

Status Acl::HandleWhoAmI(Connection *conn, std::string *output) {
  std::string username;
  if (conn->HasAclProfile()) {
    username = conn->GetAclUsername();
  }
  if (username.empty()) {
    username = "default";
  }
  *output = redis::BulkString(username);
  return Status::OK();
}

Status Acl::HandleUsers(Connection *conn, std::string *output) const {
  auto names = ListUsers();
  *output = conn->MultiBulkString(names);
  return Status::OK();
}

}  // namespace redis

namespace redis {

AclCommandManager &AclCommandManager::Instance() {
  static AclCommandManager instance;
  return instance;
}

size_t AclCommandManager::RegisterCommand(const std::string &name, [[maybe_unused]] redis::CommandCategory category) {
  const std::string key = util::ToLower(name);

  // Registration happens during server startup on a single thread before ACL is sealed,
  // so we intentionally skip locking here to avoid unnecessary contention.
  auto iter = command_bits_.find(key);
  if (iter != command_bits_.end()) {
    return iter->second;
  }

  if (sealed_.load(std::memory_order_acquire)) {
    fatal("Attempt to register ACL command `{}` after manager sealed.", name);
  }

  auto bit = next_bit_++;
  command_bits_.emplace(key, bit);
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
  const size_t index = bit.value() / 64;
  if (bitmap.size() <= index) {
    return false;
  }
  return (bitmap[index] & (UINT64_C(1) << (bit.value() % 64))) != 0;
}

void AclCommandManager::Seal() {
  std::unique_lock<std::shared_mutex> lock(mu_);
  sealed_ = true;
}

}  // namespace redis
