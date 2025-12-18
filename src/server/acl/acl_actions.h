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

#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "acl_user.h"
#include "common/status.h"

namespace redis {

class Connection;

// Action types for ACL SETUSER command parsing
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

// Parse a single token from ACL SETUSER command into actions
Status ParseSetUserToken(const std::string &token, std::vector<SetUserAction> *actions);

// Apply a single action to an AclUser
Status ApplySetUserAction(AclUser &user, const SetUserAction &action);

// Formatting and display functions
const std::vector<std::string_view> &AclCategoryNames();
std::vector<uint32_t> BuildAllCategoryBitmap();
bool CommandBitmapIsAll(const std::vector<uint64_t> &bitmap);
std::vector<std::string> BuildCommandRules(const std::vector<uint64_t> &bitmap);
std::vector<std::string> BuildCategoryRules(const std::vector<uint32_t> &bitmap);
std::vector<std::string> BuildSelectorFlags(const AclSelector &selector);
std::string FormatKeyPattern(const AclKeyPattern &kp);
std::vector<std::string> BuildSelectorKeys(const AclSelector &selector);
std::vector<std::string> BuildSelectorChannels(const AclSelector &selector);
std::vector<std::string> BuildUserFlags(const AclUser &user, const AclSelector *root_selector);
std::vector<std::string> BuildPasswordArray(const AclUser &user);
std::string BuildMapReply(Connection *conn, const std::vector<std::pair<std::string, std::string>> &entries);
std::string FormatAclUser(Connection *conn, const AclUser &user);

}  // namespace redis
