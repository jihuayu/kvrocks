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
 */

package acl

import (
	"context"
	"fmt"
	"testing"

	"github.com/apache/kvrocks/tests/gocase/util"
	"github.com/stretchr/testify/require"
)

func startACLPreviewServer(t *testing.T) *util.KvrocksServer {
	t.Helper()
	return util.StartServer(t, map[string]string{
		"acl-preview-enabled": "yes",
	})
}

// TestACLWhoami tests the ACL WHOAMI command
func TestACLWhoami(t *testing.T) {
	srv := startACLPreviewServer(t)
	defer srv.Close()

	ctx := context.Background()
	rdb := srv.NewClient()
	defer func() { require.NoError(t, rdb.Close()) }()

	t.Run("ACL WHOAMI returns default user", func(t *testing.T) {
		result, err := rdb.Do(ctx, "ACL", "WHOAMI").Result()
		require.NoError(t, err)
		require.Equal(t, "default", result)
	})
}

// TestACLUsers tests the ACL USERS command
func TestACLUsers(t *testing.T) {
	srv := startACLPreviewServer(t)
	defer srv.Close()

	ctx := context.Background()
	rdb := srv.NewClient()
	defer func() { require.NoError(t, rdb.Close()) }()

	t.Run("ACL USERS returns list of usernames", func(t *testing.T) {
		// Create some test users
		require.NoError(t, rdb.Do(ctx, "ACL", "SETUSER", "alice", "on").Err())
		require.NoError(t, rdb.Do(ctx, "ACL", "SETUSER", "bob", "on").Err())

		result, err := rdb.Do(ctx, "ACL", "USERS").Result()
		require.NoError(t, err)

		users, ok := result.([]interface{})
		require.True(t, ok)
		require.GreaterOrEqual(t, len(users), 2)

		// Check that our users are in the list
		usernames := make([]string, len(users))
		for i, u := range users {
			usernames[i] = u.(string)
		}
		require.Contains(t, usernames, "alice")
		require.Contains(t, usernames, "bob")
	})
}

// TestACLSetUser tests the ACL SETUSER command with various options
func TestACLSetUser(t *testing.T) {
	srv := startACLPreviewServer(t)
	defer srv.Close()

	ctx := context.Background()
	rdb := srv.NewClient()
	defer func() { require.NoError(t, rdb.Close()) }()

	t.Run("Create user with on flag", func(t *testing.T) {
		err := rdb.Do(ctx, "ACL", "SETUSER", "testuser1", "on").Err()
		require.NoError(t, err)

		// Verify user was created
		result, err := rdb.Do(ctx, "ACL", "GETUSER", "testuser1").Result()
		require.NoError(t, err)
		require.NotNil(t, result)
	})

	t.Run("Create user with off flag", func(t *testing.T) {
		err := rdb.Do(ctx, "ACL", "SETUSER", "testuser2", "off").Err()
		require.NoError(t, err)

		result, err := rdb.Do(ctx, "ACL", "GETUSER", "testuser2").Result()
		require.NoError(t, err)
		require.NotNil(t, result)
	})

	t.Run("Set user with password", func(t *testing.T) {
		err := rdb.Do(ctx, "ACL", "SETUSER", "testuser3", "on", ">mypassword").Err()
		require.NoError(t, err)

		result, err := rdb.Do(ctx, "ACL", "GETUSER", "testuser3").Result()
		require.NoError(t, err)
		require.NotNil(t, result)
	})

	t.Run("Set user with multiple passwords", func(t *testing.T) {
		err := rdb.Do(ctx, "ACL", "SETUSER", "testuser4", "on", ">password1", ">password2").Err()
		require.NoError(t, err)

		result, err := rdb.Do(ctx, "ACL", "GETUSER", "testuser4").Result()
		require.NoError(t, err)
		require.NotNil(t, result)
	})

	t.Run("Set user with nopass", func(t *testing.T) {
		err := rdb.Do(ctx, "ACL", "SETUSER", "testuser5", "on", "nopass").Err()
		require.NoError(t, err)

		result, err := rdb.Do(ctx, "ACL", "GETUSER", "testuser5").Result()
		require.NoError(t, err)
		require.NotNil(t, result)
	})

	t.Run("Reset password with resetpass", func(t *testing.T) {
		// First set with password
		require.NoError(t, rdb.Do(ctx, "ACL", "SETUSER", "testuser6", "on", ">oldpass").Err())
		// Then reset
		err := rdb.Do(ctx, "ACL", "SETUSER", "testuser6", "resetpass").Err()
		require.NoError(t, err)
	})

	t.Run("Remove specific password", func(t *testing.T) {
		require.NoError(t, rdb.Do(ctx, "ACL", "SETUSER", "testuser7", "on", ">pass1", ">pass2").Err())
		err := rdb.Do(ctx, "ACL", "SETUSER", "testuser7", "<pass1").Err()
		require.NoError(t, err)
	})
}

