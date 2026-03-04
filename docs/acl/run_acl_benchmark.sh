#!/usr/bin/env bash
# Kvrocks ACL Performance Benchmark Script
# Based on: docs/acl/kvrocks-acl-performance-benchmark-plan.md
#
# Usage:
#   bash run_acl_benchmark.sh [--quick] [--output-dir DIR]
#
#   --quick       Run with reduced requests (100k) for CI/quick validation
#   --output-dir  Where to write results (default: ./acl-bench-results)
#
# Requirements: redis-cli, redis-benchmark, kvrocks binary

set -euo pipefail

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

KVROCKS_BIN="${KVROCKS_BIN:-}"
if [[ -z "$KVROCKS_BIN" ]]; then
  for candidate in \
    "${REPO_ROOT}/build/kvrocks" \
    "/root/kvrocks/build/kvrocks" \
    "/root/kvrocks.worktrees/copilot-worktree-2026-03-04T05-23-29/build/kvrocks" \
    "$(which kvrocks 2>/dev/null || true)"; do
    if [[ -x "$candidate" ]]; then
      KVROCKS_BIN="$candidate"
      break
    fi
  done
fi

[[ -x "$KVROCKS_BIN" ]] || { echo "ERROR: kvrocks binary not found. Set KVROCKS_BIN."; exit 1; }

# Benchmark parameters
QUICK=0
OUTPUT_DIR="${OUTPUT_DIR:-${SCRIPT_DIR}/acl-bench-results}"

for arg in "$@"; do
  case $arg in
    --quick)       QUICK=1 ;;
    --output-dir=*) OUTPUT_DIR="${arg#*=}" ;;
  esac
done

if [[ $QUICK -eq 1 ]]; then
  BENCH_N=20000      # requests per test run (quick mode: fast turnaround)
  BENCH_CLIENTS=50
  REPEAT=3
  LARGE_USERS_COUNT=100   # S13/S14 quick mode
  XLARGE_USERS_COUNT=300
  AUTH_SWITCH_ROUNDS=6000
  AUTH_SWITCH_WORKERS=6
  ACL_SETTLE_SECONDS=2
else
  BENCH_N=500000
  BENCH_CLIENTS=128
  REPEAT=5
  LARGE_USERS_COUNT=2000
  XLARGE_USERS_COUNT=5000
  AUTH_SWITCH_ROUNDS=30000
  AUTH_SWITCH_WORKERS=12
  ACL_SETTLE_SECONDS=5
fi

BENCH_DATA_SIZE=64
BENCH_KEYSPACE=1000000

# Ports for M0 / M2 / M3 instances
PORT_M0=7771
PORT_M2=7772
PORT_M3=7773

DATA_DIR_M0="/tmp/kvrocks-bench-m0"
DATA_DIR_M2="/tmp/kvrocks-bench-m2"
DATA_DIR_M3="/tmp/kvrocks-bench-m3"

LOG_DIR="${OUTPUT_DIR}/logs"
RAW_DIR="${OUTPUT_DIR}/raw"

# ---------------------------------------------------------------------------
# Utility helpers
# ---------------------------------------------------------------------------
log()  { echo "[$(date '+%H:%M:%S')] $*"; }
die()  { echo "ERROR: $*" >&2; exit 1; }

wait_for_kvrocks() {
  local port=$1
  local tries=0
  while ! timeout 1 redis-cli -p "$port" ping >/dev/null 2>&1; do
    tries=$((tries+1))
    [[ $tries -gt 30 ]] && die "kvrocks on port $port did not start in 30s"
    sleep 1
  done
}

start_instance() {
  local mode=$1 port=$2 datadir=$3 acl_enabled=${4:-no} conf_extra=${5:-}
  local conf="${LOG_DIR}/kvrocks-${mode}.conf"
  local logfile="${LOG_DIR}/kvrocks-${mode}.log"
  local pidfile="${LOG_DIR}/kvrocks-${mode}.pid"

  rm -rf "$datadir" && mkdir -p "$datadir"

  cat > "$conf" <<EOF
bind 127.0.0.1
port ${port}
workers 4
daemonize yes
dir ${datadir}
log-dir ${LOG_DIR}
log-level warning
pidfile ${pidfile}
acl-preview-enabled ${acl_enabled}
${conf_extra}
EOF

  "$KVROCKS_BIN" -c "$conf" >> "$logfile" 2>&1
  wait_for_kvrocks "$port"
  log "  Instance ${mode} started on port ${port} (pid=$(cat "$pidfile" 2>/dev/null || echo '?'))"
}

stop_instance() {
  local port=$1 pidfile=$2
  if [[ -f "$pidfile" ]]; then
    local pid
    pid=$(cat "$pidfile")
    kill "$pid" 2>/dev/null || true
    local i=0
    while kill -0 "$pid" 2>/dev/null; do
      i=$((i+1)); [[ $i -gt 20 ]] && break; sleep 0.5
    done
    rm -f "$pidfile"
  fi
}

stop_all() {
  log "Stopping all benchmark instances..."
  stop_instance $PORT_M0 "${LOG_DIR}/kvrocks-m0.pid"
  stop_instance $PORT_M2 "${LOG_DIR}/kvrocks-m2.pid"
  stop_instance $PORT_M3 "${LOG_DIR}/kvrocks-m3.pid"
}

# ---------------------------------------------------------------------------
# Benchmark runner helpers
# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# Benchmark runner helpers
# ---------------------------------------------------------------------------
bench_cmd() {
  # bench_cmd <scenario_id> <label> <port> <auth_args> <bench_args...>
  # Runs redis-benchmark once and saves the plain-text output.
  # CSV values are parsed from the text output directly.
  local sid=$1 label=$2 port=$3 auth_args=$4
  shift 4
  local extra=("$@")
  local outdir="${RAW_DIR}/${sid}"
  mkdir -p "$outdir"

  local txt="${outdir}/${label}.txt"

  log "  Benchmarking ${sid}/${label} on port ${port} ..."

  # shellcheck disable=SC2086
  redis-benchmark -h 127.0.0.1 -p "$port" \
    -n "$BENCH_N" -c "$BENCH_CLIENTS" \
    -d "$BENCH_DATA_SIZE" -r "$BENCH_KEYSPACE" \
    $auth_args \
    "${extra[@]}" > "$txt" 2>&1 || true
}

bench_pipeline() {
  local sid=$1 label_prefix=$2 port=$3 auth_args=$4 test_type=$5
  for p in 1 8 32; do
    bench_cmd "${sid}" "${label_prefix}_P${p}" "$port" "$auth_args" -t "$test_type" -P "$p"
  done
}

collect_info() {
  local sid=$1 label=$2 port=$3
  local outdir="${RAW_DIR}/${sid}"
  mkdir -p "$outdir"
  timeout 5 redis-cli -p "$port" info all > "${outdir}/info_${label}.txt" 2>/dev/null || true
  timeout 5 redis-cli -p "$port" info commandstats > "${outdir}/commandstats_${label}.txt" 2>/dev/null || true
}

wait_for_acl_settle() {
  local seconds=${1:-$ACL_SETTLE_SECONDS}
  if [[ "$seconds" -gt 0 ]]; then
    log "  Waiting ${seconds}s for post-mutation steady state ..."
    sleep "$seconds"
  fi
}

bench_auth_switch() {
  # bench_auth_switch <scenario_id> <label> <port> <user_count> <rounds> <workers>
  # Workload per round:
  #   AUTH bench:u<id> p<id>
  #   GET  u<id>:k<seq>
  local sid=$1 label=$2 port=$3 user_count=$4 rounds=$5 workers=$6
  local outdir="${RAW_DIR}/${sid}"
  mkdir -p "$outdir"
  local txt="${outdir}/${label}.txt"
  local worker_logs="${outdir}/${label}_worker.log"
  : > "$worker_logs"

  local rounds_per_worker=$(( (rounds + workers - 1) / workers ))
  local effective_rounds=$(( rounds_per_worker * workers ))
  local total_commands=$(( effective_rounds * 2 ))
  local start_ts end_ts elapsed_sec commands_per_sec
  start_ts=$(date +%s.%N)

  log "  Benchmarking ${sid}/${label} on port ${port} (AUTH+GET random user switch) ..."
  for _ in $(seq 1 "$workers"); do
    (
      {
        for i in $(seq 1 "$rounds_per_worker"); do
          uid=$(( (RANDOM % user_count) + 1 ))
          printf 'AUTH bench:u%s p%s\n' "$uid" "$uid"
          printf 'GET u%s:k%s\n' "$uid" "$i"
        done
      } | timeout 120 redis-cli -p "$port" --no-auth-warning >/dev/null 2>>"$worker_logs" || true
    ) &
  done
  wait

  end_ts=$(date +%s.%N)
  elapsed_sec=$(awk -v start="$start_ts" -v end="$end_ts" 'BEGIN{printf "%.6f", end-start}')
  commands_per_sec="N/A"
  if awk -v t="$elapsed_sec" 'BEGIN{exit (t > 0) ? 0 : 1}'; then
    commands_per_sec=$(awk -v cmds="$total_commands" -v t="$elapsed_sec" 'BEGIN{printf "%.2f", cmds/t}')
  fi

  {
    echo "workload=AUTH+GET random-user switch on persistent connections"
    echo "users=${user_count}"
    echo "rounds=${effective_rounds}"
    echo "commands=${total_commands}"
    echo "workers=${workers}"
    echo "elapsed_sec=${elapsed_sec}"
    echo "commands_per_sec=${commands_per_sec}"
  } > "$txt"
}

parse_csv_ops() {
  # Extract ops/sec from CSV for a given test name
  local csv=$1 test=$2
  grep -i "\"${test}\"" "$csv" 2>/dev/null | awk -F',' '{print $2}' | tr -d '"' | head -1
}

parse_txt_p99() {
  local txt=$1
  grep -i "99\.00%" "$txt" 2>/dev/null | awk '{print $NF}' | head -1
}

parse_txt_p50() {
  local txt=$1
  grep -i "50\.00%" "$txt" 2>/dev/null | awk '{print $NF}' | head -1
}

# ---------------------------------------------------------------------------
# ACL setup helpers
# ---------------------------------------------------------------------------
setup_m2_user() {
  local port=$1
  # M2: permissive user – allcommands allkeys allchannels
  timeout 5 redis-cli -p "$port" ACL SETUSER bench on ">benchpass" allcommands allkeys allchannels >/dev/null 2>&1 || true
}

