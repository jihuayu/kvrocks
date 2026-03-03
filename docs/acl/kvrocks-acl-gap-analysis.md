# Kvrocks ACL 与 Redis 设计差异清单（含优先级）

本文对照以下文档与本地实现：

- `docs/acl/redis-acl-design.md`
- `docs/acl/redis-acl-resp3.md`

目标：明确当前差异、分级优先级，并记录本次已经完成的 P0 修复。

## 1. 差异分级

### 约束与非目标（已确认）

以下两项为当前项目的**明确限制**，不计入“需对齐 Redis 的缺口”：

1. `ACL` 容器命令保持 `admin-only`（仅管理员连接可执行），不放开给普通 ACL 用户。
2. `+cmd|sub` / `-cmd|sub` 子命令级授权暂缓实现（当前按整命令位图授权）。

### P0（安全与核心语义）

1. ACL 运行时校验原先仅检查命令位图，缺少 selector OR + key/channel 校验链路。
2. `ACL SETUSER` 中 category/key/channel/selector modifier 原先解析后并未真正生效。
3. 用户槽位分配存在下溢风险：满槽时 `size_t` 返回 `-1` 导致边界错误。
4. ACL 用户认证路径对 `nopass/default` 语义覆盖不足（尤其 `AUTH <username> <password>` 与 `AUTH <password>` 的 default 分支）。

### P1（协议与兼容性）

1. `ACL LOG` 仍未实现完整事件记录/聚合/计数语义（当前仅有基础占位能力）。
2. `ACL LOAD/SAVE` 仍未接入真实 ACL 文件解析与持久化（当前返回“未配置 ACL file”错误）。
3. ACL 校验与 Cluster 重定向先后顺序未完全对齐 Redis。

### P2（生态与运维）

1. ACL 与复制/传播策略和 Redis 语义不同（Kvrocks 目前会通过 propagate 复制 ACL 变更）。
2. ACL LOG / metrics / TLS cert ACL 观测能力不完整。
3. `+cmd|sub` / `-cmd|sub` 子命令粒度授权（延期项，非当前阶段目标）。

## 2. 本次已完成的 P0 修复

### 2.1 运行时 ACL 校验链路补齐

- 增加 selector OR 语义检查。
- 增加 key 权限检查（按命令 key-range 抽 key，结合 `%R/%W/~` 规则）。
- 增加 channel 权限检查（`SUBSCRIBE/SSUBSCRIBE/PUBLISH/SPUBLISH` 使用 glob，`PSUBSCRIBE` 使用字面量匹配）。

涉及文件：

- `src/server/redis_connection.cc`
- `src/server/redis_connection.h`

### 2.2 `ACL SETUSER` 的 category/key/channel/selector 真正生效

- `+@category/-@category` 可作用于命令位图。
- `allkeys/resetkeys/~.../%R~/%W~` 可作用于 root selector。
- `allchannels/resetchannels/&...` 可作用于 root selector。
- `(...)` selector 创建可生效，并限制 selector 内仅允许 command/key/channel modifier。
- 支持跨参数的 selector 合并（`( ... )` 可拆分到多个 argv）。

涉及文件：

- `src/server/acl/acl_actions.cc`
- `src/server/acl/acl_actions.h`
- `src/server/acl/acl_command_manager.cc`
- `src/server/acl/acl_command_manager.h`
- `src/server/acl/acl.cc`

### 2.3 用户槽位边界 bug 修复

- `AclUserManager::findFreeSlotLocked` 改为 `std::optional<size_t>`。
- `SetUser/AddUser` 在无空槽时正确返回失败。

涉及文件：

- `src/server/acl/acl_user.cc`
- `src/server/acl/acl_user.h`

### 2.4 认证路径的 default/nopass 关键语义修复

- 用户名认证分支：`nopass` 用户可通过 `AUTH <username> <password>` 登录（仅要求用户启用）。
- `AUTH <password>` 增加 default ACL 用户路径（含密码校验分支）。
- 修复无用户名认证时 ACL profile 名称错误绑定（不再把密码当用户名）。

涉及文件：

- `src/server/server.cc`
- `src/commands/cmd_server.cc`

### 2.5 本轮 P1 对齐项（已完成）

- 补齐 ACL 子命令覆盖：`HELP/CAT/LIST/DELUSER/GENPASS/LOG/DRYRUN/LOAD/SAVE`。
  - 其中 `LOAD/SAVE` 在未配置 ACL 文件时返回明确错误，`LOG` 先提供 `RESET` 与空列表占位。
- ACL 拒绝错误码对齐为 `NOPERM`。
- `default off` 后新连接未认证访问返回 `NOAUTH`，`HELLO 3` 无认证链时返回 `NOAUTH`。
- `ACL SETUSER` 支持 `sanitize-payload` / `skip-sanitize-payload`，并在 `GETUSER flags` 中回显。
- 增加用户名合法性校验（拒绝空白/控制字符）。
- `ACL GETUSER` 去除 `namespace` 字段，向 Redis 输出结构靠拢。

涉及文件：

- `src/commands/cmd_server.cc`
- `src/common/status.h`
- `src/server/redis_reply.cc`
- `src/server/redis_connection.cc`
- `src/server/acl/acl.cc`
- `src/server/acl/acl_actions.cc`
- `src/server/acl/acl_actions.h`
- `src/server/acl/acl_user.cc`
- `src/server/acl/acl_user.h`
- `tests/gocase/unit/acl/acl_test.go`

## 3. 当前状态说明

虽然 P0 关键路径和本轮 P1 对齐项已补齐，但实现仍处于 `acl-preview-enabled` 范围内，尚未达到 Redis ACL 的全量协议兼容；后续建议按剩余 P1/P2 列表继续推进。
