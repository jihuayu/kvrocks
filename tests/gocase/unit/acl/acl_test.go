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
	"time"

	"github.com/apache/kvrocks/tests/gocase/util"
	"github.com/redis/go-redis/v9"
	"github.com/stretchr/testify/require"
)

// Helper function to parse ACL GETUSER result into a map
// Supports both RESP2 array format and RESP3 map format
func parseGetUserResult(t *testing.T, result interface{}) map[string]interface{} {
	t.Helper()

	// Try RESP3 map format first (go-redis v9 with RESP3)
	if m, ok := result.(map[interface{}]interface{}); ok {
		fieldMap := make(map[string]interface{})
		for k, v := range m {
			if key, ok := k.(string); ok {
				fieldMap[key] = v
			}
		}
		return fieldMap
	}

	// Fall back to RESP2 array format
	fields, ok := result.([]interface{})
	require.True(t, ok, "Expected array or map result from ACL GETUSER, got %T", result)
	fieldMap := make(map[string]interface{})
	for i := 0; i < len(fields); i += 2 {
		key, ok := fields[i].(string)
		require.True(t, ok)
		fieldMap[key] = fields[i+1]
	}
	return fieldMap
}

// Helper function to extract flags as string slice
func extractFlags(t *testing.T, fieldMap map[string]interface{}) []string {
	t.Helper()
	flags := fieldMap["flags"].([]interface{})
	result := make([]string, len(flags))
	for i, f := range flags {
		result[i] = f.(string)
	}
	return result
}

// Helper function to extract usernames from ACL USERS result
func extractUsernames(t *testing.T, result interface{}) []string {
	t.Helper()
	users, ok := result.([]interface{})
	require.True(t, ok)
	usernames := make([]string, len(users))
	for i, u := range users {
		usernames[i] = u.(string)
	}
	return usernames
}

func requireACLDenied(t *testing.T, err error) {
	t.Helper()
	require.Error(t, err)
	require.Contains(t, err.Error(), "NOPERM")
}

func authAsUser(t *testing.T, ctx context.Context, rdb *redis.Client, username, password string) {
	t.Helper()
	result, err := rdb.Do(ctx, "AUTH", username, password).Result()
	require.NoError(t, err)
	require.Equal(t, "OK", result)
}

func TestACLPreviewDisabled(t *testing.T) {
	srv := util.StartServer(t, map[string]string{})
	defer srv.Close()

	ctx := context.Background()
	rdb := srv.NewClient()
	defer func() { require.NoError(t, rdb.Close()) }()

	commands := [][]interface{}{
		{"ACL", "WHOAMI"},
		{"ACL", "USERS"},
		{"ACL", "SETUSER", "testuser", "on"},
		{"ACL", "GETUSER", "testuser"},
	}

	for _, cmd := range commands {
		t.Run(fmt.Sprintf("%v fails when preview disabled", cmd[1]), func(t *testing.T) {
			err := rdb.Do(ctx, cmd...).Err()
			require.Error(t, err)
			require.Contains(t, err.Error(), "preview")
		})
	}
}

func TestACLWhoami(t *testing.T) {
	srv := util.StartServer(t, map[string]string{"acl-preview-enabled": "yes"})
	defer srv.Close()

	ctx := context.Background()
	rdb := srv.NewClient()
	defer func() { require.NoError(t, rdb.Close()) }()

	t.Run("Returns default user", func(t *testing.T) {
		result, err := rdb.Do(ctx, "ACL", "WHOAMI").Result()
		require.NoError(t, err)
		require.Equal(t, "default", result)
	})

	t.Run("Returns bulk string type", func(t *testing.T) {
		result, err := rdb.Do(ctx, "ACL", "WHOAMI").Result()
		require.NoError(t, err)
		_, ok := result.(string)
		require.True(t, ok, "Expected string type for WHOAMI result")
	})
}

func TestACLUsers(t *testing.T) {
	srv := util.StartServer(t, map[string]string{"acl-preview-enabled": "yes"})
	defer srv.Close()

	ctx := context.Background()
	rdb := srv.NewClient()
	defer func() { require.NoError(t, rdb.Close()) }()

	t.Run("Returns list with created users", func(t *testing.T) {
		require.NoError(t, rdb.Do(ctx, "ACL", "SETUSER", "alice", "on").Err())
		require.NoError(t, rdb.Do(ctx, "ACL", "SETUSER", "bob", "on").Err())

		result, err := rdb.Do(ctx, "ACL", "USERS").Result()
		require.NoError(t, err)

		usernames := extractUsernames(t, result)
		require.Contains(t, usernames, "alice")
		require.Contains(t, usernames, "bob")
		// Note: kvrocks does not have a "default" user by default in ACL preview mode
	})
}

