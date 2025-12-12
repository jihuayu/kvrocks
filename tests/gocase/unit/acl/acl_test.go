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
		// Use a valid category from kvrocks: @string, @hash, @list, @set, @zset, etc.
		err := rdb.Do(ctx, "ACL", "SETUSER", "cmduser5", "on", "+@string").Err()
		require.NoError(t, err)
	})

	t.Run("Deny command category with -@category", func(t *testing.T) {
		// Use a valid category from kvrocks
		err := rdb.Do(ctx, "ACL", "SETUSER", "cmduser6", "on", "allcommands", "-@script").Err()
		require.NoError(t, err)
	})

	t.Run("Allow all categories with +@all", func(t *testing.T) {
		// kvrocks uses +@all instead of allcategories
		err := rdb.Do(ctx, "ACL", "SETUSER", "cmduser7", "on", "+@all").Err()
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

	t.Run("Read-write key permission prefixes not supported", func(t *testing.T) {
		// %R and %W prefixes are not supported in kvrocks
		err := rdb.Do(ctx, "ACL", "SETUSER", "keyuser5", "on", "%R~readonly:*").Err()
		require.Error(t, err)
		require.Contains(t, err.Error(), "not supported")
	})

	t.Run("Write-only key permission prefixes not supported", func(t *testing.T) {
		// %R and %W prefixes are not supported in kvrocks
		err := rdb.Do(ctx, "ACL", "SETUSER", "keyuser6", "on", "%W~writeonly:*").Err()
		require.Error(t, err)
		require.Contains(t, err.Error(), "not supported")
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

	t.Run("Selectors are not supported", func(t *testing.T) {
		// ACL selectors with parentheses are not supported in kvrocks
		err := rdb.Do(ctx, "ACL", "SETUSER", "seluser1", "on", "+get", "(", "+set", "~cache:*", ")").Err()
		require.Error(t, err)
		require.Contains(t, err.Error(), "not supported")
	})

	t.Run("Multiple selectors not supported", func(t *testing.T) {
		// ACL selectors are not supported in kvrocks
		err := rdb.Do(ctx, "ACL", "SETUSER", "seluser2", "on",
			"+get", "~read:*",
			"(", "+set", "+del", "~write:*", ")").Err()
		require.Error(t, err)
		require.Contains(t, err.Error(), "not supported")
	})

	t.Run("Clearselectors not supported", func(t *testing.T) {
		// clearselectors is not supported in kvrocks
		err := rdb.Do(ctx, "ACL", "SETUSER", "seluser3", "clearselectors").Err()
		require.Error(t, err)
		require.Contains(t, err.Error(), "not supported")
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
		require.True(t, ok, "Expected array result from ACL GETUSER")
		require.NotEmpty(t, fields)
	})

	t.Run("Get non-existing user returns nil", func(t *testing.T) {
		result, err := rdb.Do(ctx, "ACL", "GETUSER", "nonexistent").Result()
		// Kvrocks returns nil for non-existent users, which causes a redis: nil error
		if err != nil {
			require.Contains(t, err.Error(), "nil")
		} else {
			require.Nil(t, result)
		}
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
		// Use valid kvrocks categories: +@string, +@hash, +@list, etc.
		err := rdb.Do(ctx, "ACL", "SETUSER", "readonly",
			"on", ">readpass", "+@string", "+@hash", "~data:*", "~cache:*").Err()
		require.NoError(t, err)

		result, err := rdb.Do(ctx, "ACL", "GETUSER", "readonly").Result()
		require.NoError(t, err)
		require.NotNil(t, result)
	})

	t.Run("Admin user with all permissions except script commands", func(t *testing.T) {
		// Use valid kvrocks categories
		err := rdb.Do(ctx, "ACL", "SETUSER", "admin",
			"on", ">adminpass", "allkeys", "allchannels", "allcommands", "-@script").Err()
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
			// Use +@all instead of @all for kvrocks
			err := rdb.Do(ctx, "ACL", "SETUSER", username,
				"on", fmt.Sprintf(">%s_pass", tenant),
				pattern, "+@all").Err()
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

// TestACLGetUserFormat tests the format of ACL GETUSER output
func TestACLGetUserFormat(t *testing.T) {
	srv := startACLPreviewServer(t)
	defer srv.Close()

	ctx := context.Background()
	rdb := srv.NewClient()
	defer func() { require.NoError(t, rdb.Close()) }()

	t.Run("GETUSER returns correct format for simple user", func(t *testing.T) {
		require.NoError(t, rdb.Do(ctx, "ACL", "SETUSER", "formatuser1", "on").Err())

		result, err := rdb.Do(ctx, "ACL", "GETUSER", "formatuser1").Result()
		require.NoError(t, err)
		require.NotNil(t, result)

		// Result should be a map/array with expected fields
		fields, ok := result.([]interface{})
		require.True(t, ok)
		require.NotEmpty(t, fields)

		// Convert to map for easier verification
		fieldMap := make(map[string]interface{})
		for i := 0; i < len(fields); i += 2 {
			key, ok := fields[i].(string)
			require.True(t, ok)
			fieldMap[key] = fields[i+1]
		}

		// Verify expected fields exist
		require.Contains(t, fieldMap, "flags")
		require.Contains(t, fieldMap, "passwords")
		require.Contains(t, fieldMap, "commands")
		require.Contains(t, fieldMap, "keys")
		require.Contains(t, fieldMap, "channels")
		require.Contains(t, fieldMap, "selectors")
	})

	t.Run("GETUSER flags field contains on/off status", func(t *testing.T) {
		require.NoError(t, rdb.Do(ctx, "ACL", "SETUSER", "formatuser2", "on").Err())

		result, err := rdb.Do(ctx, "ACL", "GETUSER", "formatuser2").Result()
		require.NoError(t, err)

		fields := result.([]interface{})
		fieldMap := make(map[string]interface{})
		for i := 0; i < len(fields); i += 2 {
			fieldMap[fields[i].(string)] = fields[i+1]
		}

		flags := fieldMap["flags"].([]interface{})
		flagStrings := make([]string, len(flags))
		for i, f := range flags {
			flagStrings[i] = f.(string)
		}
		require.Contains(t, flagStrings, "on")
	})

	t.Run("GETUSER flags field shows off for disabled user", func(t *testing.T) {
		require.NoError(t, rdb.Do(ctx, "ACL", "SETUSER", "formatuser3", "off").Err())

		result, err := rdb.Do(ctx, "ACL", "GETUSER", "formatuser3").Result()
		require.NoError(t, err)

		fields := result.([]interface{})
		fieldMap := make(map[string]interface{})
		for i := 0; i < len(fields); i += 2 {
			fieldMap[fields[i].(string)] = fields[i+1]
		}

		flags := fieldMap["flags"].([]interface{})
		flagStrings := make([]string, len(flags))
		for i, f := range flags {
			flagStrings[i] = f.(string)
		}
		require.Contains(t, flagStrings, "off")
	})

	t.Run("GETUSER shows nopass flag when set", func(t *testing.T) {
		require.NoError(t, rdb.Do(ctx, "ACL", "SETUSER", "formatuser4", "on", "nopass").Err())

		result, err := rdb.Do(ctx, "ACL", "GETUSER", "formatuser4").Result()
		require.NoError(t, err)

		fields := result.([]interface{})
		fieldMap := make(map[string]interface{})
		for i := 0; i < len(fields); i += 2 {
			fieldMap[fields[i].(string)] = fields[i+1]
		}

		flags := fieldMap["flags"].([]interface{})
		flagStrings := make([]string, len(flags))
		for i, f := range flags {
			flagStrings[i] = f.(string)
		}
		require.Contains(t, flagStrings, "nopass")
	})

	t.Run("GETUSER passwords field is empty for nopass user", func(t *testing.T) {
		require.NoError(t, rdb.Do(ctx, "ACL", "SETUSER", "formatuser5", "on", "nopass").Err())

		result, err := rdb.Do(ctx, "ACL", "GETUSER", "formatuser5").Result()
		require.NoError(t, err)

		fields := result.([]interface{})
		fieldMap := make(map[string]interface{})
		for i := 0; i < len(fields); i += 2 {
			fieldMap[fields[i].(string)] = fields[i+1]
		}

		passwords := fieldMap["passwords"].([]interface{})
		require.Empty(t, passwords)
	})

	t.Run("GETUSER shows key patterns correctly", func(t *testing.T) {
		require.NoError(t, rdb.Do(ctx, "ACL", "SETUSER", "formatuser6", "on", "~user:*", "~cache:*").Err())

		result, err := rdb.Do(ctx, "ACL", "GETUSER", "formatuser6").Result()
		require.NoError(t, err)

		fields := result.([]interface{})
		fieldMap := make(map[string]interface{})
		for i := 0; i < len(fields); i += 2 {
			fieldMap[fields[i].(string)] = fields[i+1]
		}

		keys := fieldMap["keys"].([]interface{})
		require.GreaterOrEqual(t, len(keys), 2)
	})

	t.Run("GETUSER shows channel patterns correctly", func(t *testing.T) {
		require.NoError(t, rdb.Do(ctx, "ACL", "SETUSER", "formatuser7", "on", "&news:*", "&events:*").Err())

		result, err := rdb.Do(ctx, "ACL", "GETUSER", "formatuser7").Result()
		require.NoError(t, err)

		fields := result.([]interface{})
		fieldMap := make(map[string]interface{})
		for i := 0; i < len(fields); i += 2 {
			fieldMap[fields[i].(string)] = fields[i+1]
		}

		channels := fieldMap["channels"].([]interface{})
		require.GreaterOrEqual(t, len(channels), 2)
	})

	t.Run("GETUSER shows allkeys flag correctly", func(t *testing.T) {
		require.NoError(t, rdb.Do(ctx, "ACL", "SETUSER", "formatuser8", "on", "allkeys").Err())

		result, err := rdb.Do(ctx, "ACL", "GETUSER", "formatuser8").Result()
		require.NoError(t, err)

		fields := result.([]interface{})
		fieldMap := make(map[string]interface{})
		for i := 0; i < len(fields); i += 2 {
			fieldMap[fields[i].(string)] = fields[i+1]
		}

		flags := fieldMap["flags"].([]interface{})
		flagStrings := make([]string, len(flags))
		for i, f := range flags {
			flagStrings[i] = f.(string)
		}
		require.Contains(t, flagStrings, "allkeys")
	})

	t.Run("GETUSER shows allchannels flag correctly", func(t *testing.T) {
		require.NoError(t, rdb.Do(ctx, "ACL", "SETUSER", "formatuser9", "on", "allchannels").Err())

		result, err := rdb.Do(ctx, "ACL", "GETUSER", "formatuser9").Result()
		require.NoError(t, err)

		fields := result.([]interface{})
		fieldMap := make(map[string]interface{})
		for i := 0; i < len(fields); i += 2 {
			fieldMap[fields[i].(string)] = fields[i+1]
		}

		flags := fieldMap["flags"].([]interface{})
		flagStrings := make([]string, len(flags))
		for i, f := range flags {
			flagStrings[i] = f.(string)
		}
		require.Contains(t, flagStrings, "allchannels")
	})

	t.Run("GETUSER shows selectors field even without selectors", func(t *testing.T) {
		// Since kvrocks doesn't support selectors with parentheses, just test basic user
		require.NoError(t, rdb.Do(ctx, "ACL", "SETUSER", "formatuser10", "on",
			"+get", "+set", "~read:*").Err())

		result, err := rdb.Do(ctx, "ACL", "GETUSER", "formatuser10").Result()
		require.NoError(t, err)

		fields := result.([]interface{})
		fieldMap := make(map[string]interface{})
		for i := 0; i < len(fields); i += 2 {
			fieldMap[fields[i].(string)] = fields[i+1]
		}

		// The selectors field should still be present
		require.Contains(t, fieldMap, "selectors")
	})

	t.Run("GETUSER shows namespace field", func(t *testing.T) {
		require.NoError(t, rdb.Do(ctx, "ACL", "SETUSER", "formatuser11", "on").Err())

		result, err := rdb.Do(ctx, "ACL", "GETUSER", "formatuser11").Result()
		require.NoError(t, err)

		fields := result.([]interface{})
		fieldMap := make(map[string]interface{})
		for i := 0; i < len(fields); i += 2 {
			fieldMap[fields[i].(string)] = fields[i+1]
		}

		require.Contains(t, fieldMap, "namespace")
	})
}

// TestACLUsersFormat tests the format of ACL USERS output
func TestACLUsersFormat(t *testing.T) {
	srv := startACLPreviewServer(t)
	defer srv.Close()

	ctx := context.Background()
	rdb := srv.NewClient()
	defer func() { require.NoError(t, rdb.Close()) }()

	t.Run("USERS returns array of strings", func(t *testing.T) {
		require.NoError(t, rdb.Do(ctx, "ACL", "SETUSER", "listuser1", "on").Err())
		require.NoError(t, rdb.Do(ctx, "ACL", "SETUSER", "listuser2", "on").Err())

		result, err := rdb.Do(ctx, "ACL", "USERS").Result()
		require.NoError(t, err)

		users, ok := result.([]interface{})
		require.True(t, ok)
		require.GreaterOrEqual(t, len(users), 2)

		// All elements should be strings
		for _, u := range users {
			_, ok := u.(string)
			require.True(t, ok, "Expected string type for username")
		}
	})

	t.Run("USERS includes default user", func(t *testing.T) {
		result, err := rdb.Do(ctx, "ACL", "USERS").Result()
		require.NoError(t, err)

		users := result.([]interface{})
		usernames := make([]string, len(users))
		for i, u := range users {
			usernames[i] = u.(string)
		}
		require.Contains(t, usernames, "default")
	})
}

// TestACLWhoamiFormat tests the format of ACL WHOAMI output
func TestACLWhoamiFormat(t *testing.T) {
	srv := startACLPreviewServer(t)
	defer srv.Close()

	ctx := context.Background()
	rdb := srv.NewClient()
	defer func() { require.NoError(t, rdb.Close()) }()

	t.Run("WHOAMI returns bulk string", func(t *testing.T) {
		result, err := rdb.Do(ctx, "ACL", "WHOAMI").Result()
		require.NoError(t, err)

		username, ok := result.(string)
		require.True(t, ok, "Expected string type for WHOAMI result")
		require.NotEmpty(t, username)
	})

	t.Run("WHOAMI returns default for unauthenticated connection", func(t *testing.T) {
		result, err := rdb.Do(ctx, "ACL", "WHOAMI").Result()
		require.NoError(t, err)
		require.Equal(t, "default", result)
	})
}

// TestACLSetUserFormat tests the format of ACL SETUSER responses
func TestACLSetUserFormat(t *testing.T) {
	srv := startACLPreviewServer(t)
	defer srv.Close()

	ctx := context.Background()
	rdb := srv.NewClient()
	defer func() { require.NoError(t, rdb.Close()) }()

	t.Run("SETUSER returns OK on success", func(t *testing.T) {
		result, err := rdb.Do(ctx, "ACL", "SETUSER", "setformatuser1", "on").Result()
		require.NoError(t, err)
		require.Equal(t, "OK", result)
	})

	t.Run("SETUSER returns OK for multiple modifiers", func(t *testing.T) {
		result, err := rdb.Do(ctx, "ACL", "SETUSER", "setformatuser2",
			"on", ">password", "allkeys", "+get", "+set").Result()
		require.NoError(t, err)
		require.Equal(t, "OK", result)
	})

	t.Run("SETUSER returns error for invalid modifier", func(t *testing.T) {
		err := rdb.Do(ctx, "ACL", "SETUSER", "setformatuser3", "invalidmodifier").Err()
		require.Error(t, err)
	})

	t.Run("SETUSER returns error for empty key pattern", func(t *testing.T) {
		err := rdb.Do(ctx, "ACL", "SETUSER", "setformatuser4", "~").Err()
		require.Error(t, err)
	})

	t.Run("SETUSER returns error for empty channel pattern", func(t *testing.T) {
		err := rdb.Do(ctx, "ACL", "SETUSER", "setformatuser5", "&").Err()
		require.Error(t, err)
	})

	t.Run("SETUSER returns error for incomplete command modifier", func(t *testing.T) {
		err := rdb.Do(ctx, "ACL", "SETUSER", "setformatuser6", "+").Err()
		require.Error(t, err)
	})

	t.Run("SETUSER returns error for incomplete category modifier", func(t *testing.T) {
		err := rdb.Do(ctx, "ACL", "SETUSER", "setformatuser7", "+@").Err()
		require.Error(t, err)
	})
}

// TestACLPreviewDisabled tests that ACL commands fail when preview is disabled
func TestACLPreviewDisabled(t *testing.T) {
	// Start server without ACL preview enabled
	srv := util.StartServer(t, map[string]string{})
	defer srv.Close()

	ctx := context.Background()
	rdb := srv.NewClient()
	defer func() { require.NoError(t, rdb.Close()) }()

	t.Run("ACL WHOAMI fails when preview disabled", func(t *testing.T) {
		err := rdb.Do(ctx, "ACL", "WHOAMI").Err()
		require.Error(t, err)
		require.Contains(t, err.Error(), "preview")
	})

	t.Run("ACL USERS fails when preview disabled", func(t *testing.T) {
		err := rdb.Do(ctx, "ACL", "USERS").Err()
		require.Error(t, err)
		require.Contains(t, err.Error(), "preview")
	})

	t.Run("ACL SETUSER fails when preview disabled", func(t *testing.T) {
		err := rdb.Do(ctx, "ACL", "SETUSER", "testuser", "on").Err()
		require.Error(t, err)
		require.Contains(t, err.Error(), "preview")
	})

	t.Run("ACL GETUSER fails when preview disabled", func(t *testing.T) {
		err := rdb.Do(ctx, "ACL", "GETUSER", "testuser").Err()
		require.Error(t, err)
		require.Contains(t, err.Error(), "preview")
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
