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

#include "acl.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <exception>
#include <fstream>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "acl_actions.h"
#include "commands/commander.h"
#include "commands/error_constants.h"
#include "common/db_util.h"
#include "common/string_util.h"
#include "fmt/format.h"
#include "server/namespace.h"
#include "server/redis_connection.h"
#include "server/redis_reply.h"
#include "server/server.h"

namespace redis {

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

namespace {

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

bool HasInvalidUsernameChar(const std::string &username) {
  return std::any_of(username.begin(), username.end(), [](char ch) {
    const auto uch = static_cast<unsigned char>(ch);
    return std::isspace(uch) != 0 || std::iscntrl(uch) != 0;
  });
}

std::vector<std::string> BuildListChannels(const AclSelector &selector) {
  auto channels = BuildSelectorChannels(selector);
  for (auto &channel : channels) {
    if (channel != "*") {
      channel = "&" + channel;
    }
  }
  return channels;
}

std::vector<std::string> BuildSelectorRuleTokens(const AclSelector &selector) {
  std::vector<std::string> tokens;

  auto command_rules = BuildCommandRules(selector.allowed_commands);
  tokens.insert(tokens.end(), command_rules.begin(), command_rules.end());

  auto key_rules = BuildSelectorKeys(selector);
  tokens.insert(tokens.end(), key_rules.begin(), key_rules.end());

  auto channel_rules = BuildListChannels(selector);
  tokens.insert(tokens.end(), channel_rules.begin(), channel_rules.end());

  return tokens;
}

std::string BuildAclListEntry(const std::string &username, const AclUser &user) {
  std::vector<std::string> tokens;
  tokens.emplace_back("user");
  tokens.emplace_back(username);

  const AclSelector *root_selector = user.allowed_commands.empty() ? nullptr : &user.allowed_commands.front();
  auto flags = BuildUserFlags(user, root_selector);
  tokens.insert(tokens.end(), flags.begin(), flags.end());

  for (const auto &password : user.passwords) {
    tokens.emplace_back("#" + password);
  }

  if (root_selector != nullptr) {
    auto root_rules = BuildSelectorRuleTokens(*root_selector);
    tokens.insert(tokens.end(), root_rules.begin(), root_rules.end());
  }

  for (size_t i = 1; i < user.allowed_commands.size(); ++i) {
    auto selector_rules = BuildSelectorRuleTokens(user.allowed_commands[i]);
    tokens.emplace_back("(" + util::StringJoin(selector_rules, " ") + ")");
  }

  return util::StringJoin(tokens, " ");
}

std::vector<std::string> BuildAclCategoriesReply() {
  std::vector<std::string> categories;
  categories.reserve(AclCategoryNames().size());
  for (auto category : AclCategoryNames()) {
    std::string value(category);
    if (!value.empty() && value.front() == '@') {
      value.erase(0, 1);
    }
    categories.emplace_back(std::move(value));
  }
  return categories;
}

}  // namespace

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
  user_manager_->SetUser(username, new_entry);

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
      return {Status::NotOK, "failed to populate ACL user cache"};
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
  if (HasInvalidUsernameChar(username)) {
    return {Status::RedisParseErr, "The username contains invalid characters"};
  }

  std::vector<SetUserAction> actions;
  auto merged_modifiers_or = MergeSelectorArguments(modifiers);
  if (!merged_modifiers_or.IsOK()) {
    return std::move(merged_modifiers_or).ToStatus();
  }
  auto merged_modifiers = std::move(merged_modifiers_or.GetValue());