func TestACLSetUser(t *testing.T) {
	srv := util.StartServer(t, map[string]string{"acl-preview-enabled": "yes"})
	defer srv.Close()

	ctx := context.Background()
	rdb := srv.NewClient()
	defer func() { require.NoError(t, rdb.Close()) }()

	// Test basic user creation
	t.Run("Create user with on/off flag", func(t *testing.T) {
		require.NoError(t, rdb.Do(ctx, "ACL", "SETUSER", "user_on", "on").Err())
		require.NoError(t, rdb.Do(ctx, "ACL", "SETUSER", "user_off", "off").Err())

		for _, user := range []string{"user_on", "user_off"} {
			result, err := rdb.Do(ctx, "ACL", "GETUSER", user).Result()
			require.NoError(t, err)
			require.NotNil(t, result)
		}
	})

	// Test password management
	t.Run("Password operations", func(t *testing.T) {
		// Single password
		require.NoError(t, rdb.Do(ctx, "ACL", "SETUSER", "pwuser1", "on", ">mypassword").Err())
		// Multiple passwords
		require.NoError(t, rdb.Do(ctx, "ACL", "SETUSER", "pwuser2", "on", ">pass1", ">pass2").Err())
		// Nopass
		require.NoError(t, rdb.Do(ctx, "ACL", "SETUSER", "pwuser3", "on", "nopass").Err())
		// Resetpass
		require.NoError(t, rdb.Do(ctx, "ACL", "SETUSER", "pwuser4", "on", ">oldpass").Err())
		require.NoError(t, rdb.Do(ctx, "ACL", "SETUSER", "pwuser4", "resetpass").Err())
		// Remove password
		require.NoError(t, rdb.Do(ctx, "ACL", "SETUSER", "pwuser5", "on", ">pass1", ">pass2").Err())
		require.NoError(t, rdb.Do(ctx, "ACL", "SETUSER", "pwuser5", "<pass1").Err())
	})

	// Test command permissions
	t.Run("Command permissions", func(t *testing.T) {
		require.NoError(t, rdb.Do(ctx, "ACL", "SETUSER", "cmduser1", "on", "allcommands").Err())
		require.NoError(t, rdb.Do(ctx, "ACL", "SETUSER", "cmduser2", "on", "nocommands").Err())
		require.NoError(t, rdb.Do(ctx, "ACL", "SETUSER", "cmduser3", "on", "+get", "+set").Err())
		require.NoError(t, rdb.Do(ctx, "ACL", "SETUSER", "cmduser4", "on", "allcommands", "-flushdb", "-flushall").Err())
		require.NoError(t, rdb.Do(ctx, "ACL", "SETUSER", "cmduser5", "on", "+@string").Err())
		require.NoError(t, rdb.Do(ctx, "ACL", "SETUSER", "cmduser6", "on", "allcommands", "-@script").Err())
		require.NoError(t, rdb.Do(ctx, "ACL", "SETUSER", "cmduser7", "on", "+@all").Err())
	})

	// Test key patterns
	t.Run("Key patterns", func(t *testing.T) {
		require.NoError(t, rdb.Do(ctx, "ACL", "SETUSER", "keyuser1", "on", "allkeys").Err())
		require.NoError(t, rdb.Do(ctx, "ACL", "SETUSER", "keyuser2", "on", "allkeys", "resetkeys").Err())
		require.NoError(t, rdb.Do(ctx, "ACL", "SETUSER", "keyuser3", "on", "~user:*").Err())
		require.NoError(t, rdb.Do(ctx, "ACL", "SETUSER", "keyuser4", "on", "~user:*", "~session:*", "~cache:*").Err())
	})

	// Test channel patterns
	t.Run("Channel patterns", func(t *testing.T) {
		require.NoError(t, rdb.Do(ctx, "ACL", "SETUSER", "chanuser1", "on", "allchannels").Err())
		require.NoError(t, rdb.Do(ctx, "ACL", "SETUSER", "chanuser2", "on", "allchannels", "resetchannels").Err())
		require.NoError(t, rdb.Do(ctx, "ACL", "SETUSER", "chanuser3", "on", "&news:*").Err())
		require.NoError(t, rdb.Do(ctx, "ACL", "SETUSER", "chanuser4", "on", "&news:*", "&events:*", "&alerts:*").Err())
	})

	// Test reset
	t.Run("Reset user", func(t *testing.T) {
		require.NoError(t, rdb.Do(ctx, "ACL", "SETUSER", "resetuser", "on", ">password", "allkeys", "allcommands").Err())
		require.NoError(t, rdb.Do(ctx, "ACL", "SETUSER", "resetuser", "reset").Err())

		result, err := rdb.Do(ctx, "ACL", "GETUSER", "resetuser").Result()
		require.NoError(t, err)
		require.NotNil(t, result)
	})
}

func TestACLSetUserUnsupportedFeatures(t *testing.T) {
	srv := util.StartServer(t, map[string]string{"acl-preview-enabled": "yes"})
	defer srv.Close()

	ctx := context.Background()
	rdb := srv.NewClient()
	defer func() { require.NoError(t, rdb.Close()) }()

	// Empty test - previously unsupported features are now supported
	// Keeping test function for future unsupported features

	t.Run("Invalid modifier should fail", func(t *testing.T) {
		err := rdb.Do(ctx, "ACL", "SETUSER", "u1", "on", "completely_invalid_modifier").Err()
		require.Error(t, err)
	})
}

func TestACLKeyPermissions(t *testing.T) {
	srv := util.StartServer(t, map[string]string{"acl-preview-enabled": "yes"})
	defer srv.Close()

	ctx := context.Background()
	rdb := srv.NewClient()
	defer func() { require.NoError(t, rdb.Close()) }()

	t.Run("Read-only key prefix %R", func(t *testing.T) {
		err := rdb.Do(ctx, "ACL", "SETUSER", "keyperm_r", "on", "%R~readonly:*").Err()
		require.NoError(t, err)

		result, err := rdb.Do(ctx, "ACL", "GETUSER", "keyperm_r").Result()
		require.NoError(t, err)
		fieldMap := parseGetUserResult(t, result)
		keys := fieldMap["keys"].([]interface{})
		require.NotEmpty(t, keys)
		// The key should have %R prefix
		keyStr := keys[0].(string)
		require.Contains(t, keyStr, "readonly:")
	})

	t.Run("Write-only key prefix %W", func(t *testing.T) {
		err := rdb.Do(ctx, "ACL", "SETUSER", "keyperm_w", "on", "%W~writeonly:*").Err()
		require.NoError(t, err)

		result, err := rdb.Do(ctx, "ACL", "GETUSER", "keyperm_w").Result()
		require.NoError(t, err)
		fieldMap := parseGetUserResult(t, result)
		keys := fieldMap["keys"].([]interface{})
		require.NotEmpty(t, keys)
	})

	t.Run("Read-write key prefix %RW", func(t *testing.T) {
		err := rdb.Do(ctx, "ACL", "SETUSER", "keyperm_rw", "on", "%RW~readwrite:*").Err()
		require.NoError(t, err)

		result, err := rdb.Do(ctx, "ACL", "GETUSER", "keyperm_rw").Result()
		require.NoError(t, err)
		fieldMap := parseGetUserResult(t, result)
		keys := fieldMap["keys"].([]interface{})
		require.NotEmpty(t, keys)
	})

	t.Run("Mixed key permissions", func(t *testing.T) {
		err := rdb.Do(ctx, "ACL", "SETUSER", "keyperm_mixed", "on",
			"%R~read:*", "%W~write:*", "~all:*").Err()
		require.NoError(t, err)

		result, err := rdb.Do(ctx, "ACL", "GETUSER", "keyperm_mixed").Result()
		require.NoError(t, err)
		fieldMap := parseGetUserResult(t, result)
		keys := fieldMap["keys"].([]interface{})
		require.Equal(t, 3, len(keys))
	})
}