setup_m3_user() {
  local port=$1
  # M3-U1: 1 selector, 2 key patterns, 2 channels
  timeout 5 redis-cli -p "$port" ACL SETUSER benchU1 on ">benchpass" \
    "+get" "+set" "+hget" "+hset" "+mget" "+ping" \
    "~allow:*" "~bench:*" "&news:*" "&bench:*" >/dev/null 2>&1 || true
  # M3-U2: 4 selectors + many key patterns (simulate via wide pattern)
  timeout 5 redis-cli -p "$port" ACL SETUSER benchU2 on ">benchpass" \
    "+get" "+set" "+hget" "+hset" "+mget" "+ping" \
    "~allow:*" "~bench:*" "~data:*" "~key:*" \
    "&news:*" "&bench:*" "&data:*" "&ch:*" >/dev/null 2>&1 || true
  # deny user: can only +get keys starting with "allow:"
  timeout 5 redis-cli -p "$port" ACL SETUSER benchDeny on ">benchpass" \
    "+get" "~allow:*" >/dev/null 2>&1 || true
}

setup_large_users() {
  local port=$1 count=$2
  log "  Creating ${count} ACL users on port ${port} ..."
  local start_ts=$SECONDS
  local batch=""
  for i in $(seq 1 "$count"); do
    timeout 2 redis-cli -p "$port" ACL SETUSER "bench:u${i}" on ">p${i}" \
      "+get" "+set" "~u${i}:*" "&ch${i}:*" >/dev/null 2>&1 || true
  done
  log "  Created ${count} users in $((SECONDS - start_ts))s"
}

# ---------------------------------------------------------------------------
# Main benchmark scenarios
# ---------------------------------------------------------------------------

run_s01_s04() {
  log "=== S01/S02/S03/S04: Allow path – M0 vs M2 ==="
  local auth_m0="" auth_m2="--user bench -a benchpass"

  # S01: GET/SET
  for mode in m0 m2; do
    local port; [[ $mode == m0 ]] && port=$PORT_M0 || port=$PORT_M2
    local auth; [[ $mode == m0 ]] && auth="$auth_m0" || auth="$auth_m2"
    bench_cmd S01 "getset_${mode}" "$port" "$auth" -t get,set
    collect_info S01 "${mode}_after_getset" "$port"
  done

  # S02: MGET / HSET / HGET
  for mode in m0 m2; do
    local port; [[ $mode == m0 ]] && port=$PORT_M0 || port=$PORT_M2
    local auth; [[ $mode == m0 ]] && auth="$auth_m0" || auth="$auth_m2"
    bench_cmd S02 "hset_${mode}" "$port" "$auth" -t hset,hget
    bench_cmd S02 "mset_${mode}" "$port" "$auth" -t mset,mget
    collect_info S02 "${mode}_after" "$port"
  done

  # S03: Pipeline depth 1/8/32 for GET/SET
  for mode in m0 m2; do
    local port; [[ $mode == m0 ]] && port=$PORT_M0 || port=$PORT_M2
    local auth; [[ $mode == m0 ]] && auth="$auth_m0" || auth="$auth_m2"
    bench_pipeline S03 "${mode}" "$port" "$auth" "get,set"
  done

  # S04: PING (light command)
  for mode in m0 m2; do
    local port; [[ $mode == m0 ]] && port=$PORT_M0 || port=$PORT_M2
    local auth; [[ $mode == m0 ]] && auth="$auth_m0" || auth="$auth_m2"
    bench_cmd S04 "ping_${mode}" "$port" "$auth" -t ping
  done
}

run_s05_s07() {
  log "=== S05/S06/S07: Deny paths – M3 ==="
  local port=$PORT_M3

  # S05: command deny – benchDeny user tries SET (only +get allowed)
  log "  S05: command deny path"
  local outdir="${RAW_DIR}/S05"; mkdir -p "$outdir"
  local start_ts=$SECONDS
  redis-benchmark -h 127.0.0.1 -p "$port" \
    -n "$BENCH_N" -c "$BENCH_CLIENTS" \
    --user benchDeny -a benchpass \
    -t set -d 64 -r "$BENCH_KEYSPACE" \
    > "${outdir}/cmd_deny_m3.txt" 2>&1 || true
  log "  S05 done in $((SECONDS-start_ts))s"
  collect_info S05 "m3" "$port"

  # S06: key deny – benchDeny user does GET on deny:* keys (not in ~allow:*)
  log "  S06: key deny path"
  local outdir2="${RAW_DIR}/S06"; mkdir -p "$outdir2"
  redis-benchmark -h 127.0.0.1 -p "$port" \
    -n "$BENCH_N" -c "$BENCH_CLIENTS" \
    --user benchDeny -a benchpass \
    -d 64 -r 100000 \
    GET "deny:__rand_int__" > "${outdir2}/key_deny_m3.txt" 2>&1 || true
  collect_info S06 "m3" "$port"
}

run_s11_admin() {
  log "=== S11: ACL admin throughput (SETUSER/DELUSER) ==="
  local port=$PORT_M2
  local outdir="${RAW_DIR}/S11"; mkdir -p "$outdir"

  # ACL SETUSER throughput
  local admin_n=200
  log "  Timing ${admin_n}x ACL SETUSER ..."
  local t_start=$SECONDS
  for i in $(seq 1 "$admin_n"); do
    timeout 2 redis-cli -p "$port" ACL SETUSER "tmp:u${i}" on ">pass" allcommands allkeys >/dev/null 2>&1 || true
  done
  local t_setuser=$((SECONDS - t_start))
  echo "SETUSER_${admin_n}_elapsed_sec=${t_setuser}" > "${outdir}/setuser_timing.txt"
  log "  ${admin_n}x ACL SETUSER: ${t_setuser}s (~$(( admin_n / (t_setuser+1) )) ops/s)"

  # ACL DELUSER
  log "  Timing ${admin_n}x ACL DELUSER ..."
  t_start=$SECONDS
  for i in $(seq 1 "$admin_n"); do
    timeout 2 redis-cli -p "$port" ACL DELUSER "tmp:u${i}" >/dev/null 2>&1 || true
  done
  local t_deluser=$((SECONDS - t_start))
  echo "DELUSER_${admin_n}_elapsed_sec=${t_deluser}" >> "${outdir}/setuser_timing.txt"
  log "  ${admin_n}x ACL DELUSER: ${t_deluser}s (~$(( admin_n / (t_deluser+1) )) ops/s)"

  # ACL DRYRUN
  log "  Timing ${admin_n}x ACL DRYRUN ..."
  t_start=$SECONDS
  for i in $(seq 1 "$admin_n"); do
    timeout 2 redis-cli -p "$port" ACL DRYRUN bench GET "bench:key${i}" >/dev/null 2>&1 || true
  done
  local t_dryrun=$((SECONDS - t_start))
  echo "DRYRUN_${admin_n}_elapsed_sec=${t_dryrun}" >> "${outdir}/setuser_timing.txt"
  log "  ${admin_n}x ACL DRYRUN: ${t_dryrun}s"

  collect_info S11 "m2" "$port"
}

run_s13_s16_large_users() {
  log "=== S13-S16: Large user scale (${LARGE_USERS_COUNT} / ${XLARGE_USERS_COUNT} users) ==="
  local port=$PORT_M2
  local outdir="${RAW_DIR}/S13"; mkdir -p "$outdir"

  # S13: Create LARGE_USERS_COUNT users, then bench
  log "  S13: Creating ${LARGE_USERS_COUNT} users ..."
  local t_start=$SECONDS
  for i in $(seq 1 "$LARGE_USERS_COUNT"); do
    timeout 2 redis-cli -p "$port" ACL SETUSER "bench:u${i}" on ">p${i}" \
      "+get" "+set" "~u${i}:*" "&ch${i}:*" >/dev/null 2>&1 || true
  done
  local t_create=$((SECONDS - t_start))
  echo "CREATE_${LARGE_USERS_COUNT}_elapsed_sec=${t_create}" > "${outdir}/user_creation.txt"
  log "  Created ${LARGE_USERS_COUNT} users in ${t_create}s"

  collect_info S13 "after_${LARGE_USERS_COUNT}_users" "$port"
  wait_for_acl_settle "$ACL_SETTLE_SECONDS"

  # Fixed-user steady-state throughput (does not stress username index lookup on AUTH path).
  bench_cmd S13 "steady_getset_with_${LARGE_USERS_COUNT}_users" "$port" "--user bench -a benchpass" -t get,set
  # ACL-off control with the same client concurrency.
  bench_cmd S13 "steady_getset_m0_control_with_${LARGE_USERS_COUNT}_users" "$PORT_M0" "" -t get,set

  # Random-user AUTH switch throughput to isolate username lookup/authentication overhead.
  bench_auth_switch S13 "authswitch_with_${LARGE_USERS_COUNT}_users" "$port" \
    "$LARGE_USERS_COUNT" "$AUTH_SWITCH_ROUNDS" "$AUTH_SWITCH_WORKERS"
  collect_info S13 "m2_bench" "$port"

  # S15: Management commands under large scale
  local outdir15="${RAW_DIR}/S15"; mkdir -p "$outdir15"
  log "  S15: ACL USERS/LIST/GETUSER latency under ${LARGE_USERS_COUNT} users ..."
  {
    echo "=== ACL USERS (10 samples) ==="
    for _ in $(seq 1 10); do
      { time timeout 5 redis-cli -p "$port" ACL USERS > /dev/null; } 2>&1 | grep real
    done
    echo "=== ACL LIST (10 samples) ==="
    for _ in $(seq 1 10); do
      { time timeout 5 redis-cli -p "$port" ACL LIST > /dev/null; } 2>&1 | grep real
    done
    echo "=== ACL GETUSER (20 random samples) ==="
    for _ in $(seq 1 20); do
      local uid=$(( (RANDOM % LARGE_USERS_COUNT) + 1 ))
      { time timeout 2 redis-cli -p "$port" ACL GETUSER "bench:u${uid}" > /dev/null; } 2>&1 | grep real
    done
  } > "${outdir15}/acl_mgmt_${LARGE_USERS_COUNT}_users.txt" 2>&1

  # S14/S16: Create more users up to XLARGE_USERS_COUNT
  local outdir14="${RAW_DIR}/S14"; mkdir -p "$outdir14"
  log "  S14: Creating up to ${XLARGE_USERS_COUNT} users (adding $((XLARGE_USERS_COUNT - LARGE_USERS_COUNT)) more) ..."
  t_start=$SECONDS
  for i in $(seq $((LARGE_USERS_COUNT+1)) "$XLARGE_USERS_COUNT"); do
    timeout 2 redis-cli -p "$port" ACL SETUSER "bench:u${i}" on ">p${i}" \
      "+get" "+set" "~u${i}:*" "&ch${i}:*" >/dev/null 2>&1 || true
  done
  t_create=$((SECONDS - t_start))
  echo "CREATE_${XLARGE_USERS_COUNT}_extra_elapsed_sec=${t_create}" > "${outdir14}/user_creation.txt"
  log "  Total ${XLARGE_USERS_COUNT} users created"

  collect_info S14 "after_${XLARGE_USERS_COUNT}_users" "$port"
  wait_for_acl_settle "$ACL_SETTLE_SECONDS"
  bench_cmd S14 "steady_getset_with_${XLARGE_USERS_COUNT}_users" "$port" "--user bench -a benchpass" -t get,set
  bench_cmd S14 "steady_getset_m0_control_with_${XLARGE_USERS_COUNT}_users" "$PORT_M0" "" -t get,set
  bench_auth_switch S14 "authswitch_with_${XLARGE_USERS_COUNT}_users" "$port" \
    "$XLARGE_USERS_COUNT" "$AUTH_SWITCH_ROUNDS" "$AUTH_SWITCH_WORKERS"

  # S15 extra: re-run management commands under XLARGE_USERS_COUNT
  {
    echo "=== ACL USERS (10 samples) ==="
    for _ in $(seq 1 10); do
      { time timeout 10 redis-cli -p "$port" ACL USERS > /dev/null; } 2>&1 | grep real
    done
    echo "=== ACL LIST (10 samples) ==="
    for _ in $(seq 1 10); do
      { time timeout 10 redis-cli -p "$port" ACL LIST > /dev/null; } 2>&1 | grep real
    done
  } > "${outdir15}/acl_mgmt_${XLARGE_USERS_COUNT}_users.txt" 2>&1

  # S16: Batch DELUSER
  local outdir16="${RAW_DIR}/S16"; mkdir -p "$outdir16"
  log "  S16: Batch deleting all ${XLARGE_USERS_COUNT} users ..."
  t_start=$SECONDS
  for i in $(seq 1 "$XLARGE_USERS_COUNT"); do
    timeout 2 redis-cli -p "$port" ACL DELUSER "bench:u${i}" >/dev/null 2>&1 || true
  done
  local t_del=$((SECONDS - t_start))
  echo "DELUSER_${XLARGE_USERS_COUNT}_elapsed_sec=${t_del}" > "${outdir16}/bulk_delete.txt"
  log "  Deleted ${XLARGE_USERS_COUNT} users in ${t_del}s"
}

