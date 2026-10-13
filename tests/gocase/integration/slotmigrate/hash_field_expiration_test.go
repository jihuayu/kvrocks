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

package slotmigrate

import (
	"context"
	"errors"
	"fmt"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/apache/kvrocks/tests/gocase/util"
	"github.com/redis/go-redis/v9"
	"github.com/stretchr/testify/require"
)

func TestSlotMigrateHashFieldExpiration(t *testing.T) {
	ctx := context.Background()
	source := util.StartServer(t, util.KvrocksServerConfigs{
		"cluster-enabled":                  "yes",
		"hash-encoding-mode":               "legacy",
		"rocksdb.disable_auto_compactions": "yes",
		"migrate-batch-size-kb":            "1",
	})
	defer source.Close()
	sourceClient := source.NewClient()
	defer func() { require.NoError(t, sourceClient.Close()) }()
	sourceID := "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx00"
	require.NoError(t, sourceClient.Do(ctx, "clusterx", "SETNODEID", sourceID).Err())

	destination := util.StartServer(t, util.KvrocksServerConfigs{
		"cluster-enabled":                  "yes",
		"hash-encoding-mode":               "legacy",
		"rocksdb.disable_auto_compactions": "yes",
	})
	defer destination.Close()
	destinationClient := destination.NewClient()
	defer func() { require.NoError(t, destinationClient.Close()) }()
	destinationID := "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx01"
	require.NoError(t, destinationClient.Do(ctx, "clusterx", "SETNODEID", destinationID).Err())
	nodes := fmt.Sprintf("%s %s %d master - 0-10000\n%s %s %d master - 10001-16383",
		sourceID, source.Host(), source.Port(), destinationID, destination.Host(), destination.Port())
	require.NoError(t, sourceClient.Do(ctx, "clusterx", "SETNODES", nodes, "1").Err())
	require.NoError(t, destinationClient.Do(ctx, "clusterx", "SETNODES", nodes, "1").Err())

	for slot, phase := range []string{"snapshot", "wal"} {
		t.Run(phase, func(t *testing.T) {
			key := fmt.Sprintf("hfe_{%s}", util.SlotTable[slot])
			legacyKey := fmt.Sprintf("legacy_{%s}", util.SlotTable[slot])
			binaryValue := strings.Repeat("\x00", 8) + strings.Repeat("value\xff", 512)
			require.NoError(t, sourceClient.ConfigSet(ctx, "hash-encoding-mode", "legacy").Err())
			require.NoError(t, sourceClient.HSet(ctx, legacyKey, "field", binaryValue).Err())
			require.NoError(t, sourceClient.ConfigSet(ctx, "hash-encoding-mode", "field-expiration").Err())
			require.NoError(t, sourceClient.HSet(ctx, key, "persistent", binaryValue, "live", "10",
				"persist", "20", "overwrite", "old", "deleted", "gone", "expired", "gone").Err())
			expireAt := time.Now().Add(10 * time.Minute).UnixMilli()
			require.NoError(t, sourceClient.Do(ctx, "hpexpireat", key, expireAt, "FIELDS", 4,
				"live", "persist", "overwrite", "deleted").Err())
			expiredValue, err := sourceClient.HGet(ctx, key, "expired").Result()
			require.NoError(t, err)
			require.Equal(t, "gone", expiredValue)
			expiredResult, err := sourceClient.Do(ctx, "hpexpire", key, 1000, "FIELDS", 1, "expired").Result()
			require.NoError(t, err)
			require.Equal(t, []interface{}{int64(1)}, expiredResult)
			require.NoError(t, sourceClient.PExpireAt(ctx, key, time.Now().Add(20*time.Minute)).Err())
			// This field must already be expired on the source before migration starts.
			require.Eventually(t, func() bool {
				return errors.Is(sourceClient.HGet(ctx, key, "expired").Err(), redis.Nil)
			}, 5*time.Second, 10*time.Millisecond)

			values := map[string]string{
				"persistent": binaryValue, "live": "10", "persist": "20", "overwrite": "old", "deleted": "gone",
			}
			expires := []interface{}{int64(-1), expireAt, expireAt, expireAt, expireAt, int64(-2)}
			expiringKey, expiringField, expiringValue := key+"_new", "short-lived", "temporary"
			updates, err := destinationClient.Do(ctx, "POLLUPDATES", 0, "MAX", 1).Result()
			require.NoError(t, err)
			migrationSequence := updates.(map[any]any)["latest_sequence"].(int64)
			if phase == "wal" {
				// Keep the snapshot transfer running while subsequent hash changes enter the WAL.
				require.NoError(t, sourceClient.ConfigSet(ctx, "migrate-batch-rate-limit-mb", "1").Err())
				fillerKey := fmt.Sprintf("filler_{%s}", util.SlotTable[slot])
				pipe := sourceClient.Pipeline()
				for i := 0; i < 1024; i++ {
					pipe.RPush(ctx, fillerKey, strings.Repeat("x", 4096))
				}
				_, err := pipe.Exec(ctx)
				require.NoError(t, err)
				require.NoError(t, sourceClient.Do(ctx, "clusterx", "migrate", slot, destinationID).Err())
				// Import starts only after the source has acquired its snapshot.
				waitForImportState(t, destinationClient, slot, "start")
				requireMigrateState(t, sourceClient, slot, SlotMigrationStateStarted)

				require.NoError(t, sourceClient.HIncrBy(ctx, key, "live", 5).Err())
				require.NoError(t, sourceClient.Do(ctx, "hpexpireat", key, expireAt+60000, "FIELDS", 1, "persistent").Err())
				require.NoError(t, sourceClient.Do(ctx, "hpersist", key, "FIELDS", 1, "persist").Err())
				require.NoError(t, sourceClient.HSet(ctx, key, "overwrite", "new").Err())
				require.NoError(t, sourceClient.HDel(ctx, key, "deleted").Err())
				require.NoError(t, sourceClient.HSet(ctx, legacyKey, "field", binaryValue+"updated").Err())
				values["live"] = "15"
				values["overwrite"] = "new"
				delete(values, "deleted")
				expires = []interface{}{expireAt + 60000, expireAt, int64(-1), int64(-1), int64(-2), int64(-2)}
			}
			require.NoError(t, sourceClient.Do(ctx, "hsetex", expiringKey, "PXAT", expireAt,
				"FIELDS", 2, "field", "created-for-migration", expiringField, expiringValue).Err())
			fieldTTL := 3 * time.Second
			if phase == "wal" {
				fieldTTL = 8 * time.Second
			}
			fieldExpireAt := time.Now().Add(fieldTTL).UnixMilli()
			require.NoError(t, sourceClient.Do(ctx, "hpexpireat", expiringKey, fieldExpireAt, "FIELDS", 1, expiringField).Err())
			expiringMetadata := util.GetKMetadata(t, sourceClient, ctx, expiringKey)

			metadata := util.GetKMetadata(t, sourceClient, ctx, key)
			require.Equal(t, "field-expiration", metadata.Mode)
			legacyMetadata := util.GetKMetadata(t, sourceClient, ctx, legacyKey)
			require.Equal(t, "legacy", legacyMetadata.Mode)
			if phase == "snapshot" {
				require.Equal(t, int64(6), metadata.Size)
				require.Equal(t, int64(1), metadata.Persist)
				require.NoError(t, sourceClient.Do(ctx, "clusterx", "migrate", slot, destinationID).Err())
			}
			waitForMigrateStateInDuration(t, sourceClient, slot, SlotMigrationStateSuccess, time.Minute)
			waitForImportState(t, destinationClient, slot, SlotImportStateSuccess)

			got, err := destinationClient.HGetAll(ctx, key).Result()
			require.NoError(t, err)
			require.Equal(t, values, got)
			fieldExpires, err := destinationClient.Do(ctx, "hpexpiretime", key, "FIELDS", 6,
				"persistent", "live", "persist", "overwrite", "deleted", "expired").Result()
			require.NoError(t, err)
			require.Equal(t, expires, fieldExpires)
			require.Equal(t, metadata, util.GetKMetadata(t, destinationClient, ctx, key))
			require.Equal(t, legacyMetadata, util.GetKMetadata(t, destinationClient, ctx, legacyKey))
			legacyValue, err := destinationClient.HGet(ctx, legacyKey, "field").Result()
			require.NoError(t, err)
			if phase == "wal" {
				require.Equal(t, binaryValue+"updated", legacyValue)
			} else {
				require.Equal(t, binaryValue, legacyValue)
			}
			require.Equal(t, expiringMetadata, util.GetKMetadata(t, destinationClient, ctx, expiringKey))
			// The destination WAL proves receipt even if the field expired before the first read.
			requireMigratedHashFieldWrite(t, destinationClient, migrationSequence,
				expiringKey, expiringField, expiringValue, fieldExpireAt)
			requireMigratedHashFieldState(t, destinationClient, expiringKey, expiringField, expiringValue, fieldExpireAt)
			newValue, err := destinationClient.HGetAll(ctx, expiringKey).Result()
			require.NoError(t, err)
			delete(newValue, expiringField)
			require.Equal(t, map[string]string{"field": "created-for-migration"}, newValue)
			newExpiry, err := destinationClient.Do(ctx, "hpexpiretime", expiringKey, "FIELDS", 1, "field").Result()
			require.NoError(t, err)
			require.Equal(t, []interface{}{expireAt}, newExpiry)

			// Writes must use the migrated mode even though the destination defaults to legacy encoding.
			wantLive := int64(11)
			if phase == "wal" {
				wantLive = 16
			}
			newLive, err := destinationClient.HIncrBy(ctx, key, "live", 1).Result()
			require.NoError(t, err)
			require.Equal(t, wantLive, newLive)
			values["live"] = fmt.Sprint(wantLive)
			liveValue, err := destinationClient.HGet(ctx, key, "live").Result()
			require.NoError(t, err)
			require.Equal(t, values["live"], liveValue)
			fieldExpires, err = destinationClient.Do(ctx, "hpexpiretime", key, "FIELDS", 1, "live").Result()
			require.NoError(t, err)
			require.Equal(t, []interface{}{expires[1]}, fieldExpires)

			require.Eventually(t, func() bool {
				return errors.Is(destinationClient.HGet(ctx, expiringKey, expiringField).Err(), redis.Nil)
			}, 10*time.Second, 10*time.Millisecond)
			require.GreaterOrEqual(t, time.Now().UnixMilli(), fieldExpireAt)
			got, err = destinationClient.HGetAll(ctx, key).Result()
			require.NoError(t, err)
			require.Equal(t, values, got)
			require.ErrorIs(t, destinationClient.HGet(ctx, expiringKey, expiringField).Err(), redis.Nil)
			newValue, err = destinationClient.HGetAll(ctx, expiringKey).Result()
			require.NoError(t, err)
			require.Equal(t, map[string]string{"field": "created-for-migration"}, newValue)
		})
	}
}

