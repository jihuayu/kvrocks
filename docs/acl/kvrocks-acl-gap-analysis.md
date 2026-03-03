# Kvrocks ACL 与 Redis 设计差异清单（含优先级）

本文对照以下文档与本地实现：

- `docs/acl/redis-acl-design.md`
- `docs/acl/redis-acl-resp3.md`

目标：明确当前差异、分级优先级，并记录本次已经完成的 P0 修复。

## 1. 差异分级

### P0（安全与核心语义）

1. ACL 运行时校验原先仅检查命令位图，缺少 selector OR + key/channel 校验链路。
2. `ACL SETUSER` 中 category/key/channel/selector modifier 原先解析后并未真正生效。
3. 用户槽位分配存在下溢风险：满槽时 `size_t` 返回 `-1` 导致边界错误。
4. ACL 用户认证路径对 `nopass/default` 语义覆盖不足（尤其 `AUTH <username> <password>` 与 `AUTH <password>` 的 default 分支）。

### P1（协议与兼容性）

1. ACL 子命令覆盖不完整（仍缺 `HELP/CAT/LIST/DELUSER/GENPASS/LOG/DRYRUN/LOAD/SAVE`）。
2. 拒绝错误码未对齐 Redis 的 `NOPERM`。
3. `GETUSER` 字段格式和 Redis 仍有差异（当前仍是 Kvrocks preview 兼容形态）。
4. ACL 校验与 Cluster 重定向先后顺序未完全对齐 Redis。

### P2（生态与运维）

1. ACL 与复制/传播策略和 Redis 语义不同（Kvrocks 目前会通过 propagate 复制 ACL 变更）。
2. ACL LOG / metrics / TLS cert ACL 观测能力不完整。

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

## 3. 当前状态说明

虽然 P0 关键路径已补齐，但实现仍处于 `acl-preview-enabled` 范围内，尚未达到 Redis ACL 的全量协议兼容；后续建议按 P1 列表继续推进。