// TestACLSetUserCommands tests ACL SETUSER with command permissions
func TestACLSetUserCommands(t *testing.T) {
	srv := startACLPreviewServer(t)
	defer srv.Close()

	ctx := context.Background()
	rdb := srv.NewClient()
	defer func() { require.NoError(t, rdb.Close()) }()

	t.Run("Allow all commands with allcommands", func(t *testing.T) {
		err := rdb.Do(ctx, "ACL", "SETUSER", "cmduser1", "on", "allcommands").Err()
		require.NoError(t, err)
	})

	t.Run("Deny all commands with nocommands", func(t *testing.T) {
		err := rdb.Do(ctx, "ACL", "SETUSER", "cmduser2", "on", "nocommands").Err()
		require.NoError(t, err)
	})

	t.Run("Allow specific command with +command", func(t *testing.T) {
		err := rdb.Do(ctx, "ACL", "SETUSER", "cmduser3", "on", "+get", "+set").Err()
		require.NoError(t, err)
	})

	t.Run("Deny specific command with -command", func(t *testing.T) {
		err := rdb.Do(ctx, "ACL", "SETUSER", "cmduser4", "on", "allcommands", "-flushdb", "-flushall").Err()
		require.NoError(t, err)
	})

	t.Run("Allow command category with @category", func(t *testing.T) {
		err := rdb.Do(ctx, "ACL", "SETUSER", "cmduser5", "on", "@read").Err()
		require.NoError(t, err)
	})

	t.Run("Deny command category with -@category", func(t *testing.T) {
		err := rdb.Do(ctx, "ACL", "SETUSER", "cmduser6", "on", "allcommands", "-@dangerous").Err()
		require.NoError(t, err)
	})

	t.Run("Allow all categories with allcategories", func(t *testing.T) {
		err := rdb.Do(ctx, "ACL", "SETUSER", "cmduser7", "on", "allcategories").Err()
		require.NoError(t, err)
	})
}

// TestACLSetUserKeys tests ACL SETUSER with key patterns
func TestACLSetUserKeys(t *testing.T) {
	srv := startACLPreviewServer(t)
	defer srv.Close()

	ctx := context.Background()
	rdb := srv.NewClient()
	defer func() { require.NoError(t, rdb.Close()) }()

	t.Run("Allow all keys with allkeys", func(t *testing.T) {
		err := rdb.Do(ctx, "ACL", "SETUSER", "keyuser1", "on", "allkeys").Err()
		require.NoError(t, err)
	})

	t.Run("Reset keys with resetkeys", func(t *testing.T) {
		err := rdb.Do(ctx, "ACL", "SETUSER", "keyuser2", "on", "allkeys", "resetkeys").Err()
		require.NoError(t, err)
	})

	t.Run("Add key pattern with ~pattern", func(t *testing.T) {
		err := rdb.Do(ctx, "ACL", "SETUSER", "keyuser3", "on", "~user:*").Err()
		require.NoError(t, err)
	})

	t.Run("Add multiple key patterns", func(t *testing.T) {
		err := rdb.Do(ctx, "ACL", "SETUSER", "keyuser4", "on", "~user:*", "~session:*", "~cache:*").Err()
		require.NoError(t, err)
	})

	t.Run("Add read-only key pattern with %R~pattern", func(t *testing.T) {
		err := rdb.Do(ctx, "ACL", "SETUSER", "keyuser5", "on", "%R~readonly:*").Err()
		require.NoError(t, err)
	})

	t.Run("Add write-only key pattern with %W~pattern", func(t *testing.T) {
		err := rdb.Do(ctx, "ACL", "SETUSER", "keyuser6", "on", "%W~writeonly:*").Err()
		require.NoError(t, err)
	})
}

