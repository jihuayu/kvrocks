# Kvrocks ACL 性能基准测试方案（含关键链路延迟变化）

## 1. 目标与核心问题

本方案用于回答两类问题：

1. ACL 自身性能：`AUTH`、`ACL SETUSER/DELUSER/GETUSER/DRYRUN/LOG` 等 ACL 功能在不同规模下的吞吐与延迟表现。
2. 关键链路延迟变化：开启 ACL 后，核心请求路径的延迟和吞吐相对“未开启 ACL”发生了多少变化，并拆解变化来自哪里。

重点关注以下路径：

1. 普通命令允许路径（read/write）
2. ACL 拒绝路径（command/key/channel deny）
3. 集群路径（ACL check + cluster redirect）
4. namespace 绑定路径（`user#ns`）
5. ACL 变更路径（尤其 `ACL DELUSER` 触发连接回收时）

---

## 2. 测试原则

1. 严格 A/B 对比：同一机器、同一二进制、同一数据集、同一负载，仅切换 ACL 相关变量。
2. 分层观测：
   - 黑盒：客户端端到端延迟和吞吐。
   - 白盒：`INFO commandstats`、`INFO ACL`、`PERFLOG`、CPU PMU 指标。
3. 区分“功能正确性”与“性能回归”：本方案只做性能基准，不替代 ACL 功能兼容测试。
4. 每个场景至少重复 5 次，使用中位数 + P95/P99 对比，避免单次抖动误判。

---

## 3. 测试环境建议

## 3.1 软硬件基线

1. 单租户压测机，固定 CPU 频率，关闭自动扩缩容。
2. `RelWithDebInfo` 构建，便于后续 `perf`/火焰图分析。
3. 压测端与服务端分离（推荐），或同机 loopback（次选）。

构建命令示例：

```bash
./x.py build -DCMAKE_BUILD_TYPE=RelWithDebInfo -j "$(nproc)"
```

## 3.2 实例拓扑

1. 单机：1 个 Kvrocks 实例（用于纯 ACL 开销和关键命令路径）。
2. 集群：3 master（用于 ACL + 路由重定向路径）。
3. namespace：至少 2 个 namespace（`default`、`team1`）。

---

## 4. ACL 配置与测试画像

## 4.1 服务端模式

定义 4 组固定模式：

1. `M0`：ACL 关闭（`acl-preview-enabled no`）
2. `M1`：ACL 开启，但使用管理员连接（用于衡量 ACL 开关存在时的最小额外开销）
3. `M2`：ACL 开启，宽松用户（`allcommands allkeys allchannels`）
4. `M3`：ACL 开启，受限用户（多 selectors + key/channel patterns）

## 4.2 用户复杂度分层（用于 M3）

1. `U1`：1 selector，2 key patterns，2 channels
2. `U2`：4 selectors，32 key patterns，16 channels
3. `U3`：16 selectors，128 key patterns，64 channels
4. `U4`：用户总量 2000（每用户 `U1` 规则）
5. `U5`：用户总量 5000（每用户 `U1` 规则）

通过分层可以观察 ACL 判定复杂度对延迟曲线的影响。

---

## 5. 负载与场景矩阵

建议最少覆盖下表场景（`Sxx`）：

| ID | 场景 | 目标 |
|---|---|---|
| S01 | `GET/SET` 允许路径（M0 vs M2） | 量化“开启 ACL 后”主链路开销 |
| S02 | `MGET/HGET/HSET` 允许路径（M0 vs M2） | 多 key/结构化命令路径开销 |
| S03 | pipeline 深度扫描（P=1/8/32） | ACL 开销在高吞吐下的放大情况 |
| S04 | `PING`/轻命令路径（M0 vs M2） | 观察 ACL 对短命令的相对影响 |
| S05 | command deny（如仅允许 `+get` 后压 `SET`） | 拒绝路径延迟与吞吐 |
| S06 | key deny（`~allow:*`，访问 `deny:*`） | key pattern 匹配开销 |
| S07 | channel deny（`&news:*`，访问 `sports:*`） | channel 匹配与拒绝开销 |
| S08 | 集群本地槽命中（ACL allow） | 集群正常路径开销 |
| S09 | 集群重定向路径（ACL allow） | ACL + redirect 组合延迟 |
| S10 | namespace 用户（`user#team1`）允许路径 | namespace 绑定对关键链路影响 |
| S11 | `ACL SETUSER/DELUSER` 高频变更 | ACL 管理面吞吐/延迟 |
| S12 | `ACL DELUSER` + 大量在线连接 | 连接回收对尾延迟的冲击 |
| S13 | ACL 用户规模 2000（随机用户认证 + 读写） | 用户索引规模对认证与请求路径影响 |
| S14 | ACL 用户规模 5000（随机用户认证 + 读写） | 更大规模用户对尾延迟与内存影响 |
| S15 | 2000/5000 用户下 `ACL USERS/LIST/GETUSER` | 管理面枚举与查询在大规模下的开销 |
| S16 | 2000/5000 用户下批量 `ACL DELUSER` | 删除风暴对业务链路冲击与收敛时间 |

