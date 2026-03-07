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

#include <gtest/gtest.h>

#include "commands/commander.h"

namespace {

struct CommandTableResetGuard {
  ~CommandTableResetGuard() { redis::CommandTable::Reset(); }
};

struct OverrideRootFallbackCommand : redis::Commander {};
struct OverrideFallbackCommand : redis::Commander {};
struct OverrideSubcommandCommand : redis::Commander {};

TEST(SubcommandResolution, ResolveRootCommandWithoutSubcommand) {
  auto resolved = redis::CommandTable::Resolve({"ping"});

  ASSERT_TRUE(resolved);
  EXPECT_EQ(resolved->root, "ping");
  EXPECT_FALSE(resolved->HasSubcommand());
  EXPECT_EQ(resolved->FullName(), "ping");
}

TEST(SubcommandResolution, ResolveRegisteredNamespaceSubcommand) {
  auto resolved = redis::CommandTable::Resolve({"namespace", "add", "ns-1", "token-1"});

  ASSERT_TRUE(resolved);
  EXPECT_EQ(resolved->root, "namespace");
  ASSERT_TRUE(resolved->HasSubcommand());
  EXPECT_EQ(*resolved->sub, "add");
  EXPECT_EQ(resolved->FullName(), "namespace|add");
  EXPECT_EQ(resolved->attributes->arity, 4);
}

TEST(SubcommandResolution, UnknownNamespaceSubcommandFallsBackToRootCommand) {
  auto resolved = redis::CommandTable::Resolve({"namespace", "missing"});

  ASSERT_TRUE(resolved);
  EXPECT_EQ(resolved->root, "namespace");
  EXPECT_FALSE(resolved->HasSubcommand());
  EXPECT_EQ(resolved->FullName(), "namespace");
}

TEST(SubcommandResolution, LookupCanonicalNamespaceSubcommandAttributes) {
  auto *attr = redis::CommandTable::LookupAttributesByName("namespace|current");

  ASSERT_NE(attr, nullptr);
  EXPECT_EQ(attr->name, "namespace|current");
  EXPECT_FALSE(attr->InitialFlags() & redis::kCmdAdmin);
}

TEST(SubcommandResolution, CanonicalLookupSurvivesCommandRename) {
  CommandTableResetGuard guard;

  auto *commands = redis::CommandTable::Get();
  auto namespace_iter = commands->find("namespace");
  ASSERT_NE(namespace_iter, commands->end());

  (*commands)["renamed_namespace"] = namespace_iter->second;
  commands->erase(namespace_iter);

  auto *attr = redis::CommandTable::LookupAttributesByName("namespace|add");
  ASSERT_NE(attr, nullptr);
  EXPECT_EQ(attr->name, "namespace|add");

  auto resolved = redis::CommandTable::Resolve({"renamed_namespace", "add", "ns-1", "token-1"});
  ASSERT_TRUE(resolved);
  EXPECT_EQ(resolved->root, "namespace");
  EXPECT_EQ(resolved->FullName(), "namespace|add");
}

TEST(SubcommandResolution, UnknownSubcommandCanUseFallbackOverride) {
  static const auto *root_registration = new redis::RegisterToCommandTable(
      redis::CommandCategory::Server, {redis::MakeCmdAttr<OverrideRootFallbackCommand>(
                                          "__test_override_subcommand__", -2, "read-only", redis::NO_KEY)});
  static const auto *subcommand_registration = new redis::RegisterToSubcommandTable(
      redis::CommandCategory::Server, "__test_override_subcommand__", redis::MakeArgIndexSubcommandResolver(1),
      redis::MakeSubcommandFallbackAttr<OverrideFallbackCommand>("__test_override_subcommand__", -2, "read-only",
                                                                 redis::NO_KEY),
      {redis::MakeSubCmdAttr<OverrideSubcommandCommand>("__test_override_subcommand__", "ok", 2, "read-only",
                                                        redis::NO_KEY)});
  (void)root_registration;
  (void)subcommand_registration;

  auto resolved = redis::CommandTable::Resolve({"__test_override_subcommand__", "missing"});

  ASSERT_TRUE(resolved);
  EXPECT_EQ(resolved->root, "__test_override_subcommand__");
  EXPECT_FALSE(resolved->HasSubcommand());

  auto fallback_cmd = resolved->attributes->factory();
  EXPECT_NE(dynamic_cast<OverrideFallbackCommand *>(fallback_cmd.get()), nullptr);
}

}  // namespace