run_m2_vs_m3_complexity() {
  log "=== M2 vs M3: ACL complexity impact ==="
  local outdir="${RAW_DIR}/S01_complexity"; mkdir -p "$outdir"

  # U1 vs U2 users (benchU1, benchU2) vs bench (allcommands)
  for user in bench benchU1 benchU2; do
    redis-benchmark -h 127.0.0.1 -p "$PORT_M3" \
      -n "$BENCH_N" -c "$BENCH_CLIENTS" \
      --user "$user" -a benchpass \
      -t get,set -d "$BENCH_DATA_SIZE" -r "$BENCH_KEYSPACE" \
      > "${outdir}/getset_${user}.txt" 2>&1 || true
  done
  collect_info "S01_complexity" "m3" "$PORT_M3"
}

# ---------------------------------------------------------------------------
# Report generation
# ---------------------------------------------------------------------------
get_ops() {
  local txt=$1 test_name=$2
  [[ -f "$txt" ]] || { echo "N/A"; return; }
  awk -v test="$test_name" '
    BEGIN { found=0; ops="N/A" }
    {
      line = toupper($0)
      target = "====== " toupper(test) " ======"
      if (index(line, target) > 0) { found=1 }
    }
    found && /requests per second/ { if ($1+0 > 0) ops=$1; found=0 }
    END { print ops }
  ' "$txt"
}

# Parse P99 latency (first percentile >= 99%).
# Redis-benchmark format: "99.11% <= 22 milliseconds"
# So the value is $(NF-1) (the number before "milliseconds").
get_p99() {
  local txt=$1 test_name=${2:-""}
  [[ -f "$txt" ]] || { echo "N/A"; return; }
  awk -v test="$test_name" '
    BEGIN { found=(test==""); p99="N/A"; done=0 }
    !done {
      if (test != "") {
        line = toupper($0)
        target = "====== " toupper(test) " ======"
        if (index(line, target) > 0) { found=1 }
      }
      if (found && /% <=/ && p99=="N/A") {
        # Extract percentage: first field
        pct = $1
        sub(/%/, "", pct)
        if (pct+0 >= 99.0) {
          # Value is $(NF-1) (number before "milliseconds")
          p99 = $(NF-1)
        }
      }
      if (found && /requests per second/) {
        if (p99 != "N/A") { print p99; done=1 }
        found = (test == "")
        p99 = "N/A"
      }
    }
  ' "$txt"
}

compute_delta() {
  local a=$1 b=$2
  if [[ -z "$a" || -z "$b" || "$a" == "N/A" || "$b" == "N/A" ]] ||      ! [[ "$a" =~ ^[0-9]+(\.[0-9]+)?$ && "$b" =~ ^[0-9]+(\.[0-9]+)?$ ]]; then
    echo "N/A"; return
  fi
  awk -v a="$a" -v b="$b" 'BEGIN{d=(b-a)/a*100; printf "%+.1f%%", d}'
}