---

## 6. 压测工具与执行方式

## 6.1 数据面压测（推荐 memtier + redis-benchmark）

1. `redis-benchmark`：快速测单命令路径和 pipeline 变化。
2. `memtier_benchmark`：更适合集群与混合读写。

`redis-benchmark` 示例（单机允许路径）：

```bash
redis-benchmark -h 127.0.0.1 -p 6666 \
  -n 5000000 -c 128 -P 16 -d 64 -r 1000000 \
  -t get,set --csv
```

ACL 用户示例（如果本机 redis-benchmark 支持用户名参数）：

```bash
redis-benchmark -h 127.0.0.1 -p 6666 \
  --user bench -a benchpass \
  -n 5000000 -c 128 -P 16 -d 64 -r 1000000 \
  -t get,set --csv
```

`memtier_benchmark` 示例（集群）：

```bash
memtier_benchmark \
  --cluster-mode \
  --server 127.0.0.1 --port 6666 \
  --threads 8 --clients 64 --test-time 120 \
  --ratio=1:1 --data-size=64 \
  --key-pattern=R:R --key-minimum=1 --key-maximum=5000000
```

说明：不同版本的 memtier 认证参数命名可能不同（如 user/password 参数），以 `memtier_benchmark --help` 为准。

## 6.2 ACL 管理面压测

管理面建议单独测：

1. `ACL SETUSER` 连续创建/更新
2. `ACL DELUSER` 连续删除
3. `ACL DRYRUN` 高频请求

示例（批量创建）：

```bash
/usr/bin/time -f "elapsed=%E cpu=%P" bash -c '
for i in $(seq 1 20000); do
  redis-cli -p 6666 ACL SETUSER "u${i}" on ">pass" allcommands allkeys > /dev/null
done
'
```

超大规模用户样例（2000/5000）：

```bash
# 2000 users
/usr/bin/time -f "elapsed=%E cpu=%P rss_kb=%M" bash -c '
for i in $(seq 1 2000); do
  redis-cli -p 6666 ACL SETUSER "bench:u${i}" on ">p${i}" +get +set "~u${i}:*" "&ch${i}:*" > /dev/null
done
'

# 5000 users
/usr/bin/time -f "elapsed=%E cpu=%P rss_kb=%M" bash -c '
for i in $(seq 1 5000); do
  redis-cli -p 6666 ACL SETUSER "bench:u${i}" on ">p${i}" +get +set "~u${i}:*" "&ch${i}:*" > /dev/null
done
'
```

大用户规模认证与数据面混合负载（S13/S14）建议：

1. 准备用户池文件（2000 或 5000）。
2. 压测客户端按固定比例随机挑选用户做短连接 `AUTH` 或长连接复用。
3. 命令组合使用 `GET/SET`，key 采用“命中本人 pattern”和“跨用户 deny”混合。
4. 对比 3 组模式：`M0`（ACL off）、`M2`（单用户）、`U4/U5`（多用户）。

推荐比例：

1. `80%` 允许请求（命中 `~u${i}:*`）
2. `20%` 拒绝请求（访问其他用户 key，产生 `NOPERM`）

S15 管理面查询示例：

```bash
# ACL USERS latency sample
for i in $(seq 1 20); do
  /usr/bin/time -f "users_elapsed=%e" redis-cli -p 6666 ACL USERS > /dev/null
done

# ACL LIST latency sample
for i in $(seq 1 20); do
  /usr/bin/time -f "list_elapsed=%e" redis-cli -p 6666 ACL LIST > /dev/null
done

# Random ACL GETUSER sample
for i in $(seq 1 200); do
  uid=$((1 + RANDOM % 5000))
  redis-cli -p 6666 ACL GETUSER "bench:u${uid}" > /dev/null
done
```

S16 批量删除示例：

```bash
# delete 2000 users
/usr/bin/time -f "elapsed=%E cpu=%P rss_kb=%M" bash -c '
for i in $(seq 1 2000); do
  redis-cli -p 6666 ACL DELUSER "bench:u${i}" > /dev/null
done
'

# delete 5000 users
/usr/bin/time -f "elapsed=%E cpu=%P rss_kb=%M" bash -c '
for i in $(seq 1 5000); do
  redis-cli -p 6666 ACL DELUSER "bench:u${i}" > /dev/null
done
'
```

## 6.3 `ACL DELUSER` 连接回收冲击测试

1. 先建立 N 个长连接并认证为同一 ACL 用户（如 N=1k/5k/10k）。
2. 并发执行常规业务压测（例如 `GET`）。
3. 中途执行 `ACL DELUSER <user>`。
4. 记录：
   - `ACL DELUSER` 自身延迟
   - 业务流量 P99/P999 抖动窗口
   - 实际断连数量和收敛时间