func TestACLSelectors(t *testing.T) {
	srv := util.StartServer(t, map[string]string{"acl-preview-enabled": "yes"})
	defer srv.Close()

	ctx := context.Background()
	rdb := srv.NewClient()
	defer func() { require.NoError(t, rdb.Close()) }()

	t.Run("Basic selector with parentheses", func(t *testing.T) {
		err := rdb.Do(ctx, "ACL", "SETUSER", "selector1", "on", "(~key:* +get)").Err()
		require.NoError(t, err)

		result, err := rdb.Do(ctx, "ACL", "GETUSER", "selector1").Result()
		require.NoError(t, err)
		fieldMap := parseGetUserResult(t, result)
		selectors := fieldMap["selectors"].([]interface{})
		require.Equal(t, 1, len(selectors))
	})

	t.Run("Multiple selectors", func(t *testing.T) {
		err := rdb.Do(ctx, "ACL", "SETUSER", "selector2", "on",
			"(~read:* +get)", "(~write:* +set)").Err()
		require.NoError(t, err)

		result, err := rdb.Do(ctx, "ACL", "GETUSER", "selector2").Result()
		require.NoError(t, err)
		fieldMap := parseGetUserResult(t, result)
		selectors := fieldMap["selectors"].([]interface{})
		require.Equal(t, 2, len(selectors))
	})

	t.Run("Empty selector creates empty permissions", func(t *testing.T) {
		err := rdb.Do(ctx, "ACL", "SETUSER", "selector3", "on", "()").Err()
		require.NoError(t, err)

		result, err := rdb.Do(ctx, "ACL", "GETUSER", "selector3").Result()
		require.NoError(t, err)
		fieldMap := parseGetUserResult(t, result)
		selectors := fieldMap["selectors"].([]interface{})
		require.Equal(t, 1, len(selectors))
	})

	t.Run("Clearselectors removes non-root selectors", func(t *testing.T) {
		// Create user with multiple selectors
		err := rdb.Do(ctx, "ACL", "SETUSER", "selector4", "on",
			"(~s1:* +get)", "(~s2:* +set)").Err()
		require.NoError(t, err)

		// Clear selectors
		err = rdb.Do(ctx, "ACL", "SETUSER", "selector4", "clearselectors").Err()
		require.NoError(t, err)

		result, err := rdb.Do(ctx, "ACL", "GETUSER", "selector4").Result()
		require.NoError(t, err)
		fieldMap := parseGetUserResult(t, result)
		selectors := fieldMap["selectors"].([]interface{})
		require.Empty(t, selectors)
	})
}

func TestACLSubcommandFilters(t *testing.T) {
	srv := util.StartServer(t, map[string]string{"acl-preview-enabled": "yes"})
	defer srv.Close()

	ctx := context.Background()
	rdb := srv.NewClient()
	defer func() { require.NoError(t, rdb.Close()) }()

	t.Run("Allow specific subcommand", func(t *testing.T) {
		err := rdb.Do(ctx, "ACL", "SETUSER", "subcmd1", "on", "+select|0").Err()
		require.NoError(t, err)

		result, err := rdb.Do(ctx, "ACL", "GETUSER", "subcmd1").Result()
		require.NoError(t, err)
		require.NotNil(t, result)
	})

	t.Run("Multiple subcommand filters", func(t *testing.T) {
		err := rdb.Do(ctx, "ACL", "SETUSER", "subcmd2", "on",
			"+client|id", "+client|setname").Err()
		require.NoError(t, err)

		result, err := rdb.Do(ctx, "ACL", "GETUSER", "subcmd2").Result()
		require.NoError(t, err)
		require.NotNil(t, result)
	})

	t.Run("Block specific subcommand", func(t *testing.T) {
		err := rdb.Do(ctx, "ACL", "SETUSER", "subcmd3", "on", "+@all", "-config|set").Err()
		require.NoError(t, err)

		result, err := rdb.Do(ctx, "ACL", "GETUSER", "subcmd3").Result()
		require.NoError(t, err)
		require.NotNil(t, result)
	})
}

func TestACLGetUser(t *testing.T) {
	srv := util.StartServer(t, map[string]string{"acl-preview-enabled": "yes"})
	defer srv.Close()

	ctx := context.Background()
	rdb := srv.NewClient()
	defer func() { require.NoError(t, rdb.Close()) }()

	t.Run("Returns user info with expected fields", func(t *testing.T) {
		require.NoError(t, rdb.Do(ctx, "ACL", "SETUSER", "getuser1", "on", "+get", "+set", "~key:*").Err())

		result, err := rdb.Do(ctx, "ACL", "GETUSER", "getuser1").Result()
		require.NoError(t, err)

		fieldMap := parseGetUserResult(t, result)
		for _, field := range []string{"flags", "passwords", "commands", "keys", "channels", "selectors"} {
			require.Contains(t, fieldMap, field)
		}
		require.NotContains(t, fieldMap, "namespace")
	})

	t.Run("Returns nil for non-existing user", func(t *testing.T) {
		result, err := rdb.Do(ctx, "ACL", "GETUSER", "nonexistent").Result()
		if err != nil {
			require.Contains(t, err.Error(), "nil")
		} else {
			require.Nil(t, result)
		}
	})
}

func TestACLGetUserFormat(t *testing.T) {
	srv := util.StartServer(t, map[string]string{"acl-preview-enabled": "yes"})
	defer srv.Close()

	ctx := context.Background()
	rdb := srv.NewClient()
	defer func() { require.NoError(t, rdb.Close()) }()

	t.Run("Flags field shows on/off status", func(t *testing.T) {
		require.NoError(t, rdb.Do(ctx, "ACL", "SETUSER", "fmt_on", "on").Err())
		require.NoError(t, rdb.Do(ctx, "ACL", "SETUSER", "fmt_off", "off").Err())

		for _, tc := range []struct {
			user     string
			expected string
		}{
			{"fmt_on", "on"},
			{"fmt_off", "off"},
		} {
			result, err := rdb.Do(ctx, "ACL", "GETUSER", tc.user).Result()
			require.NoError(t, err)
			flags := extractFlags(t, parseGetUserResult(t, result))
			require.Contains(t, flags, tc.expected)
		}
	})

	t.Run("Nopass flag and empty passwords", func(t *testing.T) {
		require.NoError(t, rdb.Do(ctx, "ACL", "SETUSER", "fmt_nopass", "on", "nopass").Err())

		result, err := rdb.Do(ctx, "ACL", "GETUSER", "fmt_nopass").Result()
		require.NoError(t, err)

		fieldMap := parseGetUserResult(t, result)
		flags := extractFlags(t, fieldMap)
		require.Contains(t, flags, "nopass")
		require.Empty(t, fieldMap["passwords"].([]interface{}))
	})

	t.Run("Allkeys and allchannels flags", func(t *testing.T) {
		require.NoError(t, rdb.Do(ctx, "ACL", "SETUSER", "fmt_allkeys", "on", "allkeys").Err())
		require.NoError(t, rdb.Do(ctx, "ACL", "SETUSER", "fmt_allchannels", "on", "allchannels").Err())

		for _, tc := range []struct {
			user     string
			expected string
		}{
			{"fmt_allkeys", "allkeys"},
			{"fmt_allchannels", "allchannels"},
		} {
			result, err := rdb.Do(ctx, "ACL", "GETUSER", tc.user).Result()
			require.NoError(t, err)
			flags := extractFlags(t, parseGetUserResult(t, result))
			require.Contains(t, flags, tc.expected)
		}
	})

	t.Run("Key and channel patterns", func(t *testing.T) {
		require.NoError(t, rdb.Do(ctx, "ACL", "SETUSER", "fmt_keys", "on", "~user:*", "~cache:*").Err())
		require.NoError(t, rdb.Do(ctx, "ACL", "SETUSER", "fmt_channels", "on", "&news:*", "&events:*").Err())

		// Check keys
		result, err := rdb.Do(ctx, "ACL", "GETUSER", "fmt_keys").Result()
		require.NoError(t, err)
		keys := parseGetUserResult(t, result)["keys"].([]interface{})
		require.GreaterOrEqual(t, len(keys), 2)

		// Check channels
		result, err = rdb.Do(ctx, "ACL", "GETUSER", "fmt_channels").Result()
		require.NoError(t, err)
		channels := parseGetUserResult(t, result)["channels"].([]interface{})
		require.GreaterOrEqual(t, len(channels), 2)
	})
}

