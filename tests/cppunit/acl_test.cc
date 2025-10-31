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

