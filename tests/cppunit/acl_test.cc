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
  for (const auto &p : patterns) {
    user.allowed_commands[0].key_patterns.emplace_back(p, redis::kAclKeyAll);
  }
  return user;
}

// Create a user with specific key patterns with permissions
redis::AclUser BuildUserWithKeyPatterns(const std::string &ns,
                                        const std::vector<std::pair<std::string, uint32_t>> &patterns) {
  auto user = BuildUser(true, ns);
  for (const auto &p : patterns) {
    user.allowed_commands[0].key_patterns.emplace_back(p.first, p.second);
  }
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
  std::unique_ptr<redis::Acl> createAcl() {
    auto acl = std::make_unique<redis::Acl>(storage_.get());
    auto status = acl->LoadAcl();
    EXPECT_TRUE(status.IsOK()) << "LoadAcl failed: " << status.Msg();
    return acl;
  }

  // Helper to assert user retrieval and return the user
  static redis::AclUser getAndAssertUser(redis::Acl &acl, const std::string &username) {
    auto user_or = acl.Get(username);
    EXPECT_TRUE(user_or.IsOK()) << "Failed to get user: " << username;
    return user_or.GetValue();
  }
};

// ============================================================================
// Basic User CRUD Operations Tests
// ============================================================================

TEST_F(AclTest, SetCreatesNewUser) {
  auto acl = createAcl();
  auto user = BuildUser(true, "ns1");

  ASSERT_TRUE(acl->Set("alice", user).IsOK());

  const auto &stored = getAndAssertUser(*acl, "alice");
  EXPECT_TRUE(stored.enabled);
  EXPECT_EQ("ns1", stored.ns);
  ASSERT_EQ(1U, stored.allowed_commands.size());
  EXPECT_EQ(0U, stored.allowed_commands.front().flags);
}

TEST_F(AclTest, SetUpdatesExistingUser) {
  auto acl = createAcl();
  auto user = BuildUser(true, "ns2");
  ASSERT_TRUE(acl->Set("bob", user).IsOK());

  // Update user properties
  user.enabled = false;
  user.allowed_commands.front().flags = 7;
  ASSERT_TRUE(acl->Set("bob", user).IsOK());

  const auto &stored = getAndAssertUser(*acl, "bob");
  EXPECT_FALSE(stored.enabled);
  EXPECT_EQ(7U, stored.allowed_commands.front().flags);
}

TEST_F(AclTest, SetPersistsUsersToStorage) {
  {
    auto acl = createAcl();
    auto user = BuildUser(true, "ns3", 1);
    auto json_str = user.ToJson().to_string();
    std::cout << "User JSON: " << json_str << std::endl;

    // Try to parse it back immediately
    try {
      auto parsed = jsoncons::json::parse(json_str);
      std::cout << "JSON parsed successfully" << std::endl;
      auto user_or = redis::AclUser::FromJson(parsed);
      if (!user_or.IsOK()) {
        std::cout << "FromJson failed: " << user_or.ToStatus().Msg() << std::endl;
      } else {
        std::cout << "FromJson succeeded" << std::endl;
      }
    } catch (const std::exception &e) {
      std::cout << "Exception during parsing: " << e.what() << std::endl;
    }

    ASSERT_TRUE(acl->Set("carol", user).IsOK());
  }

  // Reload from storage
  auto reloaded = createAcl();
  const auto &stored = getAndAssertUser(*reloaded, "carol");
  EXPECT_TRUE(stored.enabled);
  EXPECT_EQ("ns3", stored.ns);
  ASSERT_EQ(1U, stored.allowed_commands.size());
  EXPECT_EQ(1U, stored.allowed_commands.front().flags);
}

TEST_F(AclTest, DeleteUser) {
  auto acl = createAcl();
  ASSERT_TRUE(acl->Set("tempuser", BuildUser(true, "default")).IsOK());
  ASSERT_TRUE(acl->Get("tempuser").IsOK());

  ASSERT_TRUE(acl->Del("tempuser").IsOK());

  auto deleted_or = acl->Get("tempuser");
  EXPECT_TRUE(deleted_or.Is<Status::NotFound>());
}

TEST_F(AclTest, VersionIncrementsOnMutations) {
  auto acl = createAcl();
  auto before_set = acl->GetVersion();

  ASSERT_TRUE(acl->Set("version_user", BuildUser(true, "default")).IsOK());
  auto after_set = acl->GetVersion();
  EXPECT_GT(after_set, before_set);

  ASSERT_TRUE(acl->Set("version_user", BuildUser(false, "default")).IsOK());
  auto after_update = acl->GetVersion();
  EXPECT_GT(after_update, after_set);

  ASSERT_TRUE(acl->Del("version_user").IsOK());
  auto after_del = acl->GetVersion();
  EXPECT_GT(after_del, after_update);
}