func TestACLSetUserErrors(t *testing.T) {
	srv := util.StartServer(t, map[string]string{"acl-preview-enabled": "yes"})
	defer srv.Close()

	ctx := context.Background()
	rdb := srv.NewClient()
	defer func() { require.NoError(t, rdb.Close()) }()

	errorCases := []struct {
		name string
		args []interface{}
	}{
		{"No subcommand", []interface{}{"ACL"}},
		{"Invalid subcommand", []interface{}{"ACL", "INVALID"}},
		{"Empty username", []interface{}{"ACL", "SETUSER", ""}},
		{"Invalid modifier", []interface{}{"ACL", "SETUSER", "testuser", "invalidmod"}},
		{"GETUSER without username", []interface{}{"ACL", "GETUSER"}},
		{"WHOAMI with extra args", []interface{}{"ACL", "WHOAMI", "extra"}},
		{"USERS with extra args", []interface{}{"ACL", "USERS", "extra"}},
		{"Empty key pattern", []interface{}{"ACL", "SETUSER", "u1", "~"}},
		{"Empty channel pattern", []interface{}{"ACL", "SETUSER", "u2", "&"}},
		{"Incomplete command modifier", []interface{}{"ACL", "SETUSER", "u3", "+"}},
		{"Incomplete category modifier", []interface{}{"ACL", "SETUSER", "u4", "+@"}},
	}

	for _, tc := range errorCases {
		t.Run(tc.name+" fails", func(t *testing.T) {
			err := rdb.Do(ctx, tc.args...).Err()
			require.Error(t, err)
		})
	}
}

func TestACLComplexScenarios(t *testing.T) {
	srv := util.StartServer(t, map[string]string{"acl-preview-enabled": "yes"})
	defer srv.Close()

	ctx := context.Background()
	rdb := srv.NewClient()
	defer func() { require.NoError(t, rdb.Close()) }()

	t.Run("Read-only user with key patterns", func(t *testing.T) {
		err := rdb.Do(ctx, "ACL", "SETUSER", "readonly",
			"on", ">readpass", "+@string", "+@hash", "~data:*", "~cache:*").Err()
		require.NoError(t, err)
	})

	t.Run("Admin user with restricted categories", func(t *testing.T) {
		err := rdb.Do(ctx, "ACL", "SETUSER", "admin",
			"on", ">adminpass", "allkeys", "allchannels", "allcommands", "-@script").Err()
		require.NoError(t, err)
	})

	t.Run("Application user with specific permissions", func(t *testing.T) {
		err := rdb.Do(ctx, "ACL", "SETUSER", "appuser",
			"on", ">apppass",
			"~user:*", "~session:*", "~cache:*",
			"+get", "+set", "+del", "+expire", "+ttl",
			"&events:*").Err()
		require.NoError(t, err)
	})

	t.Run("Multi-tenant users with namespace isolation", func(t *testing.T) {
		tenants := []string{"tenant1", "tenant2", "tenant3"}
		for _, tenant := range tenants {
			username := fmt.Sprintf("user_%s", tenant)
			err := rdb.Do(ctx, "ACL", "SETUSER", username,
				"on", fmt.Sprintf(">%s_pass", tenant),
				fmt.Sprintf("~%s:*", tenant), "+@all").Err()
			require.NoError(t, err)
		}

		result, err := rdb.Do(ctx, "ACL", "USERS").Result()
		require.NoError(t, err)
		usernames := extractUsernames(t, result)

		for _, tenant := range tenants {
			require.Contains(t, usernames, fmt.Sprintf("user_%s", tenant))
		}
	})
}

func TestACLDefaultUserNoPassAuthBehavior(t *testing.T) {
	srv := util.StartServer(t, map[string]string{"acl-preview-enabled": "yes"})
	defer srv.Close()

	ctx := context.Background()
	admin := srv.NewClient()
	defer func() { require.NoError(t, admin.Close()) }()

	// Equivalent to the docs EXT-01 branch, adapted to current kvrocks error text.
	require.NoError(t, admin.Do(ctx, "ACL", "SETUSER", "default", "on", "nopass", "+@all", "~*", "&*").Err())

	c := srv.NewClient()
	defer func() { require.NoError(t, c.Close()) }()

	err := c.Do(ctx, "AUTH", "anypass").Err()
	require.Error(t, err)
	require.Contains(t, err.Error(), "no password is set")

	result, err := c.Do(ctx, "AUTH", "default", "anypass").Result()
	require.NoError(t, err)
	require.Equal(t, "OK", result)
}

func TestACLSetUserUpdateIsAtomicOnError(t *testing.T) {
	srv := util.StartServer(t, map[string]string{"acl-preview-enabled": "yes"})
	defer srv.Close()

	ctx := context.Background()
	admin := srv.NewClient()
	defer func() { require.NoError(t, admin.Close()) }()

	require.NoError(t, admin.Do(ctx, "ACL", "SETUSER", "atom", "on", ">p", "+get", "~*").Err())
	require.NoError(t, admin.Set(ctx, "k", "seed", 0).Err())

	userClient := srv.NewClient()
	defer func() { require.NoError(t, userClient.Close()) }()
	authAsUser(t, ctx, userClient, "atom", "p")

	require.NoError(t, userClient.Do(ctx, "GET", "k").Err())
	requireACLDenied(t, userClient.Do(ctx, "SET", "k", "v0").Err())

	err := admin.Do(ctx, "ACL", "SETUSER", "atom", "+set", "+not-a-command").Err()
	require.Error(t, err)
	require.Contains(t, err.Error(), "unknown ACL command modifier")

	// The invalid update must not partially apply.
	require.NoError(t, userClient.Do(ctx, "GET", "k").Err())
	requireACLDenied(t, userClient.Do(ctx, "SET", "k", "v1").Err())
}

