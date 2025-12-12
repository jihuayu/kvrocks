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

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>

#include "common/status.h"
#include "test_base.h"

namespace {

// ============================================================================
// Test Utilities and Helpers
// ============================================================================

// Factory function for creating AclUser with common defaults
redis::AclUser BuildUser(bool enabled, const std::string &ns, uint32_t selector_flags = 0) {
  redis::AclUser user;
  user.enabled = enabled;
  user.ns = ns;
  redis::AclSelector selector{};
  selector.flags = selector_flags;
  user.allowed_commands.push_back(selector);
  return user;
}

// Create a user with specific key patterns
redis::AclUser BuildUserWithPatterns(const std::string &ns, const std::vector<std::string> &patterns) {
  auto user = BuildUser(true, ns);
  user.allowed_commands[0].patterns = patterns;
  return user;
}

// Create a user with specific channel patterns
redis::AclUser BuildUserWithChannels(const std::string &ns, const std::vector<std::string> &channels) {
  auto user = BuildUser(true, ns);
  user.allowed_commands[0].channels = channels;
  return user;
}

// Create a user with passwords
redis::AclUser BuildUserWithPasswords(const std::string &ns, const std::set<std::string> &passwords) {
  auto user = BuildUser(true, ns);
  user.passwords = passwords;
  return user;
}

// Helper to check if a username exists in a list
bool ContainsUsername(const std::vector<std::string> &users, const std::string &target) {
  return std::find(users.begin(), users.end(), target) != users.end();
}

}  // namespace

// ============================================================================
// Test Fixture
// ============================================================================

class AclTest : public TestBase {
 protected:
  // Helper to get a fresh Acl instance with loaded data
  std::unique_ptr<redis::Acl> CreateAcl() {
    auto acl = std::make_unique<redis::Acl>(storage_.get());
    EXPECT_TRUE(acl->LoadAcl().IsOK());
    return acl;
  }

  // Helper to assert user retrieval and return the user
  redis::AclUser GetAndAssertUser(redis::Acl &acl, const std::string &username) {
    auto user_or = acl.Get(username);
    EXPECT_TRUE(user_or.IsOK()) << "Failed to get user: " << username;
    return user_or.GetValue();
  }
};

// ============================================================================
// Basic User CRUD Operations Tests
// ============================================================================

TEST_F(AclTest, SetCreatesNewUser) {
  auto acl = CreateAcl();
  auto user = BuildUser(true, "ns1");

  ASSERT_TRUE(acl->Set("alice", user).IsOK());

  const auto &stored = GetAndAssertUser(*acl, "alice");
  EXPECT_TRUE(stored.enabled);
  EXPECT_EQ("ns1", stored.ns);
  ASSERT_EQ(1U, stored.allowed_commands.size());
  EXPECT_EQ(0U, stored.allowed_commands.front().flags);
}

TEST_F(AclTest, SetUpdatesExistingUser) {
  auto acl = CreateAcl();
  auto user = BuildUser(true, "ns2");
  ASSERT_TRUE(acl->Set("bob", user).IsOK());

  // Update user properties
  user.enabled = false;
  user.allowed_commands.front().flags = 7;
  ASSERT_TRUE(acl->Set("bob", user).IsOK());

  const auto &stored = GetAndAssertUser(*acl, "bob");
  EXPECT_FALSE(stored.enabled);
  EXPECT_EQ(7U, stored.allowed_commands.front().flags);
}

TEST_F(AclTest, SetPersistsUsersToStorage) {
  {
    auto acl = CreateAcl();
    ASSERT_TRUE(acl->Set("carol", BuildUser(true, "ns3", 1)).IsOK());
  }

  // Reload from storage
  auto reloaded = CreateAcl();
  const auto &stored = GetAndAssertUser(*reloaded, "carol");
  EXPECT_TRUE(stored.enabled);
  EXPECT_EQ("ns3", stored.ns);
  ASSERT_EQ(1U, stored.allowed_commands.size());
  EXPECT_EQ(1U, stored.allowed_commands.front().flags);
}

TEST_F(AclTest, DeleteUser) {
  auto acl = CreateAcl();
  ASSERT_TRUE(acl->Set("tempuser", BuildUser(true, "default")).IsOK());
  ASSERT_TRUE(acl->Get("tempuser").IsOK());

  ASSERT_TRUE(acl->Del("tempuser").IsOK());

  auto deleted_or = acl->Get("tempuser");
  EXPECT_TRUE(deleted_or.Is<Status::NotFound>());
}

TEST_F(AclTest, ListUsers) {
  auto acl = CreateAcl();
  auto user = BuildUser(true, "default");

  ASSERT_TRUE(acl->Set("alice", user).IsOK());
  ASSERT_TRUE(acl->Set("bob", user).IsOK());
  ASSERT_TRUE(acl->Set("charlie", user).IsOK());

  auto users = acl->ListUsers();
  EXPECT_GE(users.size(), 3U);
  EXPECT_TRUE(ContainsUsername(users, "alice"));
  EXPECT_TRUE(ContainsUsername(users, "bob"));
  EXPECT_TRUE(ContainsUsername(users, "charlie"));
}