func requireMigratedHashFieldWrite(t *testing.T, client *redis.Client, sequence int64, key, field, value string, expireAt int64) {
	t.Helper()
	ctx := context.Background()
	found := false
	var receivedValue string
	receivedExpiry := int64(-1)
	for {
		result, err := client.Do(ctx, "POLLUPDATES", sequence, "MAX", 1000, "FORMAT", "RESP").Result()
		require.NoError(t, err)
		batch := result.(map[any]any)
		updates := batch["updates"].([]any)
		for i := 0; i < len(updates); i += 2 {
			if updates[i] != "default" {
				continue
			}
			for _, entry := range updates[i+1].([]any) {
				command := entry.([]any)
				if len(command) < 2 || command[1] != key {
					continue
				}
				switch command[0] {
				case "HSET":
					if command[2] == field {
						found, receivedValue, receivedExpiry = true, command[3].(string), -1
					}
				case "HPEXPIREAT":
					if command[5] == field {
						receivedExpiry, err = strconv.ParseInt(command[2].(string), 10, 64)
						require.NoError(t, err)
					}
				case "HDEL":
					if command[2] == field {
						found = false
					}
				case "DEL":
					found = false
				}
			}
		}
		next := batch["next_sequence"].(int64)
		if next == batch["latest_sequence"].(int64) {
			break
		}
		require.Greater(t, next, sequence, "migration WAL pagination must make progress")
		sequence = next
	}
	require.True(t, found, "destination WAL must contain the migrated field")
	require.Equal(t, value, receivedValue)
	require.Equal(t, expireAt, receivedExpiry)
}

