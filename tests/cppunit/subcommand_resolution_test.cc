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
  auto dispatched_command = redis::CommandTable::Resolve({"ping"});

  ASSERT_TRUE(dispatched_command);
  EXPECT_EQ(dispatched_command->root, "ping");
  EXPECT_FALSE(dispatched_command->HasSubcommand());
  EXPECT_EQ(dispatched_command->FullName(), "ping");
}

TEST(SubcommandResolution, ResolveRegisteredNamespaceSubcommand) {
  auto dispatched_command = redis::CommandTable::Resolve({"namespace", "add", "ns-1", "token-1"});

  ASSERT_TRUE(dispatched_command);
  EXPECT_EQ(dispatched_command->root, "namespace");
  ASSERT_TRUE(dispatched_command->HasSubcommand());
  EXPECT_EQ(*dispatched_command->sub, "add");
  EXPECT_EQ(dispatched_command->FullName(), "namespace|add");
  EXPECT_EQ(dispatched_command->attributes->arity, 4);
}

TEST(SubcommandResolution, UnknownNamespaceSubcommandFallsBackToRootCommand) {
  auto dispatched_command = redis::CommandTable::Resolve({"namespace", "missing"});

  ASSERT_TRUE(dispatched_command);
  EXPECT_EQ(dispatched_command->root, "namespace");
  EXPECT_FALSE(dispatched_command->HasSubcommand());
  EXPECT_EQ(dispatched_command->FullName(), "namespace");
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

  auto dispatched_command = redis::CommandTable::Resolve({"renamed_namespace", "add", "ns-1", "token-1"});
  ASSERT_TRUE(dispatched_command);
  EXPECT_EQ(dispatched_command->root, "namespace");
  EXPECT_EQ(dispatched_command->FullName(), "namespace|add");
}

TEST(SubcommandResolution, DuplicateParentCommandRegistrationsAreRejected) {
  EXPECT_DEATH(
      {
        redis::RegisterToCommandTable command_registration(
            redis::CommandCategory::Server, {redis::MakeCmdAttr<OverrideRootFallbackCommand>(
                                                "__test_conflict_parent__", 1, "read-only", redis::NO_KEY)});
        redis::RegisterToSubcommandTable subcommand_registration(
            redis::CommandCategory::Server,
            redis::MakeCmdAttr<OverrideRootFallbackCommand>("__test_conflict_parent__", -2, "read-only", redis::NO_KEY),
            redis::MakeArgIndexSubcommandResolver(1), redis::DefaultSubcommandFallback(),
            {redis::MakeSubCmdAttr<OverrideSubcommandCommand>("__test_conflict_parent__", "ok", 2, "read-only",
                                                              redis::NO_KEY)});
        (void)command_registration;
        (void)subcommand_registration;
      },
      "duplicated command registration");
}

TEST(SubcommandResolution, DuplicateCommandRegistrationsAreRejectedAfterSubcommandRegistration) {
  EXPECT_DEATH(
      {
        redis::RegisterToSubcommandTable subcommand_registration(
            redis::CommandCategory::Server,
            redis::MakeCmdAttr<OverrideRootFallbackCommand>("__test_conflict_parent_reverse__", -2, "read-only",
                                                            redis::NO_KEY),
            redis::MakeArgIndexSubcommandResolver(1), redis::DefaultSubcommandFallback(),
            {redis::MakeSubCmdAttr<OverrideSubcommandCommand>("__test_conflict_parent_reverse__", "ok", 2, "read-only",
                                                              redis::NO_KEY)});
        redis::RegisterToCommandTable command_registration(
            redis::CommandCategory::Server, {redis::MakeCmdAttr<OverrideRootFallbackCommand>(
                                                "__test_conflict_parent_reverse__", 1, "read-only", redis::NO_KEY)});
        (void)subcommand_registration;
        (void)command_registration;
      },
      "duplicated command registration");
}

TEST(SubcommandResolution, UnknownSubcommandCanUseFallbackOverride) {
  static const auto *subcommand_registration = new redis::RegisterToSubcommandTable(
      redis::CommandCategory::Server,
      redis::MakeCmdAttr<OverrideRootFallbackCommand>("__test_override_subcommand__", -2, "read-only", redis::NO_KEY),
      redis::MakeArgIndexSubcommandResolver(1),
      redis::MakeSubcommandFallbackAttr<OverrideFallbackCommand>("__test_override_subcommand__", -2, "read-only",
                                                                 redis::NO_KEY),
      {redis::MakeSubCmdAttr<OverrideSubcommandCommand>("__test_override_subcommand__", "ok", 2, "read-only",
                                                        redis::NO_KEY)});
  (void)subcommand_registration;

  auto dispatched_command = redis::CommandTable::Resolve({"__test_override_subcommand__", "missing"});

  ASSERT_TRUE(dispatched_command);
  EXPECT_EQ(dispatched_command->root, "__test_override_subcommand__");
  EXPECT_FALSE(dispatched_command->HasSubcommand());

  auto fallback_cmd = dispatched_command->attributes->factory();
  EXPECT_NE(dynamic_cast<OverrideFallbackCommand *>(fallback_cmd.get()), nullptr);
}

}  // namespace
