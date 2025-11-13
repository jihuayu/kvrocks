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
#include <exception>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/db_util.h"
#include "common/sha256.h"
#include "common/status.h"
#include "common/logging.h"
#include "common/string_util.h"

namespace redis {

namespace {

constexpr const char *kJsonFieldEnabled = "enabled";
constexpr const char *kJsonFieldNamespace = "namespace";
constexpr const char *kJsonFieldPasswords = "passwords";
constexpr const char *kJsonFieldSelectors = "selectors";
constexpr const char *kJsonFieldFlags = "flags";
constexpr const char *kJsonFieldAllowedCommands = "allowed_commands";
constexpr const char *kJsonFieldAllowedCategories = "allowed_categories";
constexpr const char *kJsonFieldPatterns = "patterns";
constexpr const char *kJsonFieldChannels = "channels";
std::string MakeAclStorageKey(const std::string &username) {
  return std::string(kAclStoragePrefix) + username;
}

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
    std::vector<uint64_t> legacy_bitmap_chunks;
    bool has_string_elements = false;
    bool has_numeric_elements = false;

    for (size_t i = 0; i < cmds.size(); ++i) {
      const auto &val = cmds[i];
      if (val.is_string()) {
        has_string_elements = true;
        command_names.push_back(val.as_string());
      } else if (val.is_number()) {
        has_numeric_elements = true;
        legacy_bitmap_chunks.push_back(val.as<uint64_t>());
      } else {
        return {Status::NotOK, "allowed_commands entries must be strings"};
      }
    }

    if (has_string_elements && has_numeric_elements) {
      return {Status::NotOK, "allowed_commands entries must not mix strings and numbers"};
    }

    if (has_string_elements) {
      auto bitmap_or = AclCommandManager::Instance().BuildBitmapForCommands(command_names);
      if (!bitmap_or.IsOK()) {
        return bitmap_or.ToStatus().Prefixed("failed to parse allowed_commands");
      }
      selector.allowed_commands = std::move(bitmap_or.GetValue());
    } else {
      selector.allowed_commands = std::move(legacy_bitmap_chunks);
    }
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

  if (json.contains(kJsonFieldPatterns)) {
    const auto &patterns = json[kJsonFieldPatterns];
    if (!patterns.is_array()) {
      return {Status::NotOK, "patterns must be an array"};
    }
    selector.patterns.reserve(patterns.size());
    for (size_t i = 0; i < patterns.size(); ++i) {
      const auto &val = patterns[i];
      if (!val.is_string()) {
        return {Status::NotOK, "patterns entries must be strings"};
      }
      selector.patterns.push_back(val.as_string());
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

  jsoncons::json pattern_array(jsoncons::json_array_arg);
  for (const auto &pattern : selector.patterns) {
    pattern_array.push_back(pattern);
  }
  json[kJsonFieldPatterns] = std::move(pattern_array);

  jsoncons::json channel_array(jsoncons::json_array_arg);
  for (const auto &channel : selector.channels) {
    channel_array.push_back(channel);
  }
  json[kJsonFieldChannels] = std::move(channel_array);

  return json;
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

Status Acl::ApplyReplicatedUpdate(const std::string &username, std::string_view serialized_user) {
  if (!user_manager_) {
    user_manager_ = std::make_unique<AclUserManager>();
  }
  jsoncons::json parsed;
  try {
    parsed = jsoncons::json::parse(serialized_user);
  } catch (const std::exception &e) {
    return {Status::NotOK, std::string("failed to parse ACL user JSON: ") + e.what()};
  }

  auto user_or = AclUser::FromJson(parsed);
  if (!user_or.IsOK()) {
    return user_or.ToStatus();
  }

  auto entry = std::make_shared<const AclUser>(user_or.GetValue());
  if (!user_manager_->SetUser(username, entry)) {
    return {Status::NotOK, "maximum number of ACL users reached"};
  }

  return Status::OK();
}

Status Acl::ApplyReplicatedDeletion(const std::string &username) {
  if (!user_manager_) {
    user_manager_ = std::make_unique<AclUserManager>();
  }
  if (!user_manager_->DeleteUser(username)) {
    return Status::OK();
  }
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
