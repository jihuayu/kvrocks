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

#include "acl_user.h"

#include "acl_command_manager.h"

namespace redis {

namespace {

constexpr const char *kJsonFieldEnabled = "enabled";
constexpr const char *kJsonFieldNoPass = "nopass";
constexpr const char *kJsonFieldSanitizePayload = "sanitize_payload";
constexpr const char *kJsonFieldNamespace = "namespace";
constexpr const char *kJsonFieldPasswords = "passwords";
constexpr const char *kJsonFieldSelectors = "selectors";
constexpr const char *kJsonFieldFlags = "flags";
constexpr const char *kJsonFieldAllowedCommands = "allowed_commands";
constexpr const char *kJsonFieldAllowedCategories = "allowed_categories";
constexpr const char *kJsonFieldKeyPatterns = "key_patterns";
constexpr const char *kJsonFieldChannels = "channels";

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

}  // namespace

jsoncons::json AclUser::ToJson() const {
  jsoncons::json json;
  json[kJsonFieldEnabled] = enabled;
  json[kJsonFieldNoPass] = nopass;
  json[kJsonFieldSanitizePayload] = sanitize_payload;
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

  if (json.contains(kJsonFieldNoPass)) {
    if (!json[kJsonFieldNoPass].is_bool()) {
      return {Status::NotOK, "nopass must be a boolean"};
    }
    user.nopass = json[kJsonFieldNoPass].as<bool>();
  }

  if (json.contains(kJsonFieldSanitizePayload)) {
    if (!json[kJsonFieldSanitizePayload].is_bool()) {
      return {Status::NotOK, "sanitize_payload must be a boolean"};
    }
    user.sanitize_payload = json[kJsonFieldSanitizePayload].as<bool>();
  } else {
    // Backward compatibility with older persisted ACL schema.
    user.sanitize_payload = true;
  }

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
  if (!json.contains(kJsonFieldNoPass)) {
    // Backward compatibility with older persisted ACL schema where empty passwords implied nopass.
    user.nopass = user.passwords.empty();
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

void ResetUserState(AclUser &user) {
  user.enabled = false;
  user.nopass = false;
  user.sanitize_payload = true;
  user.passwords.clear();
  user.allowed_commands.clear();
  AclSelector root{};
  root.flags = kAclSelectorRoot;
  user.allowed_commands.emplace_back(std::move(root));
}

AclSelector &EnsureRootSelector(AclUser &user) {
  if (user.allowed_commands.empty()) {
    AclSelector root{};
    root.flags = kAclSelectorRoot;
    user.allowed_commands.emplace_back(std::move(root));
  } else {
    user.allowed_commands.front().flags |= kAclSelectorRoot;
  }
  return user.allowed_commands.front();
}

AclUserManager::AclUserManager() {
  user_array_.fill(nullptr);
  // TODO: Load persisted ACL users and populate username_index_.
}

std::optional<size_t> AclUserManager::findFreeSlotLocked() const {
  for (size_t i = 0; i < user_array_.size(); ++i) {
    if (!std::atomic_load(&user_array_[i])) {
      return i;
    }
  }
  return std::nullopt;
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

bool AclUserManager::SetUser(const std::string &username, std::shared_ptr<const AclUser> user) {
  std::unique_lock<std::shared_mutex> lock(mu_);
  auto iter = username_index_.find(username);
  size_t slot = 0;
  if (iter == username_index_.end()) {
    auto free_slot = findFreeSlotLocked();
    if (!free_slot.has_value()) {
      return false;
    }
    slot = free_slot.value();
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

  auto free_slot = findFreeSlotLocked();
  if (!free_slot.has_value()) {
    return false;
  }
  const size_t slot = free_slot.value();

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

}  // namespace redis
