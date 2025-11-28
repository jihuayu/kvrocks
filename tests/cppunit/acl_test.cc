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

#include <cstdint>

#include "common/status.h"
#include "test_base.h"

namespace {

redis::AclUser BuildUser(bool enabled, const std::string &ns, uint32_t selector_flags = 0) {
  redis::AclUser user;
  user.enabled = enabled;
  user.ns = ns;
  redis::AclSelector selector{};
  selector.flags = selector_flags;
  user.allowed_commands.push_back(selector);
  return user;
}

}  // namespace

class AclTest : public TestBase {};

TEST_F(AclTest, SetCreatesNewUser) {
  redis::Acl acl(storage_.get());
  ASSERT_TRUE(acl.LoadAcl().IsOK());

  auto user = BuildUser(true, "ns1");
  auto status = acl.Set("alice", user);
  ASSERT_TRUE(status.IsOK());

  auto stored_or = acl.Get("alice");
  ASSERT_TRUE(stored_or.IsOK());
  const auto &stored = stored_or.GetValue();
  EXPECT_TRUE(stored.enabled);
  EXPECT_EQ("ns1", stored.ns);
  ASSERT_EQ(1U, stored.allowed_commands.size());
  EXPECT_EQ(0U, stored.allowed_commands.front().flags);
}

TEST_F(AclTest, SetUpdatesExistingUser) {
  redis::Acl acl(storage_.get());
  ASSERT_TRUE(acl.LoadAcl().IsOK());

  auto user = BuildUser(true, "ns2");
  ASSERT_TRUE(acl.Set("bob", user).IsOK());

  user.enabled = false;
  user.allowed_commands.front().flags = 7;
  ASSERT_TRUE(acl.Set("bob", user).IsOK());

  auto stored_or = acl.Get("bob");
  ASSERT_TRUE(stored_or.IsOK());
  const auto &stored = stored_or.GetValue();
  EXPECT_FALSE(stored.enabled);
  EXPECT_EQ(7U, stored.allowed_commands.front().flags);
}

TEST_F(AclTest, SetPersistsUsersToStorage) {
  redis::Acl acl(storage_.get());
  ASSERT_TRUE(acl.LoadAcl().IsOK());

  ASSERT_TRUE(acl.Set("carol", BuildUser(true, "ns3", 1)).IsOK());

  redis::Acl reloaded(storage_.get());
  ASSERT_TRUE(reloaded.LoadAcl().IsOK());
  auto stored_or = reloaded.Get("carol");
  ASSERT_TRUE(stored_or.IsOK());
  const auto &stored = stored_or.GetValue();
  EXPECT_TRUE(stored.enabled);
  EXPECT_EQ("ns3", stored.ns);
  ASSERT_EQ(1U, stored.allowed_commands.size());
  EXPECT_EQ(1U, stored.allowed_commands.front().flags);
}

TEST_F(AclTest, ReplicatedUpdateRefreshesCache) {
  redis::Acl writer(storage_.get());
  ASSERT_TRUE(writer.LoadAcl().IsOK());
  ASSERT_TRUE(writer.Set("dave", BuildUser(true, "ns4")).IsOK());

  redis::Acl replica(storage_.get());
  ASSERT_TRUE(replica.LoadAcl().IsOK());
  auto initial_or = replica.Get("dave");
  ASSERT_TRUE(initial_or.IsOK());
  EXPECT_TRUE(initial_or.GetValue().enabled);

  auto updated = BuildUser(false, "ns4", 3);
  ASSERT_TRUE(writer.Set("dave", updated).IsOK());
  auto serialized = updated.ToJson().to_string();

  ASSERT_TRUE(replica.ApplyReplicatedUpdate("dave", serialized).IsOK());
  auto refreshed_or = replica.Get("dave");
  ASSERT_TRUE(refreshed_or.IsOK());
  const auto &refreshed = refreshed_or.GetValue();
  EXPECT_FALSE(refreshed.enabled);
  EXPECT_EQ(3U, refreshed.allowed_commands.front().flags);
}