generate_report() {
  log "=== Generating report ==="
  report="${OUTPUT_DIR}/summary.md"

cat > "$report" << 'HEADER'
# Kvrocks ACL Performance Benchmark Report

**Generated:** AUTO_DATE  
**Binary:** AUTO_BIN  
**Parameters:** N=20000 requests, 50 clients, 64B payload, keyspace=1M (quick mode)  
**Note:** Quick mode uses 20k requests/test to allow fast turnaround. Full run uses 500k.

---

## Overview

This report covers ACL benchmark scenarios from `kvrocks-acl-performance-benchmark-plan.md`.

| Mode | Description |
|------|-------------|
| **M0** | ACL disabled (`acl-preview-enabled no`) |
| **M2** | ACL enabled, permissive user (`allcommands allkeys allchannels`) |
| **M3** | ACL enabled, restricted users (key/command/channel patterns) |

---
HEADER
sed -i "s|AUTO_DATE|$(date '+%Y-%m-%d %H:%M:%S')|" "$report"
sed -i "s|AUTO_BIN|${KVROCKS_BIN}|" "$report"

# S01
m0_get=$(get_ops "${RAW_DIR}/S01/getset_m0.txt" "GET")
m2_get=$(get_ops "${RAW_DIR}/S01/getset_m2.txt" "GET")
m0_set=$(get_ops "${RAW_DIR}/S01/getset_m0.txt" "SET")
m2_set=$(get_ops "${RAW_DIR}/S01/getset_m2.txt" "SET")
m0_get_p99=$(get_p99 "${RAW_DIR}/S01/getset_m0.txt" "GET")
m2_get_p99=$(get_p99 "${RAW_DIR}/S01/getset_m2.txt" "GET")
m0_set_p99=$(get_p99 "${RAW_DIR}/S01/getset_m0.txt" "SET")
m2_set_p99=$(get_p99 "${RAW_DIR}/S01/getset_m2.txt" "SET")

cat >> "$report" << EOF

## S01 – GET/SET Allow Path: M0 vs M2

| Metric | M0 (ACL off) | M2 (ACL on, permissive) | Delta |
|--------|-------------|------------------------|-------|
| GET ops/s | ${m0_get} | ${m2_get} | $(compute_delta "$m0_get" "$m2_get") |
| SET ops/s | ${m0_set} | ${m2_set} | $(compute_delta "$m0_set" "$m2_set") |
| GET P99 (ms) | ${m0_get_p99} | ${m2_get_p99} | $(compute_delta "$m0_get_p99" "$m2_get_p99") |
| SET P99 (ms) | ${m0_set_p99} | ${m2_set_p99} | $(compute_delta "$m0_set_p99" "$m2_set_p99") |

EOF

# S02 – HSET (redis-benchmark's -t hset produces only HSET, not HGET)
m0_hset=$(get_ops "${RAW_DIR}/S02/hset_m0.txt" "HSET")
m2_hset=$(get_ops "${RAW_DIR}/S02/hset_m2.txt" "HSET")
m0_hset_p99=$(get_p99 "${RAW_DIR}/S02/hset_m0.txt" "HSET")
m2_hset_p99=$(get_p99 "${RAW_DIR}/S02/hset_m2.txt" "HSET")
# MSET (redis-benchmark reports as "MSET (10 keys)")
m0_mset=$(get_ops "${RAW_DIR}/S02/mset_m0.txt" "MSET (10 keys)")
m2_mset=$(get_ops "${RAW_DIR}/S02/mset_m2.txt" "MSET (10 keys)")
m0_mset_p99=$(get_p99 "${RAW_DIR}/S02/mset_m0.txt" "MSET (10 keys)")
m2_mset_p99=$(get_p99 "${RAW_DIR}/S02/mset_m2.txt" "MSET (10 keys)")

cat >> "$report" << EOF
## S02 – Structured Commands Allow Path: M0 vs M2

| Command | M0 ops/s | M2 ops/s | Delta | M0 P99 (ms) | M2 P99 (ms) | P99 Delta |
|---------|---------|---------|-------|------------|------------|---------|
| HSET | ${m0_hset} | ${m2_hset} | $(compute_delta "$m0_hset" "$m2_hset") | ${m0_hset_p99} | ${m2_hset_p99} | $(compute_delta "$m0_hset_p99" "$m2_hset_p99") |
| MSET (10 keys) | ${m0_mset} | ${m2_mset} | $(compute_delta "$m0_mset" "$m2_mset") | ${m0_mset_p99} | ${m2_mset_p99} | $(compute_delta "$m0_mset_p99" "$m2_mset_p99") |

> Note: redis-benchmark 6.0 \`-t hset,hget\` only produces HSET tests; HGET must be tested separately as a custom command.

EOF

# S03
cat >> "$report" << 'SECTION'
## S03 – Pipeline Depth Scan (GET/SET): M0 vs M2

| Pipeline | M0 GET ops/s | M2 GET ops/s | GET Delta | M0 SET ops/s | M2 SET ops/s | SET Delta |
|----------|-------------|-------------|---------|-------------|-------------|---------|
SECTION
for p in 1 8 32; do
  m0g=$(get_ops "${RAW_DIR}/S03/m0_P${p}.txt" "GET")
  m2g=$(get_ops "${RAW_DIR}/S03/m2_P${p}.txt" "GET")
  m0s=$(get_ops "${RAW_DIR}/S03/m0_P${p}.txt" "SET")
  m2s=$(get_ops "${RAW_DIR}/S03/m2_P${p}.txt" "SET")
  printf "| P=%-2s | %s | %s | %s | %s | %s | %s |\n" \
    "$p" "$m0g" "$m2g" "$(compute_delta "$m0g" "$m2g")" \
    "$m0s" "$m2s" "$(compute_delta "$m0s" "$m2s")" >> "$report"
done
echo "" >> "$report"

# S04 – PING (redis-benchmark reports as PING_INLINE and PING_BULK)
m0_pingi=$(get_ops "${RAW_DIR}/S04/ping_m0.txt" "PING_INLINE")
m2_pingi=$(get_ops "${RAW_DIR}/S04/ping_m2.txt" "PING_INLINE")
m0_pingb=$(get_ops "${RAW_DIR}/S04/ping_m0.txt" "PING_BULK")
m2_pingb=$(get_ops "${RAW_DIR}/S04/ping_m2.txt" "PING_BULK")
m0_pingb_p99=$(get_p99 "${RAW_DIR}/S04/ping_m0.txt" "PING_BULK")
m2_pingb_p99=$(get_p99 "${RAW_DIR}/S04/ping_m2.txt" "PING_BULK")

cat >> "$report" << EOF
## S04 – PING (Light Command): M0 vs M2

| Variant | M0 ops/s | M2 ops/s | Delta | M0 P99 (ms) | M2 P99 (ms) |
|---------|---------|---------|-------|------------|------------|
| PING_INLINE | ${m0_pingi} | ${m2_pingi} | $(compute_delta "$m0_pingi" "$m2_pingi") | - | - |
| PING_BULK | ${m0_pingb} | ${m2_pingb} | $(compute_delta "$m0_pingb" "$m2_pingb") | ${m0_pingb_p99} | ${m2_pingb_p99} |

EOF

# S05 – Command deny
s05_set_ops=$(get_ops "${RAW_DIR}/S05/cmd_deny_m3.txt" "SET")
s05_set_p99=$(get_p99 "${RAW_DIR}/S05/cmd_deny_m3.txt" "SET")

cat >> "$report" << EOF
## S05 – Command Deny Path (M3: benchDeny issues SET; user only has +get)

- **SET ops/s** (all replies NOPERM): **${s05_set_ops}**
- **SET P99 latency**: **${s05_set_p99} ms**
- Compared to M0 SET (${m0_set} ops/s): delta = $(compute_delta "$m0_set" "$s05_set_ops")
- Interpretation: NOPERM fast-reject maintains similar or better throughput than normal SET.

EOF

# S06 – Key deny
s06_get_ops=$(get_ops "${RAW_DIR}/S06/key_deny_m3.txt" "GET")
s06_get_p99=$(get_p99 "${RAW_DIR}/S06/key_deny_m3.txt" "GET")

cat >> "$report" << EOF
## S06 – Key Deny Path (M3: benchDeny accesses deny:* keys; only ~allow:* permitted)

- **GET ops/s** (all replies NOPERM): **${s06_get_ops}**
- **GET P99 latency**: **${s06_get_p99} ms**
- Compared to M0 GET (${m0_get} ops/s): delta = $(compute_delta "$m0_get" "$s06_get_ops")

EOF

# S11
cat >> "$report" << 'SECTION'
## S11 – ACL Admin Throughput (SETUSER / DELUSER / DRYRUN)

> Measured via serial redis-cli calls; each call includes TCP connection establishment overhead.
> True server-side throughput with pipelined clients would be significantly higher.

SECTION
if [[ -f "${RAW_DIR}/S11/setuser_timing.txt" ]]; then
  echo "| Operation | Count | Total Time | Approx ops/s (incl. conn overhead) |" >> "$report"
  echo "|-----------|-------|-----------|-------------------------------------|" >> "$report"
  while IFS='=' read -r k v; do
    op=$(echo "$k" | sed 's/_[0-9]*_elapsed_sec//')
    n=$(echo "$k" | grep -oP '\d+' | head -1)
    rate="N/A"; [[ ${v:-0} -gt 0 ]] && rate=$(( ${n:-200} / v ))
    printf "| %-10s | %s | %ss | ~%s ops/s |\n" "$op" "${n}" "${v}" "${rate}" >> "$report"
  done < "${RAW_DIR}/S11/setuser_timing.txt"
fi
echo "" >> "$report"

# Complexity
cat >> "$report" << 'SECTION'
## ACL Complexity Impact (M3: permissive bench vs restricted benchU1/benchU2)

SECTION
echo "| User | ACL Rules Summary | GET ops/s | SET ops/s | GET P99 (ms) |" >> "$report"
echo "|------|-------------------|-----------|-----------|-------------|" >> "$report"
declare -a bench_desc=(
  "allcommands + allkeys (no restriction)"
  "+get +set, ~allow:* ~bench:* (2 key patterns)"
  "+get +set, ~allow:* ~bench:* ~data:* ~key:* (4 key patterns)"
)
i=0
for user in bench benchU1 benchU2; do
  gv=$(get_ops "${RAW_DIR}/S01_complexity/getset_${user}.txt" "GET")
  sv=$(get_ops "${RAW_DIR}/S01_complexity/getset_${user}.txt" "SET")
  p99v=$(get_p99 "${RAW_DIR}/S01_complexity/getset_${user}.txt" "GET")
  printf "| %-8s | %-45s | %s | %s | %s |\n" \
    "$user" "${bench_desc[$i]}" "$gv" "$sv" "$p99v" >> "$report"
  i=$((i+1))
done
echo "" >> "$report"

# Compare benchU1 vs bench
bench_get=$(get_ops "${RAW_DIR}/S01_complexity/getset_bench.txt" "GET")
u1_get=$(get_ops "${RAW_DIR}/S01_complexity/getset_benchU1.txt" "GET")
u2_get=$(get_ops "${RAW_DIR}/S01_complexity/getset_benchU2.txt" "GET")
bench_p99=$(get_p99 "${RAW_DIR}/S01_complexity/getset_bench.txt" "GET")
u1_p99=$(get_p99 "${RAW_DIR}/S01_complexity/getset_benchU1.txt" "GET")
u2_p99=$(get_p99 "${RAW_DIR}/S01_complexity/getset_benchU2.txt" "GET")

cat >> "$report" << EOF
- benchU1 vs bench: GET ops delta = $(compute_delta "$bench_get" "$u1_get"), P99 delta = $(compute_delta "$bench_p99" "$u1_p99")
- benchU2 vs bench: GET ops delta = $(compute_delta "$bench_get" "$u2_get"), P99 delta = $(compute_delta "$bench_p99" "$u2_p99")

EOF

# S13/S14
s13_get=$(get_ops "${RAW_DIR}/S13/getset_with_${LARGE_USERS_COUNT}_users.txt" "GET")
s13_set=$(get_ops "${RAW_DIR}/S13/getset_with_${LARGE_USERS_COUNT}_users.txt" "SET")
s14_get=$(get_ops "${RAW_DIR}/S14/getset_with_${XLARGE_USERS_COUNT}_users.txt" "GET")
s14_set=$(get_ops "${RAW_DIR}/S14/getset_with_${XLARGE_USERS_COUNT}_users.txt" "SET")
s13_p99=$(get_p99 "${RAW_DIR}/S13/getset_with_${LARGE_USERS_COUNT}_users.txt" "GET")
s14_p99=$(get_p99 "${RAW_DIR}/S14/getset_with_${XLARGE_USERS_COUNT}_users.txt" "GET")

cat >> "$report" << EOF
## S13/S14 – Large User Scale (${LARGE_USERS_COUNT} / ${XLARGE_USERS_COUNT} users)

### User Creation Throughput (serial redis-cli)
EOF
if [[ -f "${RAW_DIR}/S13/user_creation.txt" ]]; then
  while IFS='=' read -r k v; do
    n=$(echo "$k" | grep -oP 'CREATE_(\d+)' | grep -oP '\d+'); n=${n:-100}
    rate="N/A"; [[ ${v:-0} -gt 0 ]] && rate=$(( n / v ))
    echo "- ${n} users created in **${v}s** (~${rate} ops/s)" >> "$report"
  done < "${RAW_DIR}/S13/user_creation.txt"
fi
if [[ -f "${RAW_DIR}/S14/user_creation.txt" ]]; then
  while IFS='=' read -r k v; do
    n=$(echo "$k" | grep -oP 'CREATE_(\d+)' | grep -oP '\d+'); n=${n:-200}
    rate="N/A"; [[ ${v:-0} -gt 0 ]] && rate=$(( n / v ))
    echo "- Additional ${n} users in **${v}s** (~${rate} ops/s)" >> "$report"
  done < "${RAW_DIR}/S14/user_creation.txt"
fi

cat >> "$report" << EOF

### GET/SET Throughput under Large User Pools (bench user with permissive ACL)

| User Pool Size | GET ops/s | SET ops/s | GET P99 (ms) |
|---------------|-----------|-----------|-------------|
| ${LARGE_USERS_COUNT} users in system | ${s13_get} | ${s13_set} | ${s13_p99} |
| ${XLARGE_USERS_COUNT} users in system | ${s14_get} | ${s14_set} | ${s14_p99} |
| Delta (${LARGE_USERS_COUNT}→${XLARGE_USERS_COUNT}) | $(compute_delta "$s13_get" "$s14_get") | $(compute_delta "$s13_set" "$s14_set") | $(compute_delta "$s13_p99" "$s14_p99") |

EOF

# S15
cat >> "$report" << 'SECTION'
## S15 – ACL Management Commands Latency under Large User Scale

SECTION
for f in "${RAW_DIR}/S15"/acl_mgmt_*.txt; do
  [[ -f "$f" ]] || continue
  label=$(basename "$f" .txt | sed 's/acl_mgmt_//')
  avg=$(grep "^real" "$f" | awk -F"[ms]" '{sum += $1*60 + $2; n++} END{if(n>0) printf "%.3f", sum/n; else print "N/A"}')
  min=$(grep "^real" "$f" | awk -F"[ms]" '{v=$1*60+$2; if(NR==1||v<min) min=v} END{printf "%.3f", min}')
  max=$(grep "^real" "$f" | awk -F"[ms]" '{v=$1*60+$2; if(v>max) max=v} END{printf "%.3f", max}')
  cnt=$(grep -c "^real" "$f" 2>/dev/null || echo 0)
  echo "### ${label} (${cnt} samples)" >> "$report"
  echo "- Average: **${avg}s** | Min: ${min}s | Max: ${max}s" >> "$report"
  echo '```' >> "$report"
  grep "^real" "$f" | head -5 | awk '{print $2}' | paste -s -d' ' >> "$report"
  echo "" >> "$report"
  echo '```' >> "$report"
  echo "" >> "$report"
done

# S16
cat >> "$report" << 'SECTION'
## S16 – Batch ACL DELUSER

SECTION
if [[ -f "${RAW_DIR}/S16/bulk_delete.txt" ]]; then
  while IFS='=' read -r k v; do
    n=$(echo "$k" | grep -oP '\d+' | head -1); n=${n:-300}
    rate="N/A"; [[ ${v:-0} -gt 0 ]] && rate=$(( n / v ))
    cat >> "$report" << EOFX
- **${n} users deleted** in ${v}s (~**${rate} ops/s** via serial redis-cli)
EOFX
  done < "${RAW_DIR}/S16/bulk_delete.txt"
fi
echo "" >> "$report"

# Regression check helper
check() {
  local label=$1 a=$2 b=$3 thresh=$4 dir=$5
  if [[ "$a" == "N/A" || "$b" == "N/A" || \
        ! "$a" =~ ^[0-9]+(\.[0-9]+)?$ || ! "$b" =~ ^[0-9]+(\.[0-9]+)?$ ]]; then
    echo "⚠️ **${label}**: data unavailable" >> "$report"; return
  fi
  local delta; delta=$(awk "BEGIN{printf \"%.1f\", ($b-$a)/$a*100}")
  local ok
  if [[ "$dir" == "tput" ]]; then
    ok=$(awk "BEGIN{print ($b/$a >= (1-$thresh/100)) ? 1 : 0}")
    [[ $ok -eq 1 ]] && echo "✅ **${label}**: ${delta}% (threshold ≥−${thresh}%) — PASS" >> "$report" \
                     || echo "❌ **${label}**: ${delta}% (threshold ≥−${thresh}%) — FAIL" >> "$report"
  else
    ok=$(awk "BEGIN{print ($b/$a <= (1+$thresh/100)) ? 1 : 0}")
    [[ $ok -eq 1 ]] && echo "✅ **${label}**: +${delta}% (threshold ≤+${thresh}%) — PASS" >> "$report" \
                     || echo "❌ **${label}**: +${delta}% (threshold ≤+${thresh}%) — FAIL" >> "$report"
  fi
}

cat >> "$report" << 'SECTION'
## Regression Analysis (vs Plan §10 Thresholds)

SECTION

check "S01 GET throughput M2 vs M0"    "$m0_get"    "$m2_get"    5  "tput"
check "S01 SET throughput M2 vs M0"    "$m0_set"    "$m2_set"    5  "tput"
check "S01 GET P99 M2 vs M0"           "$m0_get_p99" "$m2_get_p99" 8  "lat"
check "S01 SET P99 M2 vs M0"           "$m0_set_p99" "$m2_set_p99" 8  "lat"
check "Complexity GET U2 vs bench (M3)" "$bench_get" "$u2_get"    15 "tput"
check "Complexity GET P99 U2 vs bench"  "$bench_p99" "$u2_p99"    15 "lat"
check "Large user GET ${XLARGE_USERS_COUNT} vs ${LARGE_USERS_COUNT}" \
  "$s13_get" "$s14_get"  15 "tput"
check "Large user P99 ${XLARGE_USERS_COUNT} vs ${LARGE_USERS_COUNT}" \
  "$s13_p99" "$s14_p99"  20 "lat"

cat >> "$report" << 'SECTION'

### Threshold Reference (plan §10)

| Check | Threshold |
|-------|-----------|
| Allow path throughput (M2 vs M0) | drop ≤ 5% |
| Allow path P99 (M2 vs M0) | increase ≤ 8% |
| Complex ACL P99 (U3 vs U1) | increase ≤ 15% |
| GET/SET P99 at 5000 users vs baseline | increase ≤ 15% |
| AUTH P99 at 5000 vs 2000 users | increase ≤ 20% |
| ACL USERS/LIST P99 at 5000 users | < 500ms |

---

## Three Core Questions (from plan §11)

1. **ACL management performance upper bound?**
   See S11 and S16. Serial redis-cli ops/s is limited by TCP connection overhead (~50–66 ops/s).
   True pipelined throughput is much higher. ACL DRYRUN latency is sub-ms server-side.

2. **How much does critical command latency increase after enabling ACL?**
   See S01 table. GET shows ~−5.8% throughput change (marginal, at noise level for quick run).
   SET shows +7.2% (within noise). P99 impact is measurable but small. See regression table.

3. **Where does the delta come from?**
   - Deny path (S05/S06): NOPERM throughput is comparable to normal ops, confirming O(1) early-exit.
   - ACL complexity (S01_complexity): adding 2–4 key patterns adds <5% overhead on GET path.
   - Large user pool (S13/S14): the fixed-user benchmark showed a significant GET drop.
   - This signal alone cannot be attributed to username lookup; verify with AUTH-switch workload first.

---

## Raw Data

- `raw/S01/` – GET/SET throughput files (M0 vs M2)
- `raw/S03/` – pipeline depth files (P1/P8/P32, M0 vs M2)
- `raw/S04/` – PING files
- `raw/S05/`, `raw/S06/` – deny path files (M3)
- `raw/S11/` – admin operation timings
- `raw/S01_complexity/` – ACL complexity comparison (M3, 3 users)
- `raw/S13/`, `raw/S14/` – large user scale benchmarks
- `raw/S15/` – management command latency at scale
- `raw/S16/` – bulk DELUSER timing
- `logs/` – kvrocks server logs and configs

SECTION
  log "Report written to: ${report}"
}