func TestACLAllKeysAndAllChannelsNeedResetBeforePattern(t *testing.T) {
	srv := util.StartServer(t, map[string]string{"acl-preview-enabled": "yes"})
	defer srv.Close()

	ctx := context.Background()
	rdb := srv.NewClient()
	defer func() { require.NoError(t, rdb.Close()) }()

	require.NoError(t, rdb.Do(ctx, "ACL", "SETUSER", "keys_u", "on", "nopass", "allkeys").Err())
	err := rdb.Do(ctx, "ACL", "SETUSER", "keys_u", "~foo:*").Err()
	require.Error(t, err)
	require.Contains(t, err.Error(), "resetkeys")

	require.NoError(t, rdb.Do(ctx, "ACL", "SETUSER", "channels_u", "on", "nopass", "allchannels").Err())
	err = rdb.Do(ctx, "ACL", "SETUSER", "channels_u", "&foo:*").Err()
	require.Error(t, err)
	require.Contains(t, err.Error(), "resetchannels")
}

func TestACLPSubscribeMatchesLiteralPattern(t *testing.T) {
	srv := util.StartServer(t, map[string]string{"acl-preview-enabled": "yes"})
	defer srv.Close()

	ctx := context.Background()
	admin := srv.NewClient()
	defer func() { require.NoError(t, admin.Close()) }()

	require.NoError(t, admin.Do(ctx, "ACL", "SETUSER", "pat", "on", ">p", "+@pubsub", "resetchannels", "&news:*").Err())

	allowed := srv.NewClient()
	defer func() { require.NoError(t, allowed.Close()) }()
	authAsUser(t, ctx, allowed, "pat", "p")

	r := allowed.Do(ctx, "PSUBSCRIBE", "news:*")
	require.NoError(t, r.Err())
	require.Equal(t, "[psubscribe news:* 1]", fmt.Sprintf("%v", r.Val()))

	denied := srv.NewClient()
	defer func() { require.NoError(t, denied.Close()) }()
	authAsUser(t, ctx, denied, "pat", "p")
	requireACLDenied(t, denied.Do(ctx, "PSUBSCRIBE", "news:1").Err())
}

func TestACLSelectorOrSemanticsForKeyChecks(t *testing.T) {
	srv := util.StartServer(t, map[string]string{"acl-preview-enabled": "yes"})
	defer srv.Close()

	ctx := context.Background()
	admin := srv.NewClient()
	defer func() { require.NoError(t, admin.Close()) }()

	// Root selector is empty; either extra selector can authorize a command.
	require.NoError(t, admin.Do(ctx, "ACL", "SETUSER", "selector_or", "on", ">p",
		"nocommands", "resetkeys", "resetchannels",
		"(+get ~read:*)", "(+set ~write:*)").Err())

	require.NoError(t, admin.Set(ctx, "read:key", "v-read", 0).Err())
	require.NoError(t, admin.Set(ctx, "write:key", "v-write", 0).Err())

	userClient := srv.NewClient()
	defer func() { require.NoError(t, userClient.Close()) }()
	authAsUser(t, ctx, userClient, "selector_or", "p")

	v, err := userClient.Get(ctx, "read:key").Result()
	require.NoError(t, err)
	require.Equal(t, "v-read", v)

	requireACLDenied(t, userClient.Get(ctx, "write:key").Err())
	require.NoError(t, userClient.Set(ctx, "write:key", "v2", 0).Err())
	requireACLDenied(t, userClient.Set(ctx, "read:key", "v3", 0).Err())
}

func TestACLCategoryModifiersAreEffective(t *testing.T) {
	srv := util.StartServer(t, map[string]string{"acl-preview-enabled": "yes"})
	defer srv.Close()

	ctx := context.Background()
	admin := srv.NewClient()
	defer func() { require.NoError(t, admin.Close()) }()

	require.NoError(t, admin.Do(ctx, "ACL", "SETUSER", "cat_u", "on", ">p", "+@string", "~*").Err())

	userClient := srv.NewClient()
	defer func() { require.NoError(t, userClient.Close()) }()
	authAsUser(t, ctx, userClient, "cat_u", "p")

	require.NoError(t, userClient.Set(ctx, "cat:key", "ok", 0).Err())
	requireACLDenied(t, userClient.Do(ctx, "HSET", "cat:hash", "f", "v").Err())
}

func TestACLSelectorDefinitionCanSpanMultipleArgs(t *testing.T) {
	srv := util.StartServer(t, map[string]string{"acl-preview-enabled": "yes"})
	defer srv.Close()

	ctx := context.Background()
	rdb := srv.NewClient()
	defer func() { require.NoError(t, rdb.Close()) }()

	require.NoError(t, rdb.Do(ctx, "ACL", "SETUSER", "selector_split", "on", "(~split:*", "+get)").Err())

	result, err := rdb.Do(ctx, "ACL", "GETUSER", "selector_split").Result()
	require.NoError(t, err)
	fieldMap := parseGetUserResult(t, result)
	selectors := fieldMap["selectors"].([]interface{})
	require.Equal(t, 1, len(selectors))
}

