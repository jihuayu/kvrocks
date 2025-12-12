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
		for _, field := range []string{"flags", "passwords", "commands", "keys", "channels", "selectors", "namespace"} {
			require.Contains(t, fieldMap, field)
		}
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