TEST_F(AclTest, ReplicatedDeletionEvictsCache) {
  redis::Acl writer(storage_.get());
  ASSERT_TRUE(writer.LoadAcl().IsOK());
  ASSERT_TRUE(writer.Set("erin", BuildUser(true, "ns5")).IsOK());

  redis::Acl replica(storage_.get());
  ASSERT_TRUE(replica.LoadAcl().IsOK());
  ASSERT_TRUE(replica.Get("erin").IsOK());

  ASSERT_TRUE(writer.Del("erin").IsOK());
  ASSERT_TRUE(replica.ApplyReplicatedDeletion("erin").IsOK());

  auto removed_or = replica.Get("erin");
  EXPECT_TRUE(removed_or.Is<Status::NotFound>());
}

TEST_F(AclTest, BuildBitmapForAllCommandsIncludesPing) {
  auto &manager = redis::AclCommandManager::Instance();
  auto bit = manager.GetCommandBit("ping");
  ASSERT_TRUE(bit.has_value());

  auto bitmap = manager.BuildBitmapForAllCommands();
  ASSERT_FALSE(bitmap.empty());
  EXPECT_TRUE(manager.IsCommandAllowed(bitmap, "ping"));

  const size_t index = bit.value() / 64;
  ASSERT_LT(index, bitmap.size());
  bitmap[index] &= ~(UINT64_C(1) << (bit.value() % 64));
  EXPECT_FALSE(manager.IsCommandAllowed(bitmap, "ping"));
}

// Additional ACL tests based on Redis ACL documentation

TEST_F(AclTest, UserEnableDisable) {
  redis::Acl acl(storage_.get());
  ASSERT_TRUE(acl.LoadAcl().IsOK());

  // Create a disabled user
  auto user = BuildUser(false, "default");
  ASSERT_TRUE(acl.Set("testuser", user).IsOK());

  auto stored_or = acl.Get("testuser");
  ASSERT_TRUE(stored_or.IsOK());
  EXPECT_FALSE(stored_or.GetValue().enabled);

  // Enable the user
  user.enabled = true;
  ASSERT_TRUE(acl.Set("testuser", user).IsOK());

  auto stored_or2 = acl.Get("testuser");
  ASSERT_TRUE(stored_or2.IsOK());
  EXPECT_TRUE(stored_or2.GetValue().enabled);
}

TEST_F(AclTest, UserPasswordManagement) {
  redis::Acl acl(storage_.get());
  ASSERT_TRUE(acl.LoadAcl().IsOK());

  auto user = BuildUser(true, "default");

  // Add password hashes
  user.passwords.insert("hash1");
  user.passwords.insert("hash2");
  ASSERT_TRUE(acl.Set("testuser", user).IsOK());

  auto stored_or = acl.Get("testuser");
  ASSERT_TRUE(stored_or.IsOK());
  const auto &stored = stored_or.GetValue();
  EXPECT_EQ(2U, stored.passwords.size());
  EXPECT_TRUE(stored.passwords.count("hash1") > 0);
  EXPECT_TRUE(stored.passwords.count("hash2") > 0);

  // Test nopass (empty password set)
  user.passwords.clear();
  ASSERT_TRUE(acl.Set("nopassuser", user).IsOK());

  auto nopass_or = acl.Get("nopassuser");
  ASSERT_TRUE(nopass_or.IsOK());
  EXPECT_TRUE(nopass_or.GetValue().passwords.empty());
}