// ============================================================================
// Replication Tests
// ============================================================================

TEST_F(AclTest, ReplicatedUpdateRefreshesCache) {
  auto writer = CreateAcl();
  ASSERT_TRUE(writer->Set("dave", BuildUser(true, "ns4")).IsOK());

  auto replica = CreateAcl();
  auto initial_or = replica->Get("dave");
  ASSERT_TRUE(initial_or.IsOK());
  EXPECT_TRUE(initial_or.GetValue().enabled);

  // Update via replication
  auto updated = BuildUser(false, "ns4", 3);
  ASSERT_TRUE(writer->Set("dave", updated).IsOK());
  auto serialized = updated.ToJson().to_string();

  ASSERT_TRUE(replica->ApplyReplicatedUpdate("dave", serialized).IsOK());
  const auto &refreshed = GetAndAssertUser(*replica, "dave");
  EXPECT_FALSE(refreshed.enabled);
  EXPECT_EQ(3U, refreshed.allowed_commands.front().flags);
}

TEST_F(AclTest, ReplicatedDeletionEvictsCache) {
  auto writer = CreateAcl();
  ASSERT_TRUE(writer->Set("erin", BuildUser(true, "ns5")).IsOK());

  auto replica = CreateAcl();
  ASSERT_TRUE(replica->Get("erin").IsOK());

  ASSERT_TRUE(writer->Del("erin").IsOK());
  ASSERT_TRUE(replica->ApplyReplicatedDeletion("erin").IsOK());

  auto removed_or = replica->Get("erin");
  EXPECT_TRUE(removed_or.Is<Status::NotFound>());
}

// ============================================================================
// User State Tests
// ============================================================================

TEST_F(AclTest, UserEnableDisable) {
  auto acl = CreateAcl();

  // Create disabled user
  ASSERT_TRUE(acl->Set("testuser", BuildUser(false, "default")).IsOK());
  EXPECT_FALSE(GetAndAssertUser(*acl, "testuser").enabled);

  // Enable user
  ASSERT_TRUE(acl->Set("testuser", BuildUser(true, "default")).IsOK());
  EXPECT_TRUE(GetAndAssertUser(*acl, "testuser").enabled);
}

TEST_F(AclTest, UserPasswordManagement) {
  auto acl = CreateAcl();

  // User with passwords
  auto user_with_passwords = BuildUserWithPasswords("default", {"hash1", "hash2"});
  ASSERT_TRUE(acl->Set("pwduser", user_with_passwords).IsOK());

  const auto &stored = GetAndAssertUser(*acl, "pwduser");
  EXPECT_EQ(2U, stored.passwords.size());
  EXPECT_TRUE(stored.passwords.count("hash1") > 0);
  EXPECT_TRUE(stored.passwords.count("hash2") > 0);

  // User with nopass (empty password set)
  ASSERT_TRUE(acl->Set("nopassuser", BuildUser(true, "default")).IsOK());
  EXPECT_TRUE(GetAndAssertUser(*acl, "nopassuser").passwords.empty());
}

// ============================================================================
// Selector and Pattern Tests
// ============================================================================

TEST_F(AclTest, SelectorKeyPatterns) {
  auto acl = CreateAcl();
  auto user = BuildUserWithPatterns("default", {"user:*", "session:*", "cache:*"});
  ASSERT_TRUE(acl->Set("patternuser", user).IsOK());

  const auto &stored = GetAndAssertUser(*acl, "patternuser");
  ASSERT_EQ(1U, stored.allowed_commands.size());
  const auto &patterns = stored.allowed_commands[0].patterns;
  EXPECT_EQ(3U, patterns.size());
  EXPECT_EQ("user:*", patterns[0]);
  EXPECT_EQ("session:*", patterns[1]);
  EXPECT_EQ("cache:*", patterns[2]);
}

TEST_F(AclTest, SelectorChannelPatterns) {
  auto acl = CreateAcl();
  auto user = BuildUserWithChannels("default", {"news:*", "events:*"});
  ASSERT_TRUE(acl->Set("channeluser", user).IsOK());

  const auto &stored = GetAndAssertUser(*acl, "channeluser");
  ASSERT_EQ(1U, stored.allowed_commands.size());
  const auto &channels = stored.allowed_commands[0].channels;
  EXPECT_EQ(2U, channels.size());
  EXPECT_EQ("news:*", channels[0]);
  EXPECT_EQ("events:*", channels[1]);
}