该场景用于评估 KillClient 策略对可用性和尾延迟的影响。

---

## 7. 观测指标

## 7.1 客户端指标

1. 吞吐：ops/s
2. 延迟：P50/P95/P99/P999（微秒）
3. 错误率：`NOPERM`、网络错误、重定向错误占比

## 7.2 服务端指标

1. `INFO commandstats`：`usec_per_call` 与调用量变化
2. `INFO ACL`：
   - `acl_access_denied_auth`
   - `acl_access_denied_cmd`
   - `acl_access_denied_key`
   - `acl_access_denied_channel`
3. `INFO cpu`：`used_cpu_sys/user`、worker CPU time
4. `PERFLOG GET`：命令执行体耗时和 RocksDB perf context
5. 大规模用户专项：
   - `INFO memory` 的 `used_memory` 增量（2000 vs 5000）
   - `INFO commandstats` 中 `acl`、`auth`、`get`、`set` 的 `usec_per_call`

## 7.3 系统级指标

1. `perf stat`：`cycles/instructions/branches/branch-misses/cache-misses`
2. `pidstat`/`mpstat`：CPU 利用率与上下文切换

示例：

```bash
pid=$(pgrep -x kvrocks | head -n1)
perf stat -p "$pid" -e cycles,instructions,branches,branch-misses,cache-misses -- sleep 60
```

---

## 8. 关键链路延迟拆解方法

`PERFLOG` 记录的是命令 `Execute()` 时间，不包含 ACL 校验等前置阶段。  
为了测出 ACL 对关键链路的影响，建议采用“双视角差分”：

1. 端到端延迟：来自压测工具。
2. 执行体延迟：来自 `PERFLOG` 或 `INFO commandstats` 的 `usec_per_call`。
3. 前置链路近似开销：

```text
PreExecDelta ≈ (E2E_ACL_ON - E2E_ACL_OFF) - (Exec_ACL_ON - Exec_ACL_OFF)
```

`PreExecDelta` 主要反映：ACL 检查、命令分发前校验、路由前判定等变化。

注意：`PERFLOG` 本身有观测扰动，建议分两轮：

1. 主测轮：`profiling-sample-ratio=0`（无探针干扰）
2. 拆解轮：仅针对少数命令开启低采样率（例如 1%）复测

---

## 9. 执行步骤（可落地）

1. 准备 3 份配置：`M0/M2/M3`（单机），另备集群配置。
2. 预热数据集（至少 2 倍内存工作集和 1 倍热点工作集各一轮）。
3. 逐场景执行 `S01~S12`，每场景：
   - warmup 30s
   - run 120s
   - repeat 5 次
4. 每次 run 后采集：
   - benchmark 原始输出
   - `INFO all` 快照
   - `PERFLOG GET`（如果启用）
   - `perf stat` 输出
5. 汇总并计算回归比例，生成场景级结论。

---

## 10. 回归判定建议（可作为 CI 门禁）

可采用以下默认阈值（首版建议）：

1. 允许路径（M2 对比 M0）：
   - 吞吐下降不超过 5%
   - P99 延迟上升不超过 8%
2. 复杂 ACL（U3 对比 U1）：
   - P99 上升不超过 15%
3. 拒绝路径（S05/S06/S07）：
   - 不出现随时间持续恶化（内存或锁争用迹象）
4. `ACL DELUSER` 冲击（S12）：
   - 业务请求可恢复，且抖动窗口可观测、可收敛
5. 超大用户规模（S13~S16）：
   - 5000 用户相对 2000 用户，`AUTH` P99 上升不超过 20%
   - 5000 用户下 `GET/SET` 允许路径 P99 相对单用户模式上升不超过 15%
   - `ACL USERS/LIST` 在 5000 用户规模不应出现秒级长尾（建议 P99 < 500ms）
   - 批量 `DELUSER` 期间业务链路允许短时抖动，但应在压测窗口内恢复稳定

阈值应在第一次完整基线跑完后按真实业务 SLO 调整。

---

## 11. 结果产出模板

每轮压测建议产出以下文件：

1. `summary.md`：场景结论和回归判断
2. `raw/<scenario>/benchmark.txt`：压测原始结果
3. `raw/<scenario>/info.txt`：`INFO` 快照
4. `raw/<scenario>/perf_stat.txt`：系统 PMU 数据
5. `charts/*.png`：P50/P95/P99、吞吐、CPU 对比图

最少应回答三件事：

1. ACL 功能本身的管理面性能上限是多少？
2. 开启 ACL 后关键业务命令延迟增加了多少？
3. 增量主要来自 ACL 判定、执行体，还是集群/namespace 交互链路？

---

## 12. 后续可演进项

1. 增加 ACL 专项 microbenchmark（仅测 `CheckAclCommandAllowed`，剥离网络与存储噪声）。
2. 增加火焰图对比（M0 vs M2 vs M3）定位热点函数。
3. 将 `S01/S05/S12` 三个高价值场景纳入周期性性能回归任务。