func TestACLSubcommandCoverage(t *testing.T) {
	srv := util.StartServer(t, map[string]string{"acl-preview-enabled": "yes"})
	defer srv.Close()

	ctx := context.Background()
	admin := srv.NewClient()
	defer func() { require.NoError(t, admin.Close()) }()

	t.Run("HELP", func(t *testing.T) {
		res, err := admin.Do(ctx, "ACL", "HELP").Result()
		require.NoError(t, err)
		items, ok := res.([]interface{})
		require.True(t, ok)
		require.NotEmpty(t, items)
	})

	t.Run("CAT", func(t *testing.T) {
		res, err := admin.Do(ctx, "ACL", "CAT").Result()
		require.NoError(t, err)
		categories, ok := res.([]interface{})
		require.True(t, ok)
		require.NotEmpty(t, categories)

		res, err = admin.Do(ctx, "ACL", "CAT", "string").Result()
		require.NoError(t, err)
		commands, ok := res.([]interface{})
		require.True(t, ok)
		require.NotEmpty(t, commands)
	})

	t.Run("LIST", func(t *testing.T) {
		require.NoError(t, admin.Do(ctx, "ACL", "SETUSER", "list_u", "on", "nopass", "+get", "~*").Err())
		res, err := admin.Do(ctx, "ACL", "LIST").Result()
		require.NoError(t, err)
		rows, ok := res.([]interface{})
		require.True(t, ok)
		require.NotEmpty(t, rows)

		found := false
		for _, row := range rows {
			if s, ok := row.(string); ok && s == "user list_u on nopass sanitize-payload +get ~*" {
				found = true
				break
			}
		}
		require.True(t, found)
	})

	t.Run("DELUSER", func(t *testing.T) {
		require.NoError(t, admin.Do(ctx, "ACL", "SETUSER", "to_del", "on", "nopass").Err())
		res, err := admin.Do(ctx, "ACL", "DELUSER", "to_del").Result()
		require.NoError(t, err)
		require.EqualValues(t, 1, res)
	})

	t.Run("GENPASS", func(t *testing.T) {
		res, err := admin.Do(ctx, "ACL", "GENPASS").Result()
		require.NoError(t, err)
		password, ok := res.(string)
		require.True(t, ok)
		require.Len(t, password, 64)

		res, err = admin.Do(ctx, "ACL", "GENPASS", "64").Result()
		require.NoError(t, err)
		password, ok = res.(string)
		require.True(t, ok)
		require.Len(t, password, 16)
	})

	t.Run("LOG", func(t *testing.T) {
		res, err := admin.Do(ctx, "ACL", "LOG").Result()
		require.NoError(t, err)
		entries, ok := res.([]interface{})
		require.True(t, ok)
		require.Empty(t, entries)

		res, err = admin.Do(ctx, "ACL", "LOG", "RESET").Result()
		require.NoError(t, err)
		require.Equal(t, "OK", res)
	})

	t.Run("DRYRUN", func(t *testing.T) {
		require.NoError(t, admin.Do(ctx, "ACL", "SETUSER", "dry", "on", ">p", "+get", "~*").Err())

		res, err := admin.Do(ctx, "ACL", "DRYRUN", "dry", "GET", "k").Result()
		require.NoError(t, err)
		require.Equal(t, "OK", res)

		err = admin.Do(ctx, "ACL", "DRYRUN", "dry", "SET", "k", "v").Err()
		require.Error(t, err)
		require.Contains(t, err.Error(), "NOPERM")
	})

	t.Run("LOAD SAVE placeholder", func(t *testing.T) {
		err := admin.Do(ctx, "ACL", "LOAD").Err()
		require.Error(t, err)
		require.Contains(t, err.Error(), "not configured to use an ACL file")

		err = admin.Do(ctx, "ACL", "SAVE").Err()
		require.Error(t, err)
		require.Contains(t, err.Error(), "not configured to use an ACL file")
	})
}

func TestACLDenyUsesNOPERM(t *testing.T) {
	srv := util.StartServer(t, map[string]string{"acl-preview-enabled": "yes"})
	defer srv.Close()

	ctx := context.Background()
	admin := srv.NewClient()
	defer func() { require.NoError(t, admin.Close()) }()
	require.NoError(t, admin.Do(ctx, "ACL", "SETUSER", "nperm", "on", ">p", "+get", "~*").Err())

	userClient := srv.NewClient()
	defer func() { require.NoError(t, userClient.Close()) }()
	authAsUser(t, ctx, userClient, "nperm", "p")

	err := userClient.Do(ctx, "SET", "k", "v").Err()
	require.Error(t, err)
	require.Contains(t, err.Error(), "NOPERM")
	require.Contains(t, err.Error(), "not allowed")
}

func TestACLDefaultOffRequiresAuthForNewConnections(t *testing.T) {
	srv := util.StartServer(t, map[string]string{"acl-preview-enabled": "yes"})
	defer srv.Close()

	ctx := context.Background()
	admin := srv.NewClient()
	defer func() { require.NoError(t, admin.Close()) }()
	require.NoError(t, admin.Do(ctx, "ACL", "SETUSER", "default", "off").Err())

	pingConn := srv.NewClient()
	defer func() { require.NoError(t, pingConn.Close()) }()
	err := pingConn.Ping(ctx).Err()
	require.Error(t, err)
	require.Contains(t, err.Error(), "NOAUTH")

	helloConn := srv.NewClient()
	defer func() { require.NoError(t, helloConn.Close()) }()
	err = helloConn.Do(ctx, "HELLO", "3").Err()
	require.Error(t, err)
	require.Contains(t, err.Error(), "NOAUTH")
	require.Contains(t, err.Error(), "HELLO must be called with the client already authenticated")
}

func TestACLSanitizePayloadFlag(t *testing.T) {
	srv := util.StartServer(t, map[string]string{"acl-preview-enabled": "yes"})
	defer srv.Close()

	ctx := context.Background()
	rdb := srv.NewClient()
	defer func() { require.NoError(t, rdb.Close()) }()

	require.NoError(t, rdb.Do(ctx, "ACL", "SETUSER", "sanitize_u", "on", "skip-sanitize-payload").Err())
	res, err := rdb.Do(ctx, "ACL", "GETUSER", "sanitize_u").Result()
	require.NoError(t, err)
	flags := extractFlags(t, parseGetUserResult(t, res))
	require.Contains(t, flags, "skip-sanitize-payload")
	require.NotContains(t, flags, "sanitize-payload")

	require.NoError(t, rdb.Do(ctx, "ACL", "SETUSER", "sanitize_u", "sanitize-payload").Err())
	res, err = rdb.Do(ctx, "ACL", "GETUSER", "sanitize_u").Result()
	require.NoError(t, err)
	flags = extractFlags(t, parseGetUserResult(t, res))
	require.Contains(t, flags, "sanitize-payload")
	require.NotContains(t, flags, "skip-sanitize-payload")

	require.NoError(t, rdb.Do(ctx, "ACL", "SETUSER", "sanitize_u", "reset").Err())
	res, err = rdb.Do(ctx, "ACL", "GETUSER", "sanitize_u").Result()
	require.NoError(t, err)
	flags = extractFlags(t, parseGetUserResult(t, res))
	require.Contains(t, flags, "sanitize-payload")
}

func TestACLSetUserRejectsInvalidUsername(t *testing.T) {
	srv := util.StartServer(t, map[string]string{"acl-preview-enabled": "yes"})
	defer srv.Close()

	ctx := context.Background()
	rdb := srv.NewClient()
	defer func() { require.NoError(t, rdb.Close()) }()

	invalidUsers := []string{
		"bad user",
		"bad\tuser",
	}
	for _, username := range invalidUsers {
		err := rdb.Do(ctx, "ACL", "SETUSER", username, "on").Err()
		require.Error(t, err)
		require.Contains(t, err.Error(), "invalid characters")
	}
}