// TestACLSetUserChannels tests ACL SETUSER with channel patterns
func TestACLSetUserChannels(t *testing.T) {
	srv := startACLPreviewServer(t)
	defer srv.Close()

	ctx := context.Background()
	rdb := srv.NewClient()
	defer func() { require.NoError(t, rdb.Close()) }()

	t.Run("Allow all channels with allchannels", func(t *testing.T) {
		err := rdb.Do(ctx, "ACL", "SETUSER", "chanuser1", "on", "allchannels").Err()
		require.NoError(t, err)
	})

	t.Run("Reset channels with resetchannels", func(t *testing.T) {
		err := rdb.Do(ctx, "ACL", "SETUSER", "chanuser2", "on", "allchannels", "resetchannels").Err()
		require.NoError(t, err)
	})

	t.Run("Add channel pattern with &pattern", func(t *testing.T) {
		err := rdb.Do(ctx, "ACL", "SETUSER", "chanuser3", "on", "&news:*").Err()
		require.NoError(t, err)
	})

	t.Run("Add multiple channel patterns", func(t *testing.T) {
		err := rdb.Do(ctx, "ACL", "SETUSER", "chanuser4", "on", "&news:*", "&events:*", "&alerts:*").Err()
		require.NoError(t, err)
	})
}

// TestACLSetUserSelectors tests ACL SETUSER with multiple selectors
func TestACLSetUserSelectors(t *testing.T) {
	srv := startACLPreviewServer(t)
	defer srv.Close()

	ctx := context.Background()
	rdb := srv.NewClient()
	defer func() { require.NoError(t, rdb.Close()) }()

	t.Run("Add selector with parentheses", func(t *testing.T) {
		err := rdb.Do(ctx, "ACL", "SETUSER", "seluser1", "on", "+get", "(", "+set", "~cache:*", ")").Err()
		require.NoError(t, err)
	})

	t.Run("Multiple selectors", func(t *testing.T) {
		err := rdb.Do(ctx, "ACL", "SETUSER", "seluser2", "on",
			"+get", "~read:*",
			"(", "+set", "+del", "~write:*", ")",
			"(", "@admin", "~admin:*", ")").Err()
		require.NoError(t, err)
	})

	t.Run("Clear selectors with clearselectors", func(t *testing.T) {
		require.NoError(t, rdb.Do(ctx, "ACL", "SETUSER", "seluser3", "on", "+get", "(", "+set", ")").Err())
		err := rdb.Do(ctx, "ACL", "SETUSER", "seluser3", "clearselectors").Err()
		require.NoError(t, err)
	})
}

// TestACLSetUserReset tests ACL SETUSER with reset command
func TestACLSetUserReset(t *testing.T) {
	srv := startACLPreviewServer(t)
	defer srv.Close()

	ctx := context.Background()
	rdb := srv.NewClient()
	defer func() { require.NoError(t, rdb.Close()) }()

	t.Run("Reset user to default state", func(t *testing.T) {
		// Create user with various settings
		require.NoError(t, rdb.Do(ctx, "ACL", "SETUSER", "resetuser", "on", ">password", "allkeys", "allcommands").Err())

		// Reset the user
		err := rdb.Do(ctx, "ACL", "SETUSER", "resetuser", "reset").Err()
		require.NoError(t, err)

		// Verify user still exists but is reset
		result, err := rdb.Do(ctx, "ACL", "GETUSER", "resetuser").Result()
		require.NoError(t, err)
		require.NotNil(t, result)
	})
}

// TestACLGetUser tests the ACL GETUSER command
func TestACLGetUser(t *testing.T) {
	srv := startACLPreviewServer(t)
	defer srv.Close()

	ctx := context.Background()
	rdb := srv.NewClient()
	defer func() { require.NoError(t, rdb.Close()) }()

	t.Run("Get existing user", func(t *testing.T) {
		require.NoError(t, rdb.Do(ctx, "ACL", "SETUSER", "getuser1", "on", "+get", "+set", "~key:*").Err())

		result, err := rdb.Do(ctx, "ACL", "GETUSER", "getuser1").Result()
		require.NoError(t, err)
		require.NotNil(t, result)

		// Result should be an array of field-value pairs
		fields, ok := result.([]interface{})
		require.True(t, ok)
		require.NotEmpty(t, fields)
	})

	t.Run("Get non-existing user returns nil", func(t *testing.T) {
		result, err := rdb.Do(ctx, "ACL", "GETUSER", "nonexistent").Result()
		require.NoError(t, err)
		require.Nil(t, result)
	})

	t.Run("Get user with detailed information", func(t *testing.T) {
		require.NoError(t, rdb.Do(ctx, "ACL", "SETUSER", "detailed",
			"on", ">password", "allkeys", "+get", "+set", "+del", "&channel:*").Err())

		result, err := rdb.Do(ctx, "ACL", "GETUSER", "detailed").Result()
		require.NoError(t, err)
		require.NotNil(t, result)
	})
}

