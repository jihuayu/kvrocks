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

#include "commands/commander.h"

#include <gtest/gtest.h>

namespace {

struct CommandTableResetGuard {
  ~CommandTableResetGuard() { redis::CommandTable::Reset(); }
};

TEST(SubcommandResolution, ResolveRootCommandWithoutSubcommand) {
  auto resolved = redis::CommandTable::Resolve({"ping"});

  ASSERT_TRUE(resolved);
  EXPECT_EQ(resolved->root, "ping");
  EXPECT_FALSE(resolved->HasSubcommand());
  EXPECT_EQ(resolved->FullName(), "ping");
}

TEST(SubcommandResolution, ResolveRegisteredSubcommand) {
  auto resolved = redis::CommandTable::Resolve({"config", "get", "dir"});

  ASSERT_TRUE(resolved);
  EXPECT_EQ(resolved->root, "config");
  ASSERT_TRUE(resolved->HasSubcommand());
  EXPECT_EQ(*resolved->sub, "get");
  EXPECT_EQ(resolved->FullName(), "config|get");
  EXPECT_EQ(resolved->attributes->arity, 3);
}

TEST(SubcommandResolution, LookupCanonicalSubcommandAttributes) {
  auto *attr = redis::CommandTable::LookupAttributesByName("client|pause");

  ASSERT_NE(attr, nullptr);
  EXPECT_EQ(attr->name, "client|pause");
  EXPECT_TRUE(attr->InitialFlags() & redis::kCmdAdmin);
}

TEST(SubcommandResolution, ResolveUnknownSubcommand) {
  auto resolved = redis::CommandTable::Resolve({"config", "missing"});

  EXPECT_EQ(resolved.GetCode(), Status::RedisInvalidCmd);
}

TEST(SubcommandResolution, GetKeysFromResolvedSubcommand) {
  auto key_indexes = redis::CommandTable::GetKeysFromCommand({"cluster", "keyslot", "my-key"});

  ASSERT_TRUE(key_indexes);
  ASSERT_EQ(key_indexes->size(), 1U);
  EXPECT_EQ((*key_indexes)[0], 2);
}

TEST(SubcommandResolution, CanonicalLookupSurvivesCommandRename) {
  CommandTableResetGuard guard;

  auto *commands = redis::CommandTable::Get();
  auto config_iter = commands->find("config");
  ASSERT_NE(config_iter, commands->end());

  (*commands)["renamed_config"] = config_iter->second;
  commands->erase(config_iter);

  auto *attr = redis::CommandTable::LookupAttributesByName("config|get");
  ASSERT_NE(attr, nullptr);
  EXPECT_EQ(attr->name, "config|get");

  auto resolved = redis::CommandTable::Resolve({"renamed_config", "get", "dir"});
  ASSERT_TRUE(resolved);
  EXPECT_EQ(resolved->root, "config");
  EXPECT_EQ(resolved->FullName(), "config|get");
}

TEST(SubcommandResolution, FunctionFlushIsMarkedAsWrite) {
  auto *attr = redis::CommandTable::LookupAttributesByName("function|flush");

  ASSERT_NE(attr, nullptr);
  EXPECT_TRUE(attr->InitialFlags() & redis::kCmdWrite);
}

}  // namespace
