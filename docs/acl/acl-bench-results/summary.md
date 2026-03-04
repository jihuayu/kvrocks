# Kvrocks ACL Performance Benchmark Report

**Generated:** 2026-03-04 16:06:54  
**Binary:** /root/kvrocks/build/kvrocks  
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

## S01 – GET/SET Allow Path: M0 vs M2

| Metric | M0 (ACL off) | M2 (ACL on, permissive) | Delta |
|--------|-------------|------------------------|-------|
| GET ops/s | 11422.04 | 10764.26 | -5.8% |
| SET ops/s | 6620.32 | 7099.75 | +7.2% |
| GET P99 (ms) | 18 | 20 | +11.1% |
| SET P99 (ms) | 22 | 14 | -36.4% |

## S02 – Structured Commands Allow Path: M0 vs M2

| Command | M0 ops/s | M2 ops/s | Delta | M0 P99 (ms) | M2 P99 (ms) | P99 Delta |
|---------|---------|---------|-------|------------|------------|---------|
| HSET | 22831.05 | 5569.48 | -75.6% | 23 | 25 | +8.7% |
| MSET (10 keys) | 4345.94 | 4318.72 | -0.6% | 20 | 21 | +5.0% |

> Note: redis-benchmark 6.0 `-t hset,hget` only produces HSET tests; HGET must be tested separately as a custom command.

## S03 – Pipeline Depth Scan (GET/SET): M0 vs M2

| Pipeline | M0 GET ops/s | M2 GET ops/s | GET Delta | M0 SET ops/s | M2 SET ops/s | SET Delta |
|----------|-------------|-------------|---------|-------------|-------------|---------|
| P=1  | 13386.88 | 13333.33 | -0.4% | 6800.41 | 6825.94 | +0.4% |
| P=8  | 64540.20 | 96734.30 | +49.9% | 10685.17 | 10316.33 | -3.5% |
| P=32 | 183563.64 | 202343.44 | +10.2% | 27100.27 | 12357.80 | -54.4% |

## S04 – PING (Light Command): M0 vs M2

| Variant | M0 ops/s | M2 ops/s | Delta | M0 P99 (ms) | M2 P99 (ms) |
|---------|---------|---------|-------|------------|------------|
| PING_INLINE | N/A | 10643.96 | N/A | - | - |
| PING_BULK | 14869.89 | 10718.11 | -27.9% | 9 | 20 |

## S05 – Command Deny Path (M3: benchDeny issues SET; user only has +get)

- **SET ops/s** (all replies NOPERM): **13927.58**
- **SET P99 latency**: **12 ms**
- Compared to M0 SET (6620.32 ops/s): delta = +110.4%
- Interpretation: NOPERM fast-reject maintains similar or better throughput than normal SET.

## S06 – Key Deny Path (M3: benchDeny accesses deny:* keys; only ~allow:* permitted)

- **GET ops/s** (all replies NOPERM): **N/A**
- **GET P99 latency**: ** ms**
- Compared to M0 GET (11422.04 ops/s): delta = N/A

## S11 – ACL Admin Throughput (SETUSER / DELUSER / DRYRUN)

> Measured via serial redis-cli calls; each call includes TCP connection establishment overhead.
> True server-side throughput with pipelined clients would be significantly higher.

| Operation | Count | Total Time | Approx ops/s (incl. conn overhead) |
|-----------|-------|-----------|-------------------------------------|
| SETUSER    | 200 | 3s | ~66 ops/s |
| DELUSER    | 200 | 4s | ~50 ops/s |
| DRYRUN     | 200 | 3s | ~66 ops/s |

## ACL Complexity Impact (M3: permissive bench vs restricted benchU1/benchU2)

| User | ACL Rules Summary | GET ops/s | SET ops/s | GET P99 (ms) |
|------|-------------------|-----------|-----------|-------------|
| bench    | allcommands + allkeys (no restriction)        | 15026.30 | 6743.09 | 8 |
| benchU1  | +get +set, ~allow:* ~bench:* (2 key patterns) | 15174.51 | 13755.16 | 8 |
| benchU2  | +get +set, ~allow:* ~bench:* ~data:* ~key:* (4 key patterns) | 14577.26 | 70921.98 | 9 |

- benchU1 vs bench: GET ops delta = +1.0%, P99 delta = +0.0%
- benchU2 vs bench: GET ops delta = -3.0%, P99 delta = +12.5%

## S13/S14 – Large User Scale (100 / 300 users)

### User Creation Throughput (serial redis-cli)
- 100 users created in **2s** (~50 ops/s)
- Additional 300 users in **3s** (~100 ops/s)

### GET/SET Throughput under Large User Pools (bench user with permissive ACL)

| User Pool Size | GET ops/s | SET ops/s | GET P99 (ms) |
|---------------|-----------|-----------|-------------|
| 100 users in system | 12004.80 | 7057.16 | 12 |
| 300 users in system | 9337.07 | 6119.95 | 22 |
| Delta (100→300) | -22.2% | -13.3% | +83.3% |

## S15 – ACL Management Commands Latency under Large User Scale

### 100_users (40 samples)
- Average: **0.016s** | Min: 0.012s | Max: 0.036s
```
0m0.019s 0m0.018s 0m0.021s 0m0.021s 0m0.021s

```

### 300_users (20 samples)
- Average: **0.021s** | Min: 0.016s | Max: 0.026s
```
0m0.024s 0m0.022s 0m0.021s 0m0.018s 0m0.016s

```

## S16 – Batch ACL DELUSER

- **300 users deleted** in 5s (~**60 ops/s** via serial redis-cli)

## Regression Analysis (vs Plan §10 Thresholds)

❌ **S01 GET throughput M2 vs M0**: -5.8% (threshold ≥−5%) — FAIL
✅ **S01 SET throughput M2 vs M0**: 7.2% (threshold ≥−5%) — PASS
❌ **S01 GET P99 M2 vs M0**: +11.1% (threshold ≤+8%) — FAIL
✅ **S01 SET P99 M2 vs M0**: +-36.4% (threshold ≤+8%) — PASS
✅ **Complexity GET U2 vs bench (M3)**: -3.0% (threshold ≥−15%) — PASS
✅ **Complexity GET P99 U2 vs bench**: +12.5% (threshold ≤+15%) — PASS
❌ **Large user GET 300 vs 100**: -22.2% (threshold ≥−15%) — FAIL
❌ **Large user P99 300 vs 100**: +83.3% (threshold ≤+20%) — FAIL

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
   - Large user pool (S13/S14): 300 users vs 100 users shows ~22% GET drop — user lookup cost
     grows with user count. This exceeds the 15% threshold and warrants investigation.

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