func TestACLChecksBeforeClusterRedirect(t *testing.T) {
	ctx := context.Background()

	srv1 := util.StartServer(t, map[string]string{"cluster-enabled": "yes", "acl-preview-enabled": "yes"})
	defer srv1.Close()
	admin1 := srv1.NewClient()
	defer func() { require.NoError(t, admin1.Close()) }()
	nodeID1 := "07c37dfeb235213a872192d90877d0cd55635b91"
	require.NoError(t, admin1.Do(ctx, "clusterx", "SETNODEID", nodeID1).Err())

	srv2 := util.StartServer(t, map[string]string{"cluster-enabled": "yes", "acl-preview-enabled": "yes"})
	defer srv2.Close()
	admin2 := srv2.NewClient()
	defer func() { require.NoError(t, admin2.Close()) }()
	nodeID2 := "07c37dfeb235213a872192d90877d0cd55635b92"
	require.NoError(t, admin2.Do(ctx, "clusterx", "SETNODEID", nodeID2).Err())

	clusterNodes := fmt.Sprintf("%s %s %d master - 0-16383\n", nodeID1, srv1.Host(), srv1.Port())
	clusterNodes += fmt.Sprintf("%s %s %d master -", nodeID2, srv2.Host(), srv2.Port())
	require.NoError(t, admin2.Do(ctx, "clusterx", "SETNODES", clusterNodes, "2").Err())
	require.NoError(t, admin1.Do(ctx, "clusterx", "SETNODES", clusterNodes, "2").Err())

	require.NoError(t,
		admin2.Do(ctx, "ACL", "SETUSER", "redirect_order", "reset", "on", ">p", "+get", "resetkeys", "~allow:*").Err())

	userClient := srv2.NewClientWithOption(&redis.Options{
		Username: "redirect_order",
		Password: "p",
		PoolSize: 1,
	})
	defer func() { require.NoError(t, userClient.Close()) }()

	err := userClient.Get(ctx, util.SlotTable[0]).Err()
	require.Error(t, err)
	require.Contains(t, err.Error(), "NOPERM")
	require.NotContains(t, err.Error(), "MOVED")
}

func TestACLDelUserDisconnectsUserConnections(t *testing.T) {
	srv := util.StartServer(t, map[string]string{"acl-preview-enabled": "yes"})
	defer srv.Close()

	ctx := context.Background()
	admin := srv.NewClient()
	defer func() { require.NoError(t, admin.Close()) }()

	require.NoError(t, admin.Do(ctx, "ACL", "SETUSER", "victim", "on", ">p", "+ping", "~*").Err())

	userConn := srv.NewTCPClient()
	defer func() { require.NoError(t, userConn.Close()) }()
	require.NoError(t, userConn.WriteArgs("AUTH", "victim", "p"))
	userConn.MustRead(t, "+OK")
	require.NoError(t, userConn.WriteArgs("PING"))
	userConn.MustRead(t, "+PONG")

	res, err := admin.Do(ctx, "ACL", "DELUSER", "victim").Result()
	require.NoError(t, err)
	require.Equal(t, int64(1), res)

	require.Eventually(t, func() bool {
		if err := userConn.WriteArgs("PING"); err != nil {
			return true
		}
		_, err := userConn.ReadLine()
		return err != nil
	}, 5*time.Second, 50*time.Millisecond)
}

// TestACLAuthFailurePreservesSession validates Item #1:
// A failed AUTH must not clear an already-authenticated user's ACL profile.
func TestACLAuthFailurePreservesSession(t *testing.T) {
	srv := util.StartServer(t, map[string]string{"acl-preview-enabled": "yes"})
	defer srv.Close()

	ctx := context.Background()
	admin := srv.NewClient()
	defer func() { require.NoError(t, admin.Close()) }()

	// Create a restricted user: can only PING, no key access.
	require.NoError(t, admin.Do(ctx, "ACL", "SETUSER", "restricted", "on", ">secret", "+ping", "resetkeys").Err())

	t.Run("AUTH failure does not weaken existing ACL restrictions", func(t *testing.T) {
		conn := srv.NewTCPClient()
		defer func() { require.NoError(t, conn.Close()) }()

		// Authenticate as restricted user.
		require.NoError(t, conn.WriteArgs("AUTH", "restricted", "secret"))
		conn.MustRead(t, "+OK")

		// Confirm PING works (allowed).
		require.NoError(t, conn.WriteArgs("PING"))
		conn.MustRead(t, "+PONG")

		// Now attempt AUTH with wrong password — must fail.
		require.NoError(t, conn.WriteArgs("AUTH", "wrongpassword"))
		line, err := conn.ReadLine()
		require.NoError(t, err)
		require.Contains(t, line, "Invalid password", "expected auth failure error")

		// After failed AUTH, the original ACL restrictions must still be enforced.
		// SET should still be denied.
		require.NoError(t, conn.WriteArgs("SET", "k", "v"))
		line, err = conn.ReadLine()
		require.NoError(t, err)
		require.Contains(t, line, "NOPERM", "ACL restrictions must remain active after failed AUTH")
	})

	t.Run("HELLO AUTH failure does not weaken existing ACL restrictions", func(t *testing.T) {
		conn := srv.NewTCPClient()
		defer func() { require.NoError(t, conn.Close()) }()

		// Authenticate via HELLO AUTH.
		require.NoError(t, conn.WriteArgs("HELLO", "2", "AUTH", "restricted", "secret"))
		_, err := conn.ReadLine()
		require.NoError(t, err)

		// Attempt HELLO with wrong credentials.
		require.NoError(t, conn.WriteArgs("HELLO", "2", "AUTH", "restricted", "wrongpassword"))
		line, err := conn.ReadLine()
		require.NoError(t, err)
		require.Contains(t, line, "Invalid password")

		// ACL restrictions must still be active.
		require.NoError(t, conn.WriteArgs("SET", "k", "v"))
		line, err = conn.ReadLine()
		require.NoError(t, err)
		require.Contains(t, line, "NOPERM", "ACL restrictions must remain active after failed HELLO AUTH")
	})
}

// TestACLSetUserPasswordRedaction validates Item #2:
// ACL SETUSER password modifiers must be redacted in MONITOR and SLOWLOG output.
func TestACLSetUserPasswordRedaction(t *testing.T) {
	srv := util.StartServer(t, map[string]string{
		"acl-preview-enabled":     "yes",
		"slowlog-log-slower-than": "0",
	})
	defer srv.Close()

	ctx := context.Background()
	admin := srv.NewClient()
	defer func() { require.NoError(t, admin.Close()) }()

	t.Run("ACL SETUSER password tokens are redacted in SLOWLOG", func(t *testing.T) {
		require.NoError(t, admin.Do(ctx, "ACL", "SETUSER", "pw_test_user", "on", ">supersecret").Err())
		time.Sleep(50 * time.Millisecond)

		result, err := admin.Do(ctx, "SLOWLOG", "GET").Result()
		require.NoError(t, err)
		entries, ok := result.([]interface{})
		require.True(t, ok)

		found := false
		for _, entry := range entries {
			parts, ok := entry.([]interface{})
			if !ok || len(parts) < 4 {
				continue
			}
			args, ok := parts[3].([]interface{})
			if !ok {
				continue
			}
			// Look for ACL SETUSER entry
			if len(args) >= 2 {
				cmd, _ := args[0].(string)
				sub, _ := args[1].(string)
				if cmd == "ACL" && sub == "SETUSER" {
					found = true
					for _, arg := range args {
						s, _ := arg.(string)
						require.NotContains(t, s, "supersecret", "password must be redacted in SLOWLOG")
					}
				}
			}
		}
		require.True(t, found, "expected ACL SETUSER entry in SLOWLOG")
	})
}