  actions.reserve(merged_modifiers.size());
  for (const auto &token : merged_modifiers) {
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

Status Acl::HandleList(Connection *conn, std::string *output) const {
  auto names = ListUsers();
  std::vector<std::string> entries;
  entries.reserve(names.size());

  for (const auto &name : names) {
    auto user = user_manager_->GetUserByUserName(name);
    if (!user) {
      continue;
    }
    entries.emplace_back(BuildAclListEntry(name, *user));
  }

  *output = conn->MultiBulkString(entries);
  return Status::OK();
}

Status Acl::HandleCat(Connection *conn, const std::optional<std::string> &category, std::string *output) const {
  if (!category.has_value()) {
    *output = conn->MultiBulkString(BuildAclCategoriesReply());
    return Status::OK();
  }

  std::string normalized = util::ToLower(category.value());
  if (!normalized.empty() && normalized.front() == '@') {
    normalized.erase(0, 1);
  }

  auto bitmap_or = AclCommandManager::Instance().BuildBitmapForCategory(normalized);
  if (!bitmap_or.IsOK()) {
    return bitmap_or.ToStatus();
  }

  auto commands = AclCommandManager::Instance().CommandsFromBitmap(bitmap_or.GetValue());
  *output = conn->MultiBulkString(commands);
  return Status::OK();
}

Status Acl::HandleDelUser(const std::vector<std::string> &usernames, std::vector<std::string> *deleted_usernames,
                          std::string *output) {
  if (usernames.empty()) {
    return {Status::RedisParseErr, errWrongNumOfArguments};
  }

  if (deleted_usernames) {
    deleted_usernames->clear();
  }

  int64_t deleted = 0;
  for (const auto &username : usernames) {
    if (util::ToLower(username) == "default") {
      return {Status::RedisExecErr, "The 'default' user cannot be removed"};
    }
    if (!GetUserIndex(username).has_value()) {
      continue;
    }
    auto s = Del(username);
    if (!s.IsOK()) {
      return s;
    }
    ++deleted;
    if (deleted_usernames) {
      deleted_usernames->emplace_back(username);
    }
  }

  *output = redis::Integer(deleted);
  return Status::OK();
}

Status Acl::HandleDryRun(Connection *conn, const std::string &username, const std::vector<std::string> &command_tokens,
                         std::string *output) {
  if (username.empty() || command_tokens.empty()) {
    return {Status::RedisParseErr, errWrongNumOfArguments};
  }

  auto user_index = GetUserIndex(username);
  if (!user_index.has_value()) {
    return {Status::RedisExecErr, "ACL user does not exist"};
  }

  auto user = GetCachedUserByIndex(*user_index);
  if (!user) {
    return {Status::RedisExecErr, "ACL user does not exist"};
  }

  auto cmd_or = Server::LookupAndCreateCommand(command_tokens.front());
  if (!cmd_or.IsOK()) {
    return cmd_or.ToStatus();
  }

  auto cmd = std::move(cmd_or.GetValue());
  const auto *attributes = cmd->GetAttributes();
  if (!attributes->CheckArity(static_cast<int>(command_tokens.size()))) {
    return {Status::RedisExecErr, "wrong number of arguments"};
  }
  cmd->SetArgs(command_tokens);
  auto parse_status = cmd->Parse();
  if (!parse_status.IsOK()) {
    return parse_status;
  }

  const bool had_acl_profile = conn->HasAclProfile();
  const std::string previous_username = conn->GetAclUsername();
  const size_t previous_user_index = conn->GetAclUserIndex();
  auto previous_user = had_acl_profile ? GetCachedUserByIndex(previous_user_index) : nullptr;

  conn->SetAclProfile(username, *user_index, user);
  auto acl_status = conn->CheckAclCommandAllowed(
      this, attributes, command_tokens, attributes->GenerateFlags(command_tokens, *conn->GetServer()->GetConfig()));
  if (had_acl_profile && previous_user) {
    conn->SetAclProfile(previous_username, previous_user_index, previous_user);
  } else {
    conn->ClearAclProfile();
  }

  if (!acl_status.IsOK()) {
    return acl_status;
  }

  *output = redis::RESP_OK;
  return Status::OK();
}

Status Acl::ApplyReplicatedUpdate(const std::string &username, const std::string &value) {
  jsoncons::json parsed;
  try {
    parsed = jsoncons::json::parse(value);
  } catch (const std::exception &e) {
    return {Status::NotOK, std::string("failed to parse replicated ACL user JSON: ") + e.what()};
  }

  auto user_or = AclUser::FromJson(parsed);
  if (!user_or.IsOK()) {
    return user_or.ToStatus();
  }

  auto new_entry = std::make_shared<const AclUser>(user_or.GetValue());
  user_manager_->SetUser(username, new_entry);

  return Status::OK();
}

Status Acl::ApplyReplicatedDeletion(const std::string &username) {
  if (!user_manager_->DeleteUser(username)) {
    // User might not exist in cache, which is okay for replication
    return Status::OK();
  }
  return Status::OK();
}

Status Acl::LoadAclFromFile(const std::string &path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    return {Status::NotOK, "Failed to open ACL file: " + path};
  }

  // Parse all lines first; abort on any error before making changes.
  struct ParsedLine {
    std::string username;
    std::vector<std::string> modifiers;
  };
  std::vector<ParsedLine> parsed_lines;
  std::string line;
  int line_num = 0;
  while (std::getline(file, line)) {
    ++line_num;
    // Strip trailing CR
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    // Skip blank lines and comments
    size_t first_non_space = line.find_first_not_of(" \t");
    if (first_non_space == std::string::npos || line[first_non_space] == '#') {
      continue;
    }
    auto tokens_or = util::SplitArguments(line);
    if (!tokens_or.IsOK()) {
      return tokens_or.ToStatus().Prefixed(fmt::format("ACL file line {}", line_num));
    }
    auto &tokens = tokens_or.GetValue();
    if (tokens.empty()) continue;
    if (util::ToLower(tokens[0]) != "user" || tokens.size() < 2) {
      return {Status::NotOK,
              fmt::format("ACL file line {}: expected 'user <username> [rules...]', got '{}'", line_num, line)};
    }
    ParsedLine pl;
    pl.username = tokens[1];
    pl.modifiers.assign(tokens.begin() + 2, tokens.end());
    parsed_lines.emplace_back(std::move(pl));
  }

  if (file.bad()) {
    return {Status::NotOK, "Error reading ACL file: " + path};
  }

  // Collect usernames present in file
  std::set<std::string> file_usernames;
  for (const auto &pl : parsed_lines) {
    file_usernames.insert(pl.username);
  }

  // Delete users present in storage but not in file (except "default" stays unless file has it)
  auto existing = ListUsers();
  for (const auto &existing_name : existing) {
    if (file_usernames.find(existing_name) == file_usernames.end()) {
      auto s = Del(existing_name);
      (void)s;  // best-effort; continue even on error
    }
  }

  // Apply all users from file
  std::string dummy_output;
  for (const auto &pl : parsed_lines) {
    auto s = HandleSetUser(nullptr, pl.username, pl.modifiers, &dummy_output);
    if (!s.IsOK()) {
      return s.Prefixed(fmt::format("ACL file user '{}'", pl.username));
    }
  }

  return Status::OK();
}

Status Acl::SaveAclToFile(const std::string &path) const {
  // Write to a temporary file then rename for atomicity.
  std::string tmp_path = path + ".tmp";
  std::ofstream file(tmp_path);
  if (!file.is_open()) {
    return {Status::NotOK, "Failed to open ACL temp file for writing: " + tmp_path};
  }

  auto names = ListUsers();
  for (const auto &name : names) {
    auto user = user_manager_->GetUserByUserName(name);
    if (!user) continue;
    file << BuildAclListEntry(name, *user) << "\n";
    if (!file) {
      return {Status::NotOK, "Error writing ACL file"};
    }
  }
  file.close();
  if (!file) {
    return {Status::NotOK, "Error flushing ACL file"};
  }

  if (std::rename(tmp_path.c_str(), path.c_str()) != 0) {
    return {Status::NotOK,
            fmt::format("Failed to rename ACL temp file '{}' to '{}': {}", tmp_path, path, strerror(errno))};
  }
  return Status::OK();
}

}  // namespace redis