TEST_F(AclTest, MultipleSelectorsSupport) {
  auto acl = CreateAcl();
  auto user = BuildUser(true, "default");

  // Add second selector
  redis::AclSelector second_selector{};
  second_selector.flags = 1;
  second_selector.patterns.emplace_back("readonly:*");
  user.allowed_commands.push_back(second_selector);

  ASSERT_TRUE(acl->Set("multiselect", user).IsOK());

  const auto &stored = GetAndAssertUser(*acl, "multiselect");
  EXPECT_EQ(2U, stored.allowed_commands.size());
  EXPECT_EQ(0U, stored.allowed_commands[0].flags);
  EXPECT_EQ(1U, stored.allowed_commands[1].flags);
  EXPECT_EQ(1U, stored.allowed_commands[1].patterns.size());
  EXPECT_EQ("readonly:*", stored.allowed_commands[1].patterns[0]);
}

// ============================================================================
// Command Manager Tests
// ============================================================================

TEST_F(AclTest, BuildBitmapForAllCommandsIncludesPing) {
  auto &manager = redis::AclCommandManager::Instance();
  auto bit = manager.GetCommandBit("ping");
  ASSERT_TRUE(bit.has_value());

  auto bitmap = manager.BuildBitmapForAllCommands();
  ASSERT_FALSE(bitmap.empty());
  EXPECT_TRUE(manager.IsCommandAllowed(bitmap, "ping"));

  // Disable ping and verify
  const size_t index = bit.value() / 64;
  ASSERT_LT(index, bitmap.size());
  bitmap[index] &= ~(UINT64_C(1) << (bit.value() % 64));
  EXPECT_FALSE(manager.IsCommandAllowed(bitmap, "ping"));
}

TEST_F(AclTest, CommandPermissionBitmap) {
  auto &manager = redis::AclCommandManager::Instance();

  std::vector<std::string> allowed_commands = {"get", "set", "del"};
  auto bitmap_or = manager.BuildBitmapForCommands(allowed_commands);
  ASSERT_TRUE(bitmap_or.IsOK());

  const auto &bitmap = bitmap_or.GetValue();
  // Allowed commands should pass
  EXPECT_TRUE(manager.IsCommandAllowed(bitmap, "get"));
  EXPECT_TRUE(manager.IsCommandAllowed(bitmap, "set"));
  EXPECT_TRUE(manager.IsCommandAllowed(bitmap, "del"));

  // Commands not in list should fail
  EXPECT_FALSE(manager.IsCommandAllowed(bitmap, "flushdb"));
}

// ============================================================================
// Index Mapping Tests
// ============================================================================

TEST_F(AclTest, UserIndexMapping) {
  auto acl = CreateAcl();
  ASSERT_TRUE(acl->Set("indexed", BuildUser(true, "default")).IsOK());

  // Get user index
  auto index_opt = acl->GetUserIndex("indexed");
  ASSERT_TRUE(index_opt.has_value());

  // Get username by index
  auto username_opt = acl->GetUsernameByIndex(index_opt.value());
  ASSERT_TRUE(username_opt.has_value());
  EXPECT_EQ("indexed", username_opt.value());

  // Get user by index
  auto cached_user = acl->GetCachedUserByIndex(index_opt.value());
  ASSERT_NE(nullptr, cached_user);
  EXPECT_TRUE(cached_user->enabled);
  EXPECT_EQ("default", cached_user->ns);
}

// ============================================================================
// Serialization Tests
// ============================================================================

TEST_F(AclTest, UserJsonSerialization) {
  auto user = BuildUser(true, "testns", 5);
  user.passwords.insert("pass1");
  user.passwords.insert("pass2");
  user.allowed_commands[0].patterns.emplace_back("key:*");
  user.allowed_commands[0].channels.emplace_back("chan:*");

  // Serialize
  auto json = user.ToJson();
  EXPECT_FALSE(json.is_null());

  // Deserialize and verify
  auto deserialized_or = redis::AclUser::FromJson(json);
  ASSERT_TRUE(deserialized_or.IsOK());

  const auto &deserialized = deserialized_or.GetValue();
  EXPECT_EQ(user.enabled, deserialized.enabled);
  EXPECT_EQ(user.ns, deserialized.ns);
  EXPECT_EQ(user.passwords.size(), deserialized.passwords.size());
  EXPECT_EQ(user.allowed_commands.size(), deserialized.allowed_commands.size());
  EXPECT_EQ(user.allowed_commands[0].flags, deserialized.allowed_commands[0].flags);
}

// ============================================================================
// Concurrency Tests
// ============================================================================

TEST_F(AclTest, ConcurrentUserOperations) {
  auto acl = CreateAcl();

  ASSERT_TRUE(acl->Set("concurrent1", BuildUser(true, "ns1")).IsOK());
  ASSERT_TRUE(acl->Set("concurrent2", BuildUser(true, "ns2")).IsOK());

  // Concurrent reads should work
  auto read1 = acl->Get("concurrent1");
  auto read2 = acl->Get("concurrent2");

  ASSERT_TRUE(read1.IsOK());
  ASSERT_TRUE(read2.IsOK());
  EXPECT_EQ("ns1", read1.GetValue().ns);
  EXPECT_EQ("ns2", read2.GetValue().ns);
}
