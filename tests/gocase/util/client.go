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
	"errors"
	"fmt"
	"io"
	"net"
	"regexp"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/redis/go-redis/v9"
	"github.com/stretchr/testify/require"
	"golang.org/x/sync/errgroup"
)

func FindInfoEntry(rdb *redis.Client, key string, section ...string) string {
	r := rdb.Info(context.Background(), section...)
	p := regexp.MustCompile(fmt.Sprintf("%s:(.+)", key))
	ms := p.FindStringSubmatch(r.Val())
	if len(ms) != 2 {
		return ""
	}
	return strings.TrimSpace(ms[1])
}

func WaitForSync(t testing.TB, slave *redis.Client) {
	require.Eventually(t, func() bool {
		r := FindInfoEntry(slave, "master_link_status")
		return r == "up"
	}, 5*time.Second, 100*time.Millisecond)
}

func WaitForOffsetSync(t testing.TB, master, slave *redis.Client, waitFor time.Duration) {
	require.Eventually(t, func() bool {
		o1 := FindInfoEntry(master, "master_repl_offset")
		o2 := FindInfoEntry(slave, "master_repl_offset")
		return o1 == o2
	}, waitFor, 100*time.Millisecond)
}

func SlaveOf(t testing.TB, slave *redis.Client, master *KvrocksServer) {
	require.NoError(t, slave.SlaveOf(context.Background(), master.Host(), fmt.Sprintf("%d", master.Port())).Err())
}

func Populate(t testing.TB, rdb *redis.Client, prefix string, n, size int) {
	ctx := context.Background()
	p := rdb.Pipeline()

	for i := 0; i < n; i++ {
		p.Do(ctx, "SET", fmt.Sprintf("%s%d", prefix, i), strings.Repeat("A", size))
	}

	_, err := p.Exec(ctx)
	require.NoError(t, err)
}

func SimpleTCPProxy(ctx context.Context, t testing.TB, to string, slowdown bool) uint64 {
	addr, err := findFreePort()
	if err != nil {
		t.Fatalf("can't find a free port, %v", err)
	}
	from := addr.String()

	listener, err := net.Listen("tcp", from)
	if err != nil {
		t.Fatalf("listen to %s failed, err: %v", from, err)
	}

	closeWhenDone := func(conn net.Conn, done <-chan struct{}) {
		select {
		case <-ctx.Done():
			_ = conn.Close()
		case <-done:
		}
	}

	copyBytes := func(src net.Conn, dest net.Conn) func() error {
		buffer := make([]byte, 4096)
		return func() error {
		COPY_LOOP:
			for {
				if ctx.Err() != nil {
					break COPY_LOOP
				}
				if slowdown {
					time.Sleep(time.Millisecond * 100)
				}
				_ = src.SetReadDeadline(time.Now().Add(200 * time.Millisecond))
				n, err := src.Read(buffer)
				if err != nil {
					if errors.Is(err, io.EOF) || errors.Is(err, net.ErrClosed) {
						break COPY_LOOP
					}
					if netErr, ok := err.(net.Error); ok && netErr.Timeout() {
						continue
					}
					if ctx.Err() != nil {
						break COPY_LOOP
					}
					return err
				}
				_, err = dest.Write(buffer[:n])
				if err != nil {
					if errors.Is(err, io.EOF) || errors.Is(err, net.ErrClosed) {
						break COPY_LOOP
					}
					if ctx.Err() != nil {
						break COPY_LOOP
					}
					return err
				}
			}
			return nil
		}
	}

	go func() {
		var connWG sync.WaitGroup
		defer func() {
			_ = listener.Close()
			connWG.Wait()
		}()
		go func() {
			<-ctx.Done()
			_ = listener.Close()
		}()
	LISTEN_LOOP:
		for {
			if tcpListener, ok := listener.(*net.TCPListener); ok {
				_ = tcpListener.SetDeadline(time.Now().Add(100 * time.Millisecond))
			}
			conn, err := listener.Accept()
			if err != nil {
				if errors.Is(err, net.ErrClosed) || ctx.Err() != nil {
					break LISTEN_LOOP
				}
				if netErr, ok := err.(net.Error); ok && netErr.Timeout() {
					continue
				}
				t.Logf("accept conn failed, err: %v", err)
				continue
			}
			dest, err := net.Dial("tcp", to)
			if err != nil {
				_ = conn.Close()
				if ctx.Err() != nil {
					break LISTEN_LOOP
				}
				t.Logf("dial to %s failed, err: %v", to, err)
				continue
			}
			connWG.Add(1)
			go func(conn net.Conn, dest net.Conn) {
				defer connWG.Done()
				defer conn.Close()
				defer dest.Close()

				done := make(chan struct{})
				defer close(done)
				go closeWhenDone(conn, done)
				go closeWhenDone(dest, done)

				var errGrp errgroup.Group
				errGrp.Go(copyBytes(conn, dest))
				errGrp.Go(copyBytes(dest, conn))
				if err := errGrp.Wait(); err != nil && ctx.Err() == nil {
					t.Logf("forward tcp stream failed, err: %v", err)
				}
			}(conn, dest)
		}
	}()
	return uint64(addr.Port)
}

