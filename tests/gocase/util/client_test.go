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

package util

import (
	"context"
	"fmt"
	"io"
	"net"
	"testing"
	"time"

	"github.com/stretchr/testify/require"
)

func TestSimpleTCPProxyStopsAcceptingAfterCancel(t *testing.T) {
	t.Parallel()

	upstream, err := net.Listen("tcp", "127.0.0.1:0")
	require.NoError(t, err)
	defer upstream.Close()

	go func() {
		for {
			conn, err := upstream.Accept()
			if err != nil {
				return
			}
			go func(conn net.Conn) {
				defer conn.Close()
				_, _ = io.Copy(conn, conn)
			}(conn)
		}
	}()

	ctx, cancel := context.WithCancel(context.Background())
	proxyPort := SimpleTCPProxy(ctx, t, upstream.Addr().String(), false)
	proxyAddr := fmt.Sprintf("127.0.0.1:%d", proxyPort)

	conn, err := net.DialTimeout("tcp", proxyAddr, time.Second)
	require.NoError(t, err)
	defer conn.Close()

	_, err = conn.Write([]byte("ping"))
	require.NoError(t, err)

	reply := make([]byte, 4)
	_, err = io.ReadFull(conn, reply)
	require.NoError(t, err)
	require.Equal(t, "ping", string(reply))

	cancel()

	require.Eventually(t, func() bool {
		conn, err := net.DialTimeout("tcp", proxyAddr, 100*time.Millisecond)
		if err == nil {
			_ = conn.Close()
			return false
		}
		return true
	}, 2*time.Second, 50*time.Millisecond)
}
