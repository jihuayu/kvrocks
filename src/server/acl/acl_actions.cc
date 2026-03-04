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

#include "acl_actions.h"

#include <algorithm>
#include <cctype>
#include <variant>

#include "acl_command_manager.h"
#include "common/string_util.h"
#include "fmt/format.h"
#include "server/redis_connection.h"
#include "server/redis_reply.h"
#include "vendor/sha256.h"

namespace redis {

bool CommandBitmapIsAll(const std::vector<uint64_t> &bitmap);

namespace {

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

inline void TrimCommandBitmap(std::vector<uint64_t> &bitmap) { TrimBitmap(bitmap); }

void BitOrBitmap(std::vector<uint64_t> &dst, const std::vector<uint64_t> &src) {
  if (dst.size() < src.size()) {
    dst.resize(src.size(), 0);
  }
  for (size_t i = 0; i < src.size(); ++i) {
    dst[i] |= src[i];
  }
  TrimCommandBitmap(dst);
}

void BitAndNotBitmap(std::vector<uint64_t> &dst, const std::vector<uint64_t> &mask) {
  for (size_t i = 0; i < dst.size() && i < mask.size(); ++i) {
    dst[i] &= ~mask[i];
  }
  TrimCommandBitmap(dst);
}

void RefreshAllCommandsFlag(AclSelector &selector) {
  if (CommandBitmapIsAll(selector.allowed_commands)) {
    selector.flags |= kAclSelectorAllCommands;
  } else {
    selector.flags &= ~kAclSelectorAllCommands;
  }
}

// Action application functions
Status ApplyPasswordAction(AclUser &user, const PasswordAction &action) {
  switch (action.kind) {
    case PasswordAction::Kind::kAddPlain: {
      auto digest = Sha256Hex(action.value);
      user.passwords.insert(digest);
      user.nopass = false;
      break;
    }
    case PasswordAction::Kind::kRemovePlain: {
      auto digest = Sha256Hex(action.value);
      user.passwords.erase(digest);
      break;
    }
    case PasswordAction::Kind::kAddHashed:
      user.passwords.insert(util::ToLower(action.value));
      user.nopass = false;
      break;
    case PasswordAction::Kind::kRemoveHashed:
      user.passwords.erase(util::ToLower(action.value));
      break;
  }
  return Status::OK();
}

Status ApplyCommandToggleAction(AclUser &user, const CommandToggleAction &toggle) {
  auto &root = EnsureRootSelector(user);
  auto &command_manager = AclCommandManager::Instance();

  if (toggle.all) {
    root.allowed_commands = toggle.allow ? command_manager.BuildBitmapForAllCommands() : std::vector<uint64_t>{};
    if (toggle.allow) {
      root.flags |= kAclSelectorAllCommands;
    } else {
      root.flags &= ~kAclSelectorAllCommands;
    }
    return Status::OK();
  }

  auto bit = command_manager.GetCommandBit(toggle.command);
  if (!bit.has_value()) {
    auto subcommand_pos = toggle.command.find('|');
    if (subcommand_pos != std::string::npos) {
      bit = command_manager.GetCommandBit(toggle.command.substr(0, subcommand_pos));
    }
  }
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
  RefreshAllCommandsFlag(root);
  return Status::OK();
}

Status ApplyCategoryToggleAction(AclUser &user, const CategoryToggleAction &toggle) {
  auto &root = EnsureRootSelector(user);
  auto &command_manager = AclCommandManager::Instance();
  std::vector<uint64_t> category_bitmap;

  if (toggle.all) {
    category_bitmap = command_manager.BuildBitmapForAllCommands();
  } else {
    auto bitmap_or = command_manager.BuildBitmapForCategory(toggle.category);
    if (!bitmap_or.IsOK()) {
      return bitmap_or.ToStatus();
    }
    category_bitmap = std::move(bitmap_or.GetValue());
  }

  if (toggle.allow) {
    BitOrBitmap(root.allowed_commands, category_bitmap);
  } else {
    BitAndNotBitmap(root.allowed_commands, category_bitmap);
  }
  RefreshAllCommandsFlag(root);
  return Status::OK();
}

Status ApplyKeyPatternAction(AclUser &user, const KeyPatternAction &action) {
  auto &root = EnsureRootSelector(user);
  switch (action.kind) {
    case KeyPatternAction::Kind::kReset:
      root.key_patterns.clear();
      root.flags &= ~kAclSelectorAllKeys;
      return Status::OK();
    case KeyPatternAction::Kind::kAll:
      root.key_patterns.clear();
      root.flags |= kAclSelectorAllKeys;
      return Status::OK();
    case KeyPatternAction::Kind::kAdd:
      if (action.pattern.empty()) {
        return {Status::RedisParseErr, "ACL SETUSER key pattern modifier requires a pattern"};
      }
      if ((root.flags & kAclSelectorAllKeys) != 0) {
        return {Status::RedisParseErr,
                "Adding a pattern after the * pattern (or the 'allkeys' flag) is not valid and does not have any "
                "effect. Try 'resetkeys' to start with an empty list of patterns"};
      }
      if (std::find(root.key_patterns.begin(), root.key_patterns.end(), AclKeyPattern{action.pattern, action.flags}) ==
          root.key_patterns.end()) {
        root.key_patterns.emplace_back(action.pattern, action.flags);
      }
      return Status::OK();
  }
  return Status::OK();
}

Status ApplyChannelPatternAction(AclUser &user, const ChannelPatternAction &action) {
  auto &root = EnsureRootSelector(user);
  switch (action.kind) {
    case ChannelPatternAction::Kind::kReset:
      root.channels.clear();
      root.flags &= ~kAclSelectorAllChannels;
      return Status::OK();
    case ChannelPatternAction::Kind::kAll:
      root.channels.clear();
      root.flags |= kAclSelectorAllChannels;
      return Status::OK();
    case ChannelPatternAction::Kind::kAdd:
      if (action.pattern.empty()) {
        return {Status::RedisParseErr, "ACL SETUSER channel pattern modifier requires a pattern"};
      }
      if ((root.flags & kAclSelectorAllChannels) != 0) {
        return {Status::RedisParseErr,
                "Adding a pattern after the * pattern (or the 'allchannels' flag) is not valid and does not have any "
                "effect. Try 'resetchannels' to start with an empty list of channels"};
      }
      if (std::find(root.channels.begin(), root.channels.end(), action.pattern) == root.channels.end()) {
        root.channels.emplace_back(action.pattern);
      }
      return Status::OK();
  }
  return Status::OK();
}

Status ApplySelectorAction(AclUser &user, const SelectorAction &action) {
  AclUser selector_user;
  ResetUserState(selector_user);

  for (const auto &token : action.tokens) {
    std::vector<SetUserAction> nested_actions;
    auto parse_status = ParseSetUserToken(token, &nested_actions);
    if (!parse_status.IsOK()) {
      return parse_status;
    }
    for (const auto &nested_action : nested_actions) {
      if (std::holds_alternative<EnableAction>(nested_action) || std::holds_alternative<DisableAction>(nested_action) ||
          std::holds_alternative<ResetUserAction>(nested_action) ||
          std::holds_alternative<ResetPassAction>(nested_action) ||
          std::holds_alternative<NoPassAction>(nested_action) ||
          std::holds_alternative<PasswordAction>(nested_action) ||
          std::holds_alternative<ClearSelectorsAction>(nested_action) ||
          std::holds_alternative<SanitizePayloadAction>(nested_action) ||
          std::holds_alternative<SelectorAction>(nested_action)) {
        return {Status::RedisParseErr, "ACL selector only supports command/key/channel modifiers"};
      }
      auto apply_status = ApplySetUserAction(selector_user, nested_action);
      if (!apply_status.IsOK()) {
        return apply_status;
      }
    }
  }

  auto &root = EnsureRootSelector(selector_user);
  root.flags &= ~kAclSelectorRoot;
  user.allowed_commands.push_back(root);
  return Status::OK();
}

}  // namespace

StatusOr<std::vector<std::string>> MergeSelectorArguments(const std::vector<std::string> &modifiers) {
  std::vector<std::string> merged;
  merged.reserve(modifiers.size());

  for (size_t i = 0; i < modifiers.size(); ++i) {
    const auto &token = modifiers[i];
    if (token.empty() || token.front() != '(') {
      merged.push_back(token);
      continue;
    }

    if (token.back() == ')') {
      merged.push_back(token);
      continue;
    }

    std::string combined = token;
    bool matched = false;
    while (++i < modifiers.size()) {
      combined += " ";
      combined += modifiers[i];
      if (!modifiers[i].empty() && modifiers[i].back() == ')') {
        matched = true;
        break;
      }
    }

    if (!matched) {
      return {Status::RedisParseErr, fmt::format("Unmatched parenthesis in acl selector starting at '{}'.", token)};
    }
    merged.push_back(std::move(combined));
  }

  return merged;
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
  if (lowered == "sanitize-payload") {
    actions->emplace_back(SanitizePayloadAction{true});
    return Status::OK();
  }
  if (lowered == "skip-sanitize-payload") {
    actions->emplace_back(SanitizePayloadAction{false});
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
      if (pattern.empty()) {
        return {Status::RedisParseErr, "Syntax error"};
      }
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

Status ApplySetUserAction(AclUser &user, const SetUserAction &action) {
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
                   user.nopass = false;
                   return Status::OK();
                 },
                 [&](const NoPassAction &) -> Status {
                   user.passwords.clear();
                   user.nopass = true;
                   return Status::OK();
                 },
                 [&](const SanitizePayloadAction &sanitize) -> Status {
                   user.sanitize_payload = sanitize.enabled;
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

// Formatting and display functions implementation

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
  if ((selector.flags & kAclSelectorAllKeys) != 0) {
    flags.emplace_back("allkeys");
  }
  if ((selector.flags & kAclSelectorAllChannels) != 0) {
    flags.emplace_back("allchannels");
  }
  if ((selector.flags & kAclSelectorAllCommands) != 0 || CommandBitmapIsAll(selector.allowed_commands)) {
    flags.emplace_back("allcommands");
  }
  return flags;
}

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
  if ((selector.flags & kAclSelectorAllKeys) != 0) {
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
  if ((selector.flags & kAclSelectorAllChannels) != 0) {
    return {"*"};
  }
  return selector.channels;
}

std::vector<std::string> BuildUserFlags(const AclUser &user, const AclSelector *root_selector) {
  std::vector<std::string> flags;
  flags.emplace_back(user.enabled ? "on" : "off");
  if (user.nopass) {
    flags.emplace_back("nopass");
  }
  flags.emplace_back(user.sanitize_payload ? "sanitize-payload" : "skip-sanitize-payload");

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

std::string BuildSelectorReply(Connection *conn, const AclSelector &selector) {
  std::vector<std::pair<std::string, std::string>> selector_entries;
  selector_entries.emplace_back("commands", redis::ArrayOfBulkStrings(BuildCommandRules(selector.allowed_commands)));
  selector_entries.emplace_back("keys", redis::ArrayOfBulkStrings(BuildSelectorKeys(selector)));
  selector_entries.emplace_back("channels", redis::ArrayOfBulkStrings(BuildSelectorChannels(selector)));
  return BuildMapReply(conn, selector_entries);
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

  std::vector<std::string> selector_replies;
  for (size_t i = 1; i < user.allowed_commands.size(); ++i) {
    selector_replies.emplace_back(BuildSelectorReply(conn, user.allowed_commands[i]));
  }
  entries.emplace_back("selectors", redis::Array(selector_replies));

  return BuildMapReply(conn, entries);
}

}  // namespace redis