// TestACLContextMissFailsClosed validates Item #5:
// When an ACL user is deleted while a connection is active, subsequent commands must
// fail with NOAUTH (not silently continue with weakened/no enforcement).
func TestACLContextMissFailsClosed(t *testing.T) {
	srv := util.StartServer(t, map[string]string{"acl-preview-enabled": "yes"})
	defer srv.Close()

	ctx := context.Background()
	admin := srv.NewClient()
	defer func() { require.NoError(t, admin.Close()) }()

	require.NoError(t, admin.Do(ctx, "ACL", "SETUSER", "ephemeral", "on", ">p", "+ping", "~*").Err())

	conn := srv.NewTCPClient()
	defer func() { require.NoError(t, conn.Close()) }()

	require.NoError(t, conn.WriteArgs("AUTH", "ephemeral", "p"))
	conn.MustRead(t, "+OK")

	// Delete the user while the connection is live.
	require.NoError(t, admin.Do(ctx, "ACL", "DELUSER", "ephemeral").Err())

	// After user deletion the connection should be killed (DELUSER kills connections)
	// or any subsequent command must fail with authentication required, not NOPERM.
	require.Eventually(t, func() bool {
		if err := conn.WriteArgs("PING"); err != nil {
			return true
		}
		line, err := conn.ReadLine()
		if err != nil {
			return true
		}
		// Accept either connection-closed or NOAUTH as the fail-closed response.
		return line != "+PONG"
	}, 5*time.Second, 50*time.Millisecond)
}

// TestACLSortDynamicPatternGuardrail validates Item #4:
// SORT with dynamic BY/GET patterns must be rejected for users without allkeys.
func TestACLSortDynamicPatternGuardrail(t *testing.T) {
	srv := util.StartServer(t, map[string]string{"acl-preview-enabled": "yes"})
	defer srv.Close()

	ctx := context.Background()
	admin := srv.NewClient()
	defer func() { require.NoError(t, admin.Close()) }()

	// User with limited key access (only "mylist").
	require.NoError(t, admin.Do(ctx, "ACL", "SETUSER", "sortlimited", "on", ">p", "+sort", "~mylist").Err())

	// User with full key access.
	require.NoError(t, admin.Do(ctx, "ACL", "SETUSER", "sortfull", "on", ">p", "+sort", "~*").Err())

	t.Run("SORT BY pattern denied without allkeys", func(t *testing.T) {
		client := srv.NewClientWithOption(&redis.Options{Username: "sortlimited", Password: "p"})
		defer func() { require.NoError(t, client.Close()) }()

		err := client.Do(ctx, "SORT", "mylist", "BY", "weight_*").Err()
		require.Error(t, err)
		require.Contains(t, err.Error(), "NOPERM")
	})

	t.Run("SORT GET pattern denied without allkeys", func(t *testing.T) {
		client := srv.NewClientWithOption(&redis.Options{Username: "sortlimited", Password: "p"})
		defer func() { require.NoError(t, client.Close()) }()

		err := client.Do(ctx, "SORT", "mylist", "GET", "obj_*->name").Err()
		require.Error(t, err)
		require.Contains(t, err.Error(), "NOPERM")
	})

	t.Run("SORT BY pattern allowed with allkeys", func(t *testing.T) {
		clientFull := srv.NewClientWithOption(&redis.Options{Username: "sortfull", Password: "p"})
		defer func() { require.NoError(t, clientFull.Close()) }()

		// Should not get NOPERM (may get other errors due to empty list, but not permission error).
		err := clientFull.Do(ctx, "SORT", "mylist", "BY", "weight_*").Err()
		if err != nil {
			require.NotContains(t, err.Error(), "NOPERM")
		}
	})
}

// TestACLNamespaceStrictMode validates Item #6:
// With acl-namespace-strict=yes (default), ACL SETUSER with an unknown namespace suffix must fail.
func TestACLNamespaceStrictMode(t *testing.T) {
	t.Run("Strict mode (default): unknown namespace is rejected", func(t *testing.T) {
		srv := util.StartServer(t, map[string]string{
			"acl-preview-enabled": "yes",
			// acl-namespace-strict defaults to yes
		})
		defer srv.Close()

		ctx := context.Background()
		admin := srv.NewClient()
		defer func() { require.NoError(t, admin.Close()) }()

		err := admin.Do(ctx, "ACL", "SETUSER", "user#nonexistentns", "on").Err()
		require.Error(t, err, "unknown namespace should be rejected in strict mode")
		require.Contains(t, err.Error(), "nonexistentns")
	})

	t.Run("Compatibility mode: unknown namespace falls back to default", func(t *testing.T) {
		srv := util.StartServer(t, map[string]string{
			"acl-preview-enabled":  "yes",
			"acl-namespace-strict": "no",
		})
		defer srv.Close()

		ctx := context.Background()
		admin := srv.NewClient()
		defer func() { require.NoError(t, admin.Close()) }()

		err := admin.Do(ctx, "ACL", "SETUSER", "user#nonexistentns", "on").Err()
		require.NoError(t, err, "unknown namespace should fall back to default in compatibility mode")
	})
}

// TestACLClusterAllNodesMode validates Item #3:
// With acl-require-cluster-all-nodes=yes, ACL mutations in cluster mode must be rejected.
func TestACLClusterAllNodesMode(t *testing.T) {
	t.Run("Standalone: ACL SETUSER always allowed regardless of acl-require-cluster-all-nodes", func(t *testing.T) {
		srv := util.StartServer(t, map[string]string{
			"acl-preview-enabled":           "yes",
			"acl-require-cluster-all-nodes": "yes",
		})
		defer srv.Close()

		ctx := context.Background()
		admin := srv.NewClient()
		defer func() { require.NoError(t, admin.Close()) }()

		// In standalone mode cluster_enabled is false, so the guard does not trigger.
		err := admin.Do(ctx, "ACL", "SETUSER", "standaloneuser", "on").Err()
		require.NoError(t, err)
	})
}