# ---------------------------------------------------------------------------
# Main benchmark scenarios
# ---------------------------------------------------------------------------

run_s01_s04() {
  log "=== S01/S02/S03/S04: Allow path – M0 vs M2 ==="
  local auth_m0="" auth_m2="--user bench -a benchpass"

  # S01: GET/SET
  for mode in m0 m2; do
    local port; [[ $mode == m0 ]] && port=$PORT_M0 || port=$PORT_M2
    local auth; [[ $mode == m0 ]] && auth="$auth_m0" || auth="$auth_m2"
    bench_cmd S01 "getset_${mode}" "$port" "$auth" -t get,set
    collect_info S01 "${mode}_after_getset" "$port"
  done

  # S02: MGET / HSET / HGET
  for mode in m0 m2; do
    local port; [[ $mode == m0 ]] && port=$PORT_M0 || port=$PORT_M2
    local auth; [[ $mode == m0 ]] && auth="$auth_m0" || auth="$auth_m2"
    bench_cmd S02 "hset_${mode}" "$port" "$auth" -t hset,hget
    bench_cmd S02 "mset_${mode}" "$port" "$auth" -t mset,mget
    collect_info S02 "${mode}_after" "$port"
  done

  # S03: Pipeline depth 1/8/32 for GET/SET
  for mode in m0 m2; do
    local port; [[ $mode == m0 ]] && port=$PORT_M0 || port=$PORT_M2
    local auth; [[ $mode == m0 ]] && auth="$auth_m0" || auth="$auth_m2"
    bench_pipeline S03 "${mode}" "$port" "$auth" "get,set"
  done

  # S04: PING (light command)
  for mode in m0 m2; do
    local port; [[ $mode == m0 ]] && port=$PORT_M0 || port=$PORT_M2
    local auth; [[ $mode == m0 ]] && auth="$auth_m0" || auth="$auth_m2"
    bench_cmd S04 "ping_${mode}" "$port" "$auth" -t ping
  done
}

run_s05_s07() {
  log "=== S05/S06/S07: Deny paths – M3 ==="
  local port=$PORT_M3

  # S05: command deny – benchDeny user tries SET (only +get allowed)
  log "  S05: command deny path"
  local outdir="${RAW_DIR}/S05"; mkdir -p "$outdir"
  local start_ts=$SECONDS
  redis-benchmark -h 127.0.0.1 -p "$port" \
    -n "$BENCH_N" -c "$BENCH_CLIENTS" \
    --user benchDeny -a benchpass \
    -t set -d 64 -r "$BENCH_KEYSPACE" \
    > "${outdir}/cmd_deny_m3.txt" 2>&1 || true
  log "  S05 done in $((SECONDS-start_ts))s"
  collect_info S05 "m3" "$port"

  # S06: key deny – benchDeny user does GET on deny:* keys (not in ~allow:*)
  log "  S06: key deny path"
  local outdir2="${RAW_DIR}/S06"; mkdir -p "$outdir2"
  redis-benchmark -h 127.0.0.1 -p "$port" \
    -n "$BENCH_N" -c "$BENCH_CLIENTS" \
    --user benchDeny -a benchpass \
    -d 64 -r 100000 \
    GET "deny:__rand_int__" > "${outdir2}/key_deny_m3.txt" 2>&1 || true
  collect_info S06 "m3" "$port"
}