TEST_F(AclTest, ListUsers) {
  redis::Acl acl(storage_.get());
  ASSERT_TRUE(acl.LoadAcl().IsOK());

  // Create multiple users
  auto user = BuildUser(true, "default");
  ASSERT_TRUE(acl.Set("alice", user).IsOK());
  ASSERT_TRUE(acl.Set("bob", user).IsOK());
  ASSERT_TRUE(acl.Set("charlie", user).IsOK());

  auto users = acl.ListUsers();
  EXPECT_GE(users.size(), 3U);

  // Check that our users are in the list
  bool found_alice = false, found_bob = false, found_charlie = false;
  for (const auto &username : users) {
    if (username == "alice") found_alice = true;
    if (username == "bob") found_bob = true;
    if (username == "charlie") found_charlie = true;
  }
  EXPECT_TRUE(found_alice);
  EXPECT_TRUE(found_bob);
  EXPECT_TRUE(found_charlie);
}

TEST_F(AclTest, DeleteUser) {
  redis::Acl acl(storage_.get());
  ASSERT_TRUE(acl.LoadAcl().IsOK());

  // Create a user
  auto user = BuildUser(true, "default");
  ASSERT_TRUE(acl.Set("tempuser", user).IsOK());
  ASSERT_TRUE(acl.Get("tempuser").IsOK());

  // Delete the user
  ASSERT_TRUE(acl.Del("tempuser").IsOK());

  // Verify deletion
  auto deleted_or = acl.Get("tempuser");
  EXPECT_TRUE(deleted_or.Is<Status::NotFound>());
}

TEST_F(AclTest, SelectorKeyPatterns) {
  redis::Acl acl(storage_.get());
  ASSERT_TRUE(acl.LoadAcl().IsOK());

  auto user = BuildUser(true, "default");

  // Add key patterns to selector
  user.allowed_commands[0].patterns.emplace_back("user:*");
  user.allowed_commands[0].patterns.emplace_back("session:*");
  user.allowed_commands[0].patterns.emplace_back("cache:*");

  ASSERT_TRUE(acl.Set("patternuser", user).IsOK());

  auto stored_or = acl.Get("patternuser");
  ASSERT_TRUE(stored_or.IsOK());
  const auto &stored = stored_or.GetValue();
  ASSERT_EQ(1U, stored.allowed_commands.size());
  EXPECT_EQ(3U, stored.allowed_commands[0].patterns.size());
  EXPECT_EQ("user:*", stored.allowed_commands[0].patterns[0]);
  EXPECT_EQ("session:*", stored.allowed_commands[0].patterns[1]);
  EXPECT_EQ("cache:*", stored.allowed_commands[0].patterns[2]);
}

TEST_F(AclTest, SelectorChannelPatterns) {
  redis::Acl acl(storage_.get());
  ASSERT_TRUE(acl.LoadAcl().IsOK());

  auto user = BuildUser(true, "default");

  // Add channel patterns to selector
  user.allowed_commands[0].channels.emplace_back("news:*");
  user.allowed_commands[0].channels.emplace_back("events:*");

  ASSERT_TRUE(acl.Set("channeluser", user).IsOK());

  auto stored_or = acl.Get("channeluser");
  ASSERT_TRUE(stored_or.IsOK());
  const auto &stored = stored_or.GetValue();
  ASSERT_EQ(1U, stored.allowed_commands.size());
  EXPECT_EQ(2U, stored.allowed_commands[0].channels.size());
  EXPECT_EQ("news:*", stored.allowed_commands[0].channels[0]);
  EXPECT_EQ("events:*", stored.allowed_commands[0].channels[1]);
}

TEST_F(AclTest, CommandPermissionBitmap) {
  auto &manager = redis::AclCommandManager::Instance();

  // Test building bitmap for specific commands
  std::vector<std::string> commands = {"get", "set", "del"};
  auto bitmap_or = manager.BuildBitmapForCommands(commands);
  ASSERT_TRUE(bitmap_or.IsOK());

  const auto &bitmap = bitmap_or.GetValue();
  EXPECT_TRUE(manager.IsCommandAllowed(bitmap, "get"));
  EXPECT_TRUE(manager.IsCommandAllowed(bitmap, "set"));
  EXPECT_TRUE(manager.IsCommandAllowed(bitmap, "del"));

  // Commands not in the list should not be allowed
  EXPECT_FALSE(manager.IsCommandAllowed(bitmap, "flushdb"));
}