func requireMigratedHashFieldState(t *testing.T, client *redis.Client, key, field, value string, expireAt int64) {
	t.Helper()
	ctx := context.Background()
	before, err := client.Time(ctx).Result()
	require.NoError(t, err)
	got, valueErr := client.HGet(ctx, key, field).Result()
	expires, err := client.Do(ctx, "HPEXPIRETIME", key, "FIELDS", 1, field).Int64Slice()
	require.NoError(t, err)
	after, err := client.Time(ctx).Result()
	require.NoError(t, err)
	if after.UnixMilli() <= expireAt {
		require.NoError(t, valueErr)
		require.Equal(t, value, got)
		require.Equal(t, []int64{expireAt}, expires)
	} else if before.UnixMilli() > expireAt {
		require.ErrorIs(t, valueErr, redis.Nil)
		require.Equal(t, []int64{-2}, expires)
	} else {
		// Reads spanning the deadline may see either state; WAL receipt is checked separately.
		if !errors.Is(valueErr, redis.Nil) {
			require.NoError(t, valueErr)
			require.Equal(t, value, got)
		}
		require.Contains(t, [][]int64{{expireAt}, {-2}}, expires)
	}
}

func TestSlotMigrateSnapshotHashEncodingInWAL(t *testing.T) {
	for _, mode := range []string{"legacy", "field-expiration"} {
		ctx := context.Background()
		source := util.StartServer(t, util.KvrocksServerConfigs{
			"cluster-enabled":       "yes",
			"hash-encoding-mode":    mode,
			"migrate-batch-size-kb": "16",
		})
		defer source.Close()
		src := source.NewClient()
		defer func() { require.NoError(t, src.Close()) }()
		destination := util.StartServer(t, util.KvrocksServerConfigs{
			"cluster-enabled":    "yes",
			"hash-encoding-mode": "legacy",
		})
		defer destination.Close()
		dst := destination.NewClient()
		defer func() { require.NoError(t, dst.Close()) }()

		sourceID, destinationID := strings.Repeat("a", 40), strings.Repeat("b", 40)
		require.NoError(t, src.Do(ctx, "CLUSTERX", "SETNODEID", sourceID).Err())
		require.NoError(t, dst.Do(ctx, "CLUSTERX", "SETNODEID", destinationID).Err())
		nodes := fmt.Sprintf("%s %s %d master - 0-10000\n%s %s %d master - 10001-16383",
			sourceID, source.Host(), source.Port(), destinationID, destination.Host(), destination.Port())
		require.NoError(t, src.Do(ctx, "CLUSTERX", "SETNODES", nodes, "1").Err())
		require.NoError(t, dst.Do(ctx, "CLUSTERX", "SETNODES", nodes, "1").Err())

		t.Run(mode, func(t *testing.T) {
			// A single small hash keeps this test independent of migration batch splitting and merging.
			key := fmt.Sprintf("hash{%s}", util.SlotTable[0])
			binaryValue := strings.Repeat("\x00", 8) + "value\x00\xff"
			values := map[string]string{"live": "v", "persistent": binaryValue}
			require.NoError(t, src.HSet(ctx, key, values).Err())
			wantCommands := []any{[]any{"HSET", key, "live", "v"}}
			var expireAt int64 = -1
			if mode == "field-expiration" {
				expireAt = time.Now().Add(10 * time.Minute).UnixMilli()
				result, err := src.Do(ctx, "HPEXPIREAT", key, expireAt, "FIELDS", 1, "live").Int64Slice()
				require.NoError(t, err)
				require.Equal(t, []int64{1}, result)
				wantCommands = append(wantCommands, []any{"HPEXPIREAT", key, strconv.FormatInt(expireAt, 10), "FIELDS", "1", "live"})
				require.NoError(t, src.ConfigSet(ctx, "hash-encoding-mode", "legacy").Err())
			} else {
				require.NoError(t, src.ConfigSet(ctx, "hash-encoding-mode", "field-expiration").Err())
			}
			wantCommands = append(wantCommands, []any{"HSET", key, "persistent", binaryValue})

			require.NoError(t, src.Do(ctx, "CLUSTERX", "MIGRATE", 0, destinationID).Err())
			waitForMigrateState(t, src, 0, SlotMigrationStateSuccess)
			waitForImportState(t, dst, 0, SlotImportStateSuccess)
			got, err := dst.HGetAll(ctx, key).Result()
			require.NoError(t, err)
			require.Equal(t, values, got)
			if mode == "field-expiration" {
				expires, err := dst.Do(ctx, "HPEXPIRETIME", key, "FIELDS", 2, "live", "persistent").Int64Slice()
				require.NoError(t, err)
				require.Equal(t, []int64{expireAt, -1}, expires)
			}

			// Raw KV correctness alone does not ensure that downstream WAL consumers decode the fields correctly.
			result, err := dst.Do(ctx, "POLLUPDATES", 0, "MAX", 1000, "FORMAT", "RESP").Result()
			require.NoError(t, err)
			batch := result.(map[any]any)
			require.Equal(t, batch["latest_sequence"], batch["next_sequence"])
			require.Equal(t, []any{"default", wantCommands}, batch["updates"])
		})
	}
}
