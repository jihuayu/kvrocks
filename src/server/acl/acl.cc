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

#include <exception>

#include "acl_actions.h"
#include "common/db_util.h"
#include "server/namespace.h"
#include "server/redis_connection.h"
#include "server/redis_reply.h"

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
  if (!user_manager_->SetUser(username, new_entry)) {
    return {Status::NotOK, "maximum number of ACL users reached"};
  }

  return Status::OK();
}

Status Acl::ApplyReplicatedDeletion(const std::string &username) {
  if (!user_manager_->DeleteUser(username)) {
    // User might not exist in cache, which is okay for replication
    return Status::OK();
  }
  return Status::OK();
}

}  // namespace redis