// TestACLComplexScenarios tests complex ACL scenarios
func TestACLComplexScenarios(t *testing.T) {
	srv := startACLPreviewServer(t)
	defer srv.Close()

	ctx := context.Background()
	rdb := srv.NewClient()
	defer func() { require.NoError(t, rdb.Close()) }()

	t.Run("Read-only user with specific key patterns", func(t *testing.T) {
		err := rdb.Do(ctx, "ACL", "SETUSER", "readonly",
			"on", ">readpass", "@read", "~data:*", "~cache:*").Err()
		require.NoError(t, err)

		result, err := rdb.Do(ctx, "ACL", "GETUSER", "readonly").Result()
		require.NoError(t, err)
		require.NotNil(t, result)
	})

	t.Run("Admin user with all permissions except dangerous commands", func(t *testing.T) {
		err := rdb.Do(ctx, "ACL", "SETUSER", "admin",
			"on", ">adminpass", "allkeys", "allchannels", "allcommands", "-@dangerous").Err()
		require.NoError(t, err)

		result, err := rdb.Do(ctx, "ACL", "GETUSER", "admin").Result()
		require.NoError(t, err)
		require.NotNil(t, result)
	})

	t.Run("Application user with specific permissions", func(t *testing.T) {
		err := rdb.Do(ctx, "ACL", "SETUSER", "appuser",
			"on", ">apppass",
			"~user:*", "~session:*", "~cache:*",
			"+get", "+set", "+del", "+expire", "+ttl",
			"&events:*").Err()
		require.NoError(t, err)

		result, err := rdb.Do(ctx, "ACL", "GETUSER", "appuser").Result()
		require.NoError(t, err)
		require.NotNil(t, result)
	})

	t.Run("Multi-tenant user with namespace isolation", func(t *testing.T) {
		tenants := []string{"tenant1", "tenant2", "tenant3"}
		for _, tenant := range tenants {
			username := fmt.Sprintf("user_%s", tenant)
			pattern := fmt.Sprintf("~%s:*", tenant)
			err := rdb.Do(ctx, "ACL", "SETUSER", username,
				"on", fmt.Sprintf(">%s_pass", tenant),
				pattern, "@all").Err()
			require.NoError(t, err)
		}

		// Verify all tenant users were created
		users, err := rdb.Do(ctx, "ACL", "USERS").Result()
		require.NoError(t, err)
		userList, ok := users.([]interface{})
		require.True(t, ok)

		for _, tenant := range tenants {
			username := fmt.Sprintf("user_%s", tenant)
			found := false
			for _, u := range userList {
				if u.(string) == username {
					found = true
					break
				}
			}
			require.True(t, found, "User %s should exist", username)
		}
	})
}

// TestACLErrorCases tests error handling in ACL commands
func TestACLErrorCases(t *testing.T) {
	srv := startACLPreviewServer(t)
	defer srv.Close()

	ctx := context.Background()
	rdb := srv.NewClient()
	defer func() { require.NoError(t, rdb.Close()) }()

	t.Run("ACL with no subcommand fails", func(t *testing.T) {
		err := rdb.Do(ctx, "ACL").Err()
		require.Error(t, err)
	})

	t.Run("ACL with invalid subcommand fails", func(t *testing.T) {
		err := rdb.Do(ctx, "ACL", "INVALID").Err()
		require.Error(t, err)
	})

	t.Run("ACL SETUSER with empty username fails", func(t *testing.T) {
		err := rdb.Do(ctx, "ACL", "SETUSER", "").Err()
		require.Error(t, err)
	})

	t.Run("ACL SETUSER with invalid modifier fails", func(t *testing.T) {
		err := rdb.Do(ctx, "ACL", "SETUSER", "testuser", "invalidmod").Err()
		require.Error(t, err)
	})

	t.Run("ACL GETUSER without username fails", func(t *testing.T) {
		err := rdb.Do(ctx, "ACL", "GETUSER").Err()
		require.Error(t, err)
	})

	t.Run("ACL WHOAMI with extra arguments fails", func(t *testing.T) {
		err := rdb.Do(ctx, "ACL", "WHOAMI", "extra").Err()
		require.Error(t, err)
	})

	t.Run("ACL USERS with extra arguments fails", func(t *testing.T) {
		err := rdb.Do(ctx, "ACL", "USERS", "extra").Err()
		require.Error(t, err)
	})
}

// TestACLPersistence tests that ACL changes persist across restarts
func TestACLPersistence(t *testing.T) {
	t.Skip("Skipping persistence test - requires server restart capability")
	// This test would need special handling to restart the server
	// and verify that ACL users persist
}

// TestACLReplication tests ACL replication scenarios
func TestACLReplication(t *testing.T) {
	t.Skip("Skipping replication test - requires multi-node setup")
	// This test would need a replication setup to verify
	// that ACL changes are replicated correctly
}