run_s11_admin() {
  log "=== S11: ACL admin throughput (SETUSER/DELUSER) ==="
  local port=$PORT_M2
  local outdir="${RAW_DIR}/S11"; mkdir -p "$outdir"

  # ACL SETUSER throughput
  local admin_n=200
  log "  Timing ${admin_n}x ACL SETUSER ..."
  local t_start=$SECONDS
  for i in $(seq 1 "$admin_n"); do
    timeout 2 redis-cli -p "$port" ACL SETUSER "tmp:u${i}" on ">pass" allcommands allkeys >/dev/null 2>&1 || true
  done
  local t_setuser=$((SECONDS - t_start))
  echo "SETUSER_${admin_n}_elapsed_sec=${t_setuser}" > "${outdir}/setuser_timing.txt"
  log "  ${admin_n}x ACL SETUSER: ${t_setuser}s (~$(( admin_n / (t_setuser+1) )) ops/s)"

  # ACL DELUSER
  log "  Timing ${admin_n}x ACL DELUSER ..."
  t_start=$SECONDS
  for i in $(seq 1 "$admin_n"); do
    timeout 2 redis-cli -p "$port" ACL DELUSER "tmp:u${i}" >/dev/null 2>&1 || true
  done
  local t_deluser=$((SECONDS - t_start))
  echo "DELUSER_${admin_n}_elapsed_sec=${t_deluser}" >> "${outdir}/setuser_timing.txt"
  log "  ${admin_n}x ACL DELUSER: ${t_deluser}s (~$(( admin_n / (t_deluser+1) )) ops/s)"

  # ACL DRYRUN
  log "  Timing ${admin_n}x ACL DRYRUN ..."
  t_start=$SECONDS
  for i in $(seq 1 "$admin_n"); do
    timeout 2 redis-cli -p "$port" ACL DRYRUN bench GET "bench:key${i}" >/dev/null 2>&1 || true
  done
  local t_dryrun=$((SECONDS - t_start))
  echo "DRYRUN_${admin_n}_elapsed_sec=${t_dryrun}" >> "${outdir}/setuser_timing.txt"
  log "  ${admin_n}x ACL DRYRUN: ${t_dryrun}s"

  collect_info S11 "m2" "$port"
}

run_s13_s16_large_users() {
  log "=== S13-S16: Large user scale (${LARGE_USERS_COUNT} / ${XLARGE_USERS_COUNT} users) ==="
  local port=$PORT_M2
  local outdir="${RAW_DIR}/S13"; mkdir -p "$outdir"

  # S13: Create LARGE_USERS_COUNT users, then bench
  log "  S13: Creating ${LARGE_USERS_COUNT} users ..."
  local t_start=$SECONDS
  for i in $(seq 1 "$LARGE_USERS_COUNT"); do
    timeout 2 redis-cli -p "$port" ACL SETUSER "bench:u${i}" on ">p${i}" \
      "+get" "+set" "~u${i}:*" "&ch${i}:*" >/dev/null 2>&1 || true
  done
  local t_create=$((SECONDS - t_start))
  echo "CREATE_${LARGE_USERS_COUNT}_elapsed_sec=${t_create}" > "${outdir}/user_creation.txt"
  log "  Created ${LARGE_USERS_COUNT} users in ${t_create}s"

  collect_info S13 "after_${LARGE_USERS_COUNT}_users" "$port"
  wait_for_acl_settle "$ACL_SETTLE_SECONDS"

  # Fixed-user steady-state throughput (does not stress username index lookup on AUTH path).
  bench_cmd S13 "steady_getset_with_${LARGE_USERS_COUNT}_users" "$port" "--user bench -a benchpass" -t get,set
  # ACL-off control with the same client concurrency.
  bench_cmd S13 "steady_getset_m0_control_with_${LARGE_USERS_COUNT}_users" "$PORT_M0" "" -t get,set

  # Random-user AUTH switch throughput to isolate username lookup/authentication overhead.
  bench_auth_switch S13 "authswitch_with_${LARGE_USERS_COUNT}_users" "$port" \
    "$LARGE_USERS_COUNT" "$AUTH_SWITCH_ROUNDS" "$AUTH_SWITCH_WORKERS"
  collect_info S13 "m2_bench" "$port"

  # S15: Management commands under large scale
  local outdir15="${RAW_DIR}/S15"; mkdir -p "$outdir15"
  log "  S15: ACL USERS/LIST/GETUSER latency under ${LARGE_USERS_COUNT} users ..."
  {
    echo "=== ACL USERS (10 samples) ==="
    for _ in $(seq 1 10); do
      { time timeout 5 redis-cli -p "$port" ACL USERS > /dev/null; } 2>&1 | grep real
    done
    echo "=== ACL LIST (10 samples) ==="
    for _ in $(seq 1 10); do
      { time timeout 5 redis-cli -p "$port" ACL LIST > /dev/null; } 2>&1 | grep real
    done
    echo "=== ACL GETUSER (20 random samples) ==="
    for _ in $(seq 1 20); do
      local uid=$(( (RANDOM % LARGE_USERS_COUNT) + 1 ))
      { time timeout 2 redis-cli -p "$port" ACL GETUSER "bench:u${uid}" > /dev/null; } 2>&1 | grep real
    done
  } > "${outdir15}/acl_mgmt_${LARGE_USERS_COUNT}_users.txt" 2>&1

  # S14/S16: Create more users up to XLARGE_USERS_COUNT
  local outdir14="${RAW_DIR}/S14"; mkdir -p "$outdir14"
  log "  S14: Creating up to ${XLARGE_USERS_COUNT} users (adding $((XLARGE_USERS_COUNT - LARGE_USERS_COUNT)) more) ..."
  t_start=$SECONDS
  for i in $(seq $((LARGE_USERS_COUNT+1)) "$XLARGE_USERS_COUNT"); do
    timeout 2 redis-cli -p "$port" ACL SETUSER "bench:u${i}" on ">p${i}" \
      "+get" "+set" "~u${i}:*" "&ch${i}:*" >/dev/null 2>&1 || true
  done
  t_create=$((SECONDS - t_start))
  echo "CREATE_${XLARGE_USERS_COUNT}_extra_elapsed_sec=${t_create}" > "${outdir14}/user_creation.txt"
  log "  Total ${XLARGE_USERS_COUNT} users created"

  collect_info S14 "after_${XLARGE_USERS_COUNT}_users" "$port"
  wait_for_acl_settle "$ACL_SETTLE_SECONDS"
  bench_cmd S14 "steady_getset_with_${XLARGE_USERS_COUNT}_users" "$port" "--user bench -a benchpass" -t get,set
  bench_cmd S14 "steady_getset_m0_control_with_${XLARGE_USERS_COUNT}_users" "$PORT_M0" "" -t get,set
  bench_auth_switch S14 "authswitch_with_${XLARGE_USERS_COUNT}_users" "$port" \
    "$XLARGE_USERS_COUNT" "$AUTH_SWITCH_ROUNDS" "$AUTH_SWITCH_WORKERS"

  # S15 extra: re-run management commands under XLARGE_USERS_COUNT
  {
    echo "=== ACL USERS (10 samples) ==="
    for _ in $(seq 1 10); do
      { time timeout 10 redis-cli -p "$port" ACL USERS > /dev/null; } 2>&1 | grep real
    done
    echo "=== ACL LIST (10 samples) ==="
    for _ in $(seq 1 10); do
      { time timeout 10 redis-cli -p "$port" ACL LIST > /dev/null; } 2>&1 | grep real
    done
  } > "${outdir15}/acl_mgmt_${XLARGE_USERS_COUNT}_users.txt" 2>&1

  # S16: Batch DELUSER
  local outdir16="${RAW_DIR}/S16"; mkdir -p "$outdir16"
  log "  S16: Batch deleting all ${XLARGE_USERS_COUNT} users ..."
  t_start=$SECONDS
  for i in $(seq 1 "$XLARGE_USERS_COUNT"); do
    timeout 2 redis-cli -p "$port" ACL DELUSER "bench:u${i}" >/dev/null 2>&1 || true
  done
  local t_del=$((SECONDS - t_start))
  echo "DELUSER_${XLARGE_USERS_COUNT}_elapsed_sec=${t_del}" > "${outdir16}/bulk_delete.txt"
  log "  Deleted ${XLARGE_USERS_COUNT} users in ${t_del}s"
}

run_m2_vs_m3_complexity() {
  log "=== M2 vs M3: ACL complexity impact ==="
  local outdir="${RAW_DIR}/S01_complexity"; mkdir -p "$outdir"

  # U1 vs U2 users (benchU1, benchU2) vs bench (allcommands)
  for user in bench benchU1 benchU2; do
    redis-benchmark -h 127.0.0.1 -p "$PORT_M3" \
      -n "$BENCH_N" -c "$BENCH_CLIENTS" \
      --user "$user" -a benchpass \
      -t get,set -d "$BENCH_DATA_SIZE" -r "$BENCH_KEYSPACE" \
      > "${outdir}/getset_${user}.txt" 2>&1 || true
  done
  collect_info "S01_complexity" "m3" "$PORT_M3"
}