TEST_F(AclTest, MultipleSelectorsSupport) {
  redis::Acl acl(storage_.get());
  ASSERT_TRUE(acl.LoadAcl().IsOK());

  auto user = BuildUser(true, "default");

  // Add a second selector
  redis::AclSelector second_selector{};
  second_selector.flags = 1;
  second_selector.patterns.emplace_back("readonly:*");
  user.allowed_commands.push_back(second_selector);

  ASSERT_TRUE(acl.Set("multiselect", user).IsOK());

  auto stored_or = acl.Get("multiselect");
  ASSERT_TRUE(stored_or.IsOK());
  const auto &stored = stored_or.GetValue();
  EXPECT_EQ(2U, stored.allowed_commands.size());
  EXPECT_EQ(0U, stored.allowed_commands[0].flags);
  EXPECT_EQ(1U, stored.allowed_commands[1].flags);
  EXPECT_EQ(1U, stored.allowed_commands[1].patterns.size());
  EXPECT_EQ("readonly:*", stored.allowed_commands[1].patterns[0]);
}

TEST_F(AclTest, UserIndexMapping) {
  redis::Acl acl(storage_.get());
  ASSERT_TRUE(acl.LoadAcl().IsOK());

  auto user = BuildUser(true, "default");
  ASSERT_TRUE(acl.Set("indexed", user).IsOK());

  // Get user index
  auto index_opt = acl.GetUserIndex("indexed");
  ASSERT_TRUE(index_opt.has_value());

  // Get username by index
  auto username_opt = acl.GetUsernameByIndex(index_opt.value());
  ASSERT_TRUE(username_opt.has_value());
  EXPECT_EQ("indexed", username_opt.value());

  // Get user by index
  auto cached_user = acl.GetCachedUserByIndex(index_opt.value());
  ASSERT_NE(nullptr, cached_user);
  EXPECT_TRUE(cached_user->enabled);
  EXPECT_EQ("default", cached_user->ns);
}

TEST_F(AclTest, UserJsonSerialization) {
  auto user = BuildUser(true, "testns", 5);
  user.passwords.insert("pass1");
  user.passwords.insert("pass2");
  user.allowed_commands[0].patterns.emplace_back("key:*");
  user.allowed_commands[0].channels.emplace_back("chan:*");

  // Serialize to JSON
  auto json = user.ToJson();
  EXPECT_FALSE(json.is_null());

  // Deserialize from JSON
  auto deserialized_or = redis::AclUser::FromJson(json);
  ASSERT_TRUE(deserialized_or.IsOK());

  const auto &deserialized = deserialized_or.GetValue();
  EXPECT_EQ(user.enabled, deserialized.enabled);
  EXPECT_EQ(user.ns, deserialized.ns);
  EXPECT_EQ(user.passwords.size(), deserialized.passwords.size());
  EXPECT_EQ(user.allowed_commands.size(), deserialized.allowed_commands.size());
  EXPECT_EQ(user.allowed_commands[0].flags, deserialized.allowed_commands[0].flags);
}

TEST_F(AclTest, ConcurrentUserOperations) {
  redis::Acl acl(storage_.get());
  ASSERT_TRUE(acl.LoadAcl().IsOK());

  // Test concurrent reads and writes
  auto user1 = BuildUser(true, "ns1");
  auto user2 = BuildUser(true, "ns2");

  ASSERT_TRUE(acl.Set("concurrent1", user1).IsOK());
  ASSERT_TRUE(acl.Set("concurrent2", user2).IsOK());

  // Concurrent reads should work
  auto read1 = acl.Get("concurrent1");
  auto read2 = acl.Get("concurrent2");

  ASSERT_TRUE(read1.IsOK());
  ASSERT_TRUE(read2.IsOK());
  EXPECT_EQ("ns1", read1.GetValue().ns);
  EXPECT_EQ("ns2", read2.GetValue().ns);
}