// PausableTCPProxy creates a TCP proxy that can be paused/resumed via a channel.
// Send true to pause, false to resume. Returns the proxy port.
// When paused, the proxy stops reading from the source, causing the sender's
// TCP buffer to fill up and eventually blocking writes.
func PausableTCPProxy(ctx context.Context, t testing.TB, to string, pauseCh <-chan bool) uint64 {
	addr, err := findFreePort()
	if err != nil {
		t.Fatalf("can't find a free port, %v", err)
	}
	from := addr.String()

	listener, err := net.Listen("tcp", from)
	if err != nil {
		t.Fatalf("listen to %s failed, err: %v", from, err)
	}

	paused := &atomic.Bool{}

	// Goroutine to handle pause/resume signals
	go func() {
		for {
			select {
			case <-ctx.Done():
				return
			case p := <-pauseCh:
				paused.Store(p)
			}
		}
	}()

	copyBytes := func(src, dest io.ReadWriter, direction string) func() error {
		buffer := make([]byte, 4096)
		return func() error {
		COPY_LOOP:
			for {
				select {
				case <-ctx.Done():
					t.Log("forwarding tcp stream stopped")
					break COPY_LOOP
				default:
					// When paused, only block reading from the master (to slave direction)
					// This causes master's send buffer to fill, eventually blocking master's writes
					if paused.Load() && direction == "to_slave" {
						time.Sleep(time.Millisecond * 100)
						continue
					}

					// Set read deadline to allow checking pause state periodically
					if conn, ok := src.(net.Conn); ok {
						_ = conn.SetReadDeadline(time.Now().Add(200 * time.Millisecond))
					}

					n, err := src.Read(buffer)
					if err != nil {
						if errors.Is(err, io.EOF) {
							break COPY_LOOP
						}
						if netErr, ok := err.(net.Error); ok && netErr.Timeout() {
							continue
						}
						return err
					}
					_, err = dest.Write(buffer[:n])
					if err != nil {
						if errors.Is(err, io.EOF) {
							break COPY_LOOP
						}
						return err
					}
				}
			}
			return nil
		}
	}

	go func() {
		defer listener.Close()
	LISTEN_LOOP:
		for {
			select {
			case <-ctx.Done():
				break LISTEN_LOOP

			default:
				_ = listener.(*net.TCPListener).SetDeadline(time.Now().Add(100 * time.Millisecond))
				conn, err := listener.Accept()
				if err != nil {
					if netErr, ok := err.(net.Error); ok && netErr.Timeout() {
						continue
					}
					t.Logf("accept conn failed, err: %v", err)
					continue
				}
				dest, err := net.Dial("tcp", to)
				if err != nil {
					t.Logf("dial to %s failed, err: %v", to, err)
					conn.Close()
					continue
				}
				go func() {
					var errGrp errgroup.Group
					// conn is from slave, dest is to master
					// "to_slave" = reading from master (dest), writing to slave (conn)
					// "to_master" = reading from slave (conn), writing to master (dest)
					errGrp.Go(copyBytes(dest, conn, "to_slave"))
					errGrp.Go(copyBytes(conn, dest, "to_master"))
					_ = errGrp.Wait()
					conn.Close()
					dest.Close()
				}()
			}
		}
	}()
	return uint64(addr.Port)
}