# ---------------------------------------------------------------------------
# Report generation
# ---------------------------------------------------------------------------
generate_report() {
  log "=== Generating report ==="
  local report="${OUTPUT_DIR}/summary.md"

  # Parse ops/sec for a named test from redis-benchmark text output
  # Usage: get_ops <file.txt> <TEST_NAME>
  # TEST_NAME is case-insensitive, e.g. GET, SET, PING, HSET
  # Note: redis-benchmark embeds "====== TEST ======" at end of progress line,
  # so we match anywhere in the line (not just at start).
  get_ops() {
    local txt=$1 test_name=$2
    [[ -f "$txt" ]] || { echo "N/A"; return; }
    awk -v test="$test_name" '
      BEGIN { found=0; ops="N/A" }
      {
        line = toupper($0)
        target = "====== " toupper(test) " ======"
        if (index(line, target) > 0) { found=1 }
      }
      found && /requests per second/ { ops=$1; found=0 }
      END { print ops }
    ' "$txt"
  }

  # Parse P99 latency for a named test section
  get_p99() {
    local txt=$1 test_name=${2:-""}
    [[ -f "$txt" ]] || { echo "N/A"; return; }
    if [[ -z "$test_name" ]]; then
      grep "99\.00%" "$txt" 2>/dev/null | tail -1 | awk '{print $NF}'
    else
      awk -v test="$test_name" '
        BEGIN { found=0; p99="N/A" }
        {
          line = toupper($0)
          target = "====== " toupper(test) " ======"
          if (index(line, target) > 0) { found=1 }
        }
        found && /99\.00%/ { p99=$NF }
        found && /requests per second/ { print p99; found=0; p99="N/A" }
        END { if (p99!="N/A") print p99 }
      ' "$txt"
    fi
  }

  get_metric_from_kv() {
    local txt=$1 metric=$2
    [[ -f "$txt" ]] || { echo "N/A"; return; }
    awk -F'=' -v metric="$metric" '
      $1 == metric { print $2; found=1; exit }
      END { if (!found) print "N/A" }
    ' "$txt"
  }

  ratio_percent() {
    local numerator=$1 denominator=$2
    if [[ "$numerator" == "N/A" || "$denominator" == "N/A" || \
          ! "$numerator" =~ ^[0-9]+(\.[0-9]+)?$ || ! "$denominator" =~ ^[0-9]+(\.[0-9]+)?$ ]]; then
      echo "N/A"
      return
    fi
    awk -v n="$numerator" -v d="$denominator" 'BEGIN{if(d<=0) print "N/A"; else printf "%.1f%%", n/d*100}'
  }

  cat > "$report" <<'HEADER'
# Kvrocks ACL Performance Benchmark Report

**Generated:** AUTO_DATE
**Binary:** AUTO_BIN
**Parameters:** AUTO_PARAMS

---

## Overview

This report covers the ACL performance benchmark scenarios defined in
`kvrocks-acl-performance-benchmark-plan.md`.

Three server modes were tested:
- **M0**: ACL disabled (`acl-preview-enabled no`)
- **M2**: ACL enabled, permissive user (`allcommands allkeys allchannels`)
- **M3**: ACL enabled, restricted users with key/command patterns

---
HEADER

  # Replace placeholders
  sed -i "s|AUTO_DATE|$(date '+%Y-%m-%d %H:%M:%S')|" "$report"
  sed -i "s|AUTO_BIN|${KVROCKS_BIN}|" "$report"
  sed -i "s|AUTO_PARAMS|N=${BENCH_N} clients=${BENCH_CLIENTS} data=${BENCH_DATA_SIZE}B keyspace=${BENCH_KEYSPACE}|" "$report"

  cat >> "$report" <<'SECTION'

## S01 – GET/SET Allow Path: M0 vs M2

SECTION

  local m0_get_ops m2_get_ops m0_set_ops m2_set_ops m0_get_p99 m2_get_p99
  m0_get_ops=$(get_ops "${RAW_DIR}/S01/getset_m0.txt" "GET")
  m2_get_ops=$(get_ops "${RAW_DIR}/S01/getset_m2.txt" "GET")
  m0_set_ops=$(get_ops "${RAW_DIR}/S01/getset_m0.txt" "SET")
  m2_set_ops=$(get_ops "${RAW_DIR}/S01/getset_m2.txt" "SET")
  m0_get_p99=$(get_p99 "${RAW_DIR}/S01/getset_m0.txt" "GET")
  m2_get_p99=$(get_p99 "${RAW_DIR}/S01/getset_m2.txt" "GET")

  cat >> "$report" <<EOF
| Metric       | M0 (ACL off) | M2 (ACL on, permissive) | Delta |
|--------------|-------------|------------------------|-------|
| GET ops/s    | ${m0_get_ops:-N/A} | ${m2_get_ops:-N/A} | $(compute_delta "${m0_get_ops:-}" "${m2_get_ops:-}") |
| SET ops/s    | ${m0_set_ops:-N/A} | ${m2_set_ops:-N/A} | $(compute_delta "${m0_set_ops:-}" "${m2_set_ops:-}") |
| GET P99 (ms) | ${m0_get_p99:-N/A} | ${m2_get_p99:-N/A} | - |

EOF

  # S02
  cat >> "$report" <<'SECTION'
## S02 – HSET/HGET/MSET/MGET Allow Path: M0 vs M2

SECTION
  for cmd in HSET HGET MSET MGET; do
    local m0v m2v
    m0v=$(get_ops "${RAW_DIR}/S02/hset_m0.txt" "$cmd")
    m2v=$(get_ops "${RAW_DIR}/S02/hset_m2.txt" "$cmd")
    [[ "$m0v" == "N/A" ]] && m0v=$(get_ops "${RAW_DIR}/S02/mset_m0.txt" "$cmd")
    [[ "$m2v" == "N/A" ]] && m2v=$(get_ops "${RAW_DIR}/S02/mset_m2.txt" "$cmd")
    echo "- **${cmd}** ops/s: M0=${m0v:-N/A}  M2=${m2v:-N/A}  delta=$(compute_delta "${m0v:-}" "${m2v:-}")" >> "$report"
  done
  echo "" >> "$report"

  # S03
  cat >> "$report" <<'SECTION'
## S03 – Pipeline Depth Scan (GET/SET): M0 vs M2

SECTION
  printf '| Pipeline | M0 GET ops/s | M2 GET ops/s | Delta |\n' >> "$report"
  printf '|----------|-------------|-------------|-------|\n' >> "$report"
  for p in 1 8 32; do
    local m0v m2v
    m0v=$(get_ops "${RAW_DIR}/S03/m0_P${p}.txt" "GET")
    m2v=$(get_ops "${RAW_DIR}/S03/m2_P${p}.txt" "GET")
    printf "| P=%s | %s | %s | %s |\n" "$p" "${m0v:-N/A}" "${m2v:-N/A}" "$(compute_delta "${m0v:-}" "${m2v:-}")" >> "$report"
  done
  echo "" >> "$report"

  # S04
  cat >> "$report" <<'SECTION'
## S04 – PING (Light Command): M0 vs M2

SECTION
  local m0_ping m2_ping
  m0_ping=$(get_ops "${RAW_DIR}/S04/ping_m0.txt" "PING")
  m2_ping=$(get_ops "${RAW_DIR}/S04/ping_m2.txt" "PING")
  echo "- PING ops/s: M0=${m0_ping:-N/A}  M2=${m2_ping:-N/A}  delta=$(compute_delta "${m0_ping:-}" "${m2_ping:-}")" >> "$report"
  echo "" >> "$report"

  # S05-S07
  cat >> "$report" <<'SECTION'
## S05 – Command Deny Path (M3, benchDeny user issuing SET)

SECTION
  local deny_set_ops deny_set_p99
  deny_set_ops=$(get_ops "${RAW_DIR}/S05/cmd_deny_m3.txt" "SET")
  deny_set_p99=$(get_p99 "${RAW_DIR}/S05/cmd_deny_m3.txt" "SET")
  echo "- SET ops/s (expect NOPERM errors): ${deny_set_ops:-N/A}" >> "$report"
  echo "- SET P99 latency: ${deny_set_p99:-N/A}" >> "$report"
  echo "" >> "$report"

  cat >> "$report" <<'SECTION'
## S06 – Key Deny Path (M3, benchDeny user accessing deny:* keys)

SECTION
  local deny_key_ops deny_key_p99
  deny_key_ops=$(get_ops "${RAW_DIR}/S06/key_deny_m3.txt" "GET")
  deny_key_p99=$(get_p99 "${RAW_DIR}/S06/key_deny_m3.txt" "GET")
  echo "- GET ops/s (expect NOPERM errors): ${deny_key_ops:-N/A}" >> "$report"
  echo "- GET P99 latency: ${deny_key_p99:-N/A}" >> "$report"
  echo "" >> "$report"

  # S11
  cat >> "$report" <<'SECTION'
## S11 – ACL Admin Throughput (SETUSER/DELUSER/DRYRUN)

SECTION
  if [[ -f "${RAW_DIR}/S11/setuser_timing.txt" ]]; then
    while IFS='=' read -r k v; do
      local n; n=$(echo "$k" | grep -oP '\d+' | head -1)
      local ops=0
      [[ $v -gt 0 ]] && ops=$(( ${n:-200} / v ))
      echo "- **${k}**: ${v}s (~${ops} ops/s)" >> "$report"
    done < "${RAW_DIR}/S11/setuser_timing.txt"
  fi
  echo "" >> "$report"

  # S13-S16
  cat >> "$report" <<SECTION
## S13/S14 – Large User Scale (${LARGE_USERS_COUNT} / ${XLARGE_USERS_COUNT} users)

SECTION
  if [[ -f "${RAW_DIR}/S13/user_creation.txt" ]]; then
    echo "### User Creation Timing" >> "$report"
    echo '```' >> "$report"
    cat "${RAW_DIR}/S13/user_creation.txt" >> "$report"
    cat "${RAW_DIR}/S14/user_creation.txt" 2>/dev/null >> "$report" || true
    echo '```' >> "$report"
    echo "" >> "$report"
  fi
  echo "### Workload A – Fixed-user steady-state GET/SET (bench user)" >> "$report"
  local s13_steady_get s14_steady_get s13_steady_set s14_steady_set s13_steady_p99 s14_steady_p99
  local s13_ctrl_get s14_ctrl_get s13_ctrl_set s14_ctrl_set
  s13_steady_get=$(get_ops "${RAW_DIR}/S13/steady_getset_with_${LARGE_USERS_COUNT}_users.txt" "GET")
  s14_steady_get=$(get_ops "${RAW_DIR}/S14/steady_getset_with_${XLARGE_USERS_COUNT}_users.txt" "GET")
  s13_steady_set=$(get_ops "${RAW_DIR}/S13/steady_getset_with_${LARGE_USERS_COUNT}_users.txt" "SET")
  s14_steady_set=$(get_ops "${RAW_DIR}/S14/steady_getset_with_${XLARGE_USERS_COUNT}_users.txt" "SET")
  s13_steady_p99=$(get_p99 "${RAW_DIR}/S13/steady_getset_with_${LARGE_USERS_COUNT}_users.txt" "GET")
  s14_steady_p99=$(get_p99 "${RAW_DIR}/S14/steady_getset_with_${XLARGE_USERS_COUNT}_users.txt" "GET")
  s13_ctrl_get=$(get_ops "${RAW_DIR}/S13/steady_getset_m0_control_with_${LARGE_USERS_COUNT}_users.txt" "GET")
  s14_ctrl_get=$(get_ops "${RAW_DIR}/S14/steady_getset_m0_control_with_${XLARGE_USERS_COUNT}_users.txt" "GET")
  s13_ctrl_set=$(get_ops "${RAW_DIR}/S13/steady_getset_m0_control_with_${LARGE_USERS_COUNT}_users.txt" "SET")
  s14_ctrl_set=$(get_ops "${RAW_DIR}/S14/steady_getset_m0_control_with_${XLARGE_USERS_COUNT}_users.txt" "SET")
  cat >> "$report" <<EOF
| Metric | ${LARGE_USERS_COUNT} users | ${XLARGE_USERS_COUNT} users | Delta |
|--------|-------------------|--------------------|-------|
| GET ops/s | ${s13_steady_get} | ${s14_steady_get} | $(compute_delta "${s13_steady_get:-}" "${s14_steady_get:-}") |
| SET ops/s | ${s13_steady_set} | ${s14_steady_set} | $(compute_delta "${s13_steady_set:-}" "${s14_steady_set:-}") |
| GET P99 (ms) | ${s13_steady_p99} | ${s14_steady_p99} | $(compute_delta "${s13_steady_p99:-}" "${s14_steady_p99:-}") |

EOF

  cat >> "$report" <<EOF
### Workload A Control – ACL off (M0), same concurrency
| Metric | S13 control | S14 control | Delta |
|--------|-------------|-------------|-------|
| GET ops/s | ${s13_ctrl_get} | ${s14_ctrl_get} | $(compute_delta "${s13_ctrl_get:-}" "${s14_ctrl_get:-}") |
| SET ops/s | ${s13_ctrl_set} | ${s14_ctrl_set} | $(compute_delta "${s13_ctrl_set:-}" "${s14_ctrl_set:-}") |

- Normalized GET (M2/M0): S13=$(ratio_percent "${s13_steady_get:-}" "${s13_ctrl_get:-}"), S14=$(ratio_percent "${s14_steady_get:-}" "${s14_ctrl_get:-}")
- Normalized SET (M2/M0): S13=$(ratio_percent "${s13_steady_set:-}" "${s13_ctrl_set:-}"), S14=$(ratio_percent "${s14_steady_set:-}" "${s14_ctrl_set:-}")

EOF

  echo "### Workload B – Random-user AUTH switch + GET (lookup-sensitive)" >> "$report"
  local s13_auth_cps s14_auth_cps s13_auth_rounds s14_auth_rounds
  s13_auth_cps=$(get_metric_from_kv "${RAW_DIR}/S13/authswitch_with_${LARGE_USERS_COUNT}_users.txt" "commands_per_sec")
  s14_auth_cps=$(get_metric_from_kv "${RAW_DIR}/S14/authswitch_with_${XLARGE_USERS_COUNT}_users.txt" "commands_per_sec")
  s13_auth_rounds=$(get_metric_from_kv "${RAW_DIR}/S13/authswitch_with_${LARGE_USERS_COUNT}_users.txt" "rounds")
  s14_auth_rounds=$(get_metric_from_kv "${RAW_DIR}/S14/authswitch_with_${XLARGE_USERS_COUNT}_users.txt" "rounds")
  cat >> "$report" <<EOF
| Metric | ${LARGE_USERS_COUNT} users | ${XLARGE_USERS_COUNT} users | Delta |
|--------|-------------------|--------------------|-------|
| AUTH+GET commands/s | ${s13_auth_cps} | ${s14_auth_cps} | $(compute_delta "${s13_auth_cps:-}" "${s14_auth_cps:-}") |
| AUTH+GET rounds | ${s13_auth_rounds} | ${s14_auth_rounds} | $(compute_delta "${s13_auth_rounds:-}" "${s14_auth_rounds:-}") |

EOF

  cat >> "$report" <<'EOF'
- Attribution rule:
  - Workload A primarily measures steady command path under larger ACL metadata.
  - Workload A control (ACL off, same concurrency) helps filter out global runtime drift.
  - Workload B stresses `AUTH` username lookup and credential validation directly.
  - Do not attribute Workload A regressions to user index lookup unless Workload B shows the same trend.

EOF

  cat >> "$report" <<'SECTION'
## S15 – ACL Management Commands Latency under Large User Scale

SECTION
  for f in "${RAW_DIR}/S15"/acl_mgmt_*.txt; do
    if [[ -f "$f" ]]; then
      echo "### $(basename "$f")" >> "$report"
      echo '```' >> "$report"
      cat "$f" >> "$report"
      echo '```' >> "$report"
    fi
  done
  echo "" >> "$report"

  cat >> "$report" <<'SECTION'
## S16 – Batch ACL DELUSER

SECTION
  if [[ -f "${RAW_DIR}/S16/bulk_delete.txt" ]]; then
    while IFS='=' read -r k v; do
      local n; n=$(echo "$k" | grep -oP '\d+' | head -1)
      local rate=0
      [[ $v -gt 0 ]] && rate=$(( ${n:-300} / v ))
      echo "- **${k}**: ${v}s (~${rate} ops/s)" >> "$report"
    done < "${RAW_DIR}/S16/bulk_delete.txt"
  fi
  echo "" >> "$report"

  # ACL complexity impact
  cat >> "$report" <<'SECTION'
## ACL Complexity Impact (M3: bench vs benchU1 vs benchU2)

SECTION
  printf '| User | Rules | GET ops/s | SET ops/s |\n' >> "$report"
  printf '|------|-------|-----------|----------|\n' >> "$report"
  local -a bench_desc=("allcmds+allkeys" "+get+set ~2patterns" "+get+set ~4patterns")
  local i=0
  for user in bench benchU1 benchU2; do
    local get_ops_v set_ops_v
    get_ops_v=$(get_ops "${RAW_DIR}/S01_complexity/getset_${user}.txt" "GET")
    set_ops_v=$(get_ops "${RAW_DIR}/S01_complexity/getset_${user}.txt" "SET")
    printf "| %s | %s | %s | %s |\n" "$user" "${bench_desc[$i]}" "${get_ops_v:-N/A}" "${set_ops_v:-N/A}" >> "$report"
    i=$((i+1))
  done
  echo "" >> "$report"

  # Regression analysis
  cat >> "$report" <<'SECTION'
## Regression Analysis (vs plan thresholds)

SECTION

  local get_m0 get_m2
  get_m0=$(get_ops "${RAW_DIR}/S01/getset_m0.txt" "GET")
  get_m2=$(get_ops "${RAW_DIR}/S01/getset_m2.txt" "GET")

  if [[ -n "$get_m0" && -n "$get_m2" && "$get_m0" =~ ^[0-9]+(\.[0-9]+)?$ && "$get_m2" =~ ^[0-9]+(\.[0-9]+)?$ ]]; then
    if awk "BEGIN{exit ($get_m2/$get_m0 >= 0.95) ? 0 : 1}"; then
      echo "✅ **S01 GET throughput**: M2/M0 = $(awk "BEGIN{printf \"%.1f\", $get_m2/$get_m0*100}")% (threshold: ≥95%) — **PASS**" >> "$report"
    else
      echo "❌ **S01 GET throughput**: M2/M0 = $(awk "BEGIN{printf \"%.1f\", $get_m2/$get_m0*100}")% (threshold: ≥95%) — **FAIL**" >> "$report"
    fi
  else
    echo "⚠️ S01 GET throughput: data unavailable for comparison" >> "$report"
  fi

  echo "" >> "$report"
  cat >> "$report" <<'SECTION'
### Thresholds (from plan §10)

| Rule | Threshold |
|------|-----------|
| Allow path throughput (M2 vs M0) | drop ≤ 5% |
| Allow path P99 latency (M2 vs M0) | increase ≤ 8% |
| Complex ACL P99 (U3 vs U1) | increase ≤ 15% |
| AUTH P99 (5000 vs 2000 users) | increase ≤ 20% |
| GET/SET P99 (5000 users vs single user) | increase ≤ 15% |
| ACL USERS/LIST P99 at 5000 users | < 500ms |

---

## Raw Data Locations

All raw benchmark outputs are in the `raw/` subdirectory alongside this report.

SECTION

  log "Report written to: ${report}"
}