TEST_F(AclTest, VersionIncrementsOnReplicatedMutations) {
  auto acl = createAcl();
  auto before_update = acl->GetVersion();

  auto user = BuildUser(true, "default");
  ASSERT_TRUE(acl->ApplyReplicatedUpdate("replicated_user", user.ToJson().to_string()).IsOK());
  auto after_update = acl->GetVersion();
  EXPECT_GT(after_update, before_update);

  ASSERT_TRUE(acl->ApplyReplicatedDeletion("replicated_user").IsOK());
  auto after_delete = acl->GetVersion();
  EXPECT_GT(after_delete, after_update);
}

TEST_F(AclTest, ListUsers) {
  auto acl = createAcl();
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
// User State Tests
// ============================================================================

TEST_F(AclTest, UserEnableDisable) {
  auto acl = createAcl();

  // Create disabled user
  ASSERT_TRUE(acl->Set("testuser", BuildUser(false, "default")).IsOK());
  EXPECT_FALSE(getAndAssertUser(*acl, "testuser").enabled);

  // Enable user
  ASSERT_TRUE(acl->Set("testuser", BuildUser(true, "default")).IsOK());
  EXPECT_TRUE(getAndAssertUser(*acl, "testuser").enabled);
}

TEST_F(AclTest, UserPasswordManagement) {
  auto acl = createAcl();

  // User with passwords
  auto user_with_passwords = BuildUserWithPasswords("default", {"hash1", "hash2"});
  ASSERT_TRUE(acl->Set("pwduser", user_with_passwords).IsOK());

  const auto &stored = getAndAssertUser(*acl, "pwduser");
  EXPECT_EQ(2U, stored.passwords.size());
  EXPECT_TRUE(stored.passwords.count("hash1") > 0);
  EXPECT_TRUE(stored.passwords.count("hash2") > 0);

  // User with nopass (empty password set)
  ASSERT_TRUE(acl->Set("nopassuser", BuildUser(true, "default")).IsOK());
  EXPECT_TRUE(getAndAssertUser(*acl, "nopassuser").passwords.empty());
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
  auto acl = createAcl();
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

TEST_F(AclTest, IndexedUserLookupByUsername) {
  auto acl = createAcl();
  ASSERT_TRUE(acl->Set("indexed_user", BuildUser(true, "default")).IsOK());

  auto indexed_user = acl->GetIndexedUserByUsername("indexed_user");
  ASSERT_TRUE(indexed_user.has_value());
  ASSERT_NE(nullptr, indexed_user->user);
  EXPECT_TRUE(indexed_user->user->enabled);
  EXPECT_EQ("default", indexed_user->user->ns);

  auto index_opt = acl->GetUserIndex("indexed_user");
  ASSERT_TRUE(index_opt.has_value());
  EXPECT_EQ(index_opt.value(), indexed_user->index);
}

TEST_F(AclTest, UsernameMatchByIndexWithSlotReuse) {
  auto acl = createAcl();
  ASSERT_TRUE(acl->Set("slot_user_a", BuildUser(true, "default")).IsOK());

  auto first_index = acl->GetUserIndex("slot_user_a");
  ASSERT_TRUE(first_index.has_value());
  EXPECT_TRUE(acl->IsUsernameMatchedByIndex(first_index.value(), "slot_user_a"));

  ASSERT_TRUE(acl->Del("slot_user_a").IsOK());
  EXPECT_FALSE(acl->IsUsernameMatchedByIndex(first_index.value(), "slot_user_a"));

  ASSERT_TRUE(acl->Set("slot_user_b", BuildUser(true, "default")).IsOK());
  auto second_index = acl->GetUserIndex("slot_user_b");
  ASSERT_TRUE(second_index.has_value());
  EXPECT_EQ(first_index.value(), second_index.value());
  EXPECT_FALSE(acl->IsUsernameMatchedByIndex(second_index.value(), "slot_user_a"));
  EXPECT_TRUE(acl->IsUsernameMatchedByIndex(second_index.value(), "slot_user_b"));
}

// ============================================================================
// Serialization Tests
// ============================================================================

TEST_F(AclTest, UserJsonSerialization) {
  auto user = BuildUser(true, "testns", 5);
  user.passwords.insert("pass1");
  user.passwords.insert("pass2");
  user.allowed_commands[0].key_patterns.emplace_back("key:*", redis::kAclKeyAll);
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
  auto acl = createAcl();

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

// ============================================================================
// Key Permission Tests (New Feature: %R~, %W~, %RW~)
// ============================================================================

TEST_F(AclTest, KeyPatternPermissions) {
  auto acl = createAcl();
  auto user = BuildUserWithKeyPatterns(
      "default",
      {{"read:*", redis::kAclKeyRead}, {"write:*", redis::kAclKeyWrite}, {"readwrite:*", redis::kAclKeyAll}});
  ASSERT_TRUE(acl->Set("keypermuser", user).IsOK());

  const auto &stored = getAndAssertUser(*acl, "keypermuser");
  ASSERT_EQ(1U, stored.allowed_commands.size());
  const auto &key_patterns = stored.allowed_commands[0].key_patterns;
  ASSERT_EQ(3U, key_patterns.size());

  EXPECT_EQ("read:*", key_patterns[0].pattern);
  EXPECT_EQ(redis::kAclKeyRead, key_patterns[0].flags);

  EXPECT_EQ("write:*", key_patterns[1].pattern);
  EXPECT_EQ(redis::kAclKeyWrite, key_patterns[1].flags);

  EXPECT_EQ("readwrite:*", key_patterns[2].pattern);
  EXPECT_EQ(redis::kAclKeyAll, key_patterns[2].flags);
}

TEST_F(AclTest, KeyPatternPermissionsSerialization) {
  auto user = BuildUser(true, "default");
  user.allowed_commands[0].key_patterns.emplace_back("readonly:*", redis::kAclKeyRead);
  user.allowed_commands[0].key_patterns.emplace_back("writeonly:*", redis::kAclKeyWrite);
  user.allowed_commands[0].key_patterns.emplace_back("full:*", redis::kAclKeyAll);

  // Serialize
  auto json = user.ToJson();
  EXPECT_FALSE(json.is_null());

  // Deserialize and verify permissions are preserved
  auto deserialized_or = redis::AclUser::FromJson(json);
  ASSERT_TRUE(deserialized_or.IsOK());

  const auto &deserialized = deserialized_or.GetValue();
  ASSERT_EQ(3U, deserialized.allowed_commands[0].key_patterns.size());

  EXPECT_EQ("readonly:*", deserialized.allowed_commands[0].key_patterns[0].pattern);
  EXPECT_EQ(redis::kAclKeyRead, deserialized.allowed_commands[0].key_patterns[0].flags);

  EXPECT_EQ("writeonly:*", deserialized.allowed_commands[0].key_patterns[1].pattern);
  EXPECT_EQ(redis::kAclKeyWrite, deserialized.allowed_commands[0].key_patterns[1].flags);

  EXPECT_EQ("full:*", deserialized.allowed_commands[0].key_patterns[2].pattern);
  EXPECT_EQ(redis::kAclKeyAll, deserialized.allowed_commands[0].key_patterns[2].flags);
}

// ============================================================================
// Multiple Selector Tests
// ============================================================================

TEST_F(AclTest, MultipleSelectorsSerialization) {
  auto user = BuildUser(true, "default");

  // Root selector with read-only access
  user.allowed_commands[0].key_patterns.emplace_back("read:*", redis::kAclKeyRead);

  // Second selector with write-only access
  redis::AclSelector second_selector{};
  second_selector.flags = 0;
  second_selector.key_patterns.emplace_back("write:*", redis::kAclKeyWrite);
  second_selector.channels.emplace_back("events:*");
  user.allowed_commands.push_back(second_selector);

  // Serialize
  auto json = user.ToJson();
  EXPECT_FALSE(json.is_null());

  // Deserialize and verify
  auto deserialized_or = redis::AclUser::FromJson(json);
  ASSERT_TRUE(deserialized_or.IsOK());

  const auto &deserialized = deserialized_or.GetValue();
  ASSERT_EQ(2U, deserialized.allowed_commands.size());

  // Check first selector
  ASSERT_EQ(1U, deserialized.allowed_commands[0].key_patterns.size());
  EXPECT_EQ("read:*", deserialized.allowed_commands[0].key_patterns[0].pattern);
  EXPECT_EQ(redis::kAclKeyRead, deserialized.allowed_commands[0].key_patterns[0].flags);

  // Check second selector
  ASSERT_EQ(1U, deserialized.allowed_commands[1].key_patterns.size());
  EXPECT_EQ("write:*", deserialized.allowed_commands[1].key_patterns[0].pattern);
  EXPECT_EQ(redis::kAclKeyWrite, deserialized.allowed_commands[1].key_patterns[0].flags);
  ASSERT_EQ(1U, deserialized.allowed_commands[1].channels.size());
  EXPECT_EQ("events:*", deserialized.allowed_commands[1].channels[0]);
}

// ============================================================================
// Key Pattern Struct Tests
// ============================================================================

TEST_F(AclTest, AclKeyPatternEquality) {
  redis::AclKeyPattern p1{"key:*", redis::kAclKeyAll};
  redis::AclKeyPattern p2{"key:*", redis::kAclKeyAll};
  redis::AclKeyPattern p3{"key:*", redis::kAclKeyRead};
  redis::AclKeyPattern p4{"other:*", redis::kAclKeyAll};

  EXPECT_TRUE(p1 == p2);
  EXPECT_FALSE(p1 == p3);  // Different flags
  EXPECT_FALSE(p1 == p4);  // Different pattern
}

TEST_F(AclTest, AclKeyPatternDefaultFlags) {
  redis::AclKeyPattern default_pattern;
  EXPECT_EQ(redis::kAclKeyAll, default_pattern.flags);  // Default should be all permissions
}
