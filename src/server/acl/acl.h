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

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "acl_command_manager.h"
#include "acl_log.h"
#include "acl_user.h"
#include "common/status.h"
#include "storage/storage.h"

class Namespace;

namespace redis {

inline constexpr std::string_view kAclStoragePrefix = "acl|";

// Storage helper functions
std::string MakeAclStorageKey(const std::string &username);
Status PersistAclUser(engine::Storage *storage, const std::string &username, const AclUser &user);
Status RemoveAclUser(engine::Storage *storage, const std::string &username);

class Connection;

class Acl {
 public:
  explicit Acl(engine::Storage *storage) : storage_(storage) {}

  StatusOr<AclUser> Get(const std::string &username);
  Status Set(const std::string &username, const AclUser &user);
  Status Del(const std::string &username);
  Status LoadAcl();
  std::optional<size_t> GetUserIndex(const std::string &username);
  std::shared_ptr<const AclUser> GetCachedUserByIndex(size_t index);
  std::vector<std::string> ListUsers() const;
  std::optional<std::string> GetUsernameByIndex(size_t index) const;

  Status HandleSetUser(Namespace *ns_mgr, const std::string &username, const std::vector<std::string> &modifiers,
                       std::string *output, bool strict_namespace = true);
  Status HandleGetUser(Connection *conn, const std::string &username, std::string *output);
  static Status HandleWhoAmI(Connection *conn, std::string *output);
  Status HandleUsers(Connection *conn, std::string *output) const;
  Status HandleList(Connection *conn, std::string *output) const;
  Status HandleCat(Connection *conn, const std::optional<std::string> &category, std::string *output) const;
  Status HandleDelUser(const std::vector<std::string> &usernames, std::vector<std::string> *deleted_usernames,
                       std::string *output);
  Status HandleDryRun(Connection *conn, const std::string &username, const std::vector<std::string> &command_tokens,
                      std::string *output);

  // Replication-related methods for applying ACL changes from master
  Status ApplyReplicatedUpdate(const std::string &username, const std::string &value);
  Status ApplyReplicatedDeletion(const std::string &username);

  AclLog &GetAclLog() { return acl_log_; }

  // Load ACL users from an ACL file (one "user ..." line per line).
  Status LoadAclFromFile(const std::string &path);
  // Save all ACL users to an ACL file in "ACL LIST" format.
  Status SaveAclToFile(const std::string &path) const;

 private:
  engine::Storage *storage_;
  std::unique_ptr<AclUserManager> user_manager_;
  AclLog acl_log_;
};

}  // namespace redis