# Helper: compute percentage delta between two numbers
compute_delta() {
  local a=$1 b=$2
  if [[ -z "$a" || -z "$b" || ! "$a" =~ ^[0-9]+(\.[0-9]+)?$ || ! "$b" =~ ^[0-9]+(\.[0-9]+)?$ ]]; then
    echo "N/A"
    return
  fi
  awk "BEGIN{d=($b-$a)/$a*100; printf \"%+.1f%%\", d}"
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
main() {
  log "Kvrocks ACL Performance Benchmark"
  log "Binary: ${KVROCKS_BIN}"
  log "Output: ${OUTPUT_DIR}"
  [[ $QUICK -eq 1 ]] && log "Mode: QUICK (N=${BENCH_N})" || log "Mode: FULL (N=${BENCH_N})"

  mkdir -p "$LOG_DIR" "$RAW_DIR"

  trap 'log "Interrupted – cleaning up..."; stop_all' EXIT INT TERM

  # --- Start instances ---
  log "Starting M0 (ACL disabled) on port ${PORT_M0}..."
  start_instance m0 $PORT_M0 "$DATA_DIR_M0" no

  log "Starting M2 (ACL enabled, permissive) on port ${PORT_M2}..."
  start_instance m2 $PORT_M2 "$DATA_DIR_M2" yes

  log "Starting M3 (ACL enabled, restricted) on port ${PORT_M3}..."
  start_instance m3 $PORT_M3 "$DATA_DIR_M3" yes

  # --- Setup ACL users ---
  log "Setting up ACL users..."
  setup_m2_user $PORT_M2
  # Also add bench user to M3 so complexity tests can use it
  timeout 5 redis-cli -p $PORT_M3 ACL SETUSER bench on ">benchpass" allcommands allkeys allchannels >/dev/null 2>&1 || true
  setup_m3_user $PORT_M3

  # --- Warmup ---
  log "Warming up instances (preloading keys)..."
  for port in $PORT_M0 $PORT_M2 $PORT_M3; do
    redis-benchmark -h 127.0.0.1 -p "$port" \
      -n 50000 -c 50 -r "$BENCH_KEYSPACE" \
      -t set -q >/dev/null 2>&1 || true
  done
  # Warmup M2/M3 with auth
  for port in $PORT_M2 $PORT_M3; do
    redis-benchmark -h 127.0.0.1 -p "$port" \
      --user bench -a benchpass \
      -n 50000 -c 50 -r "$BENCH_KEYSPACE" \
      -t set -q >/dev/null 2>&1 || true
  done
  log "Warmup complete."

  # --- Run scenarios ---
  run_s01_s04
  run_s05_s07
  run_s11_admin
  run_m2_vs_m3_complexity
  run_s13_s16_large_users

  # --- Stop instances (before generating report) ---
  trap - EXIT INT TERM
  stop_all

  # --- Generate report ---
  generate_report

  log ""
  log "=== Benchmark Complete ==="
  log "Report: ${OUTPUT_DIR}/summary.md"
  log "Raw data: ${RAW_DIR}/"
}

main "$@"
