# Redis ACL 设计与实现剖析（基于本仓库源码）

本文档从实现角度解释 Redis ACL 在本仓库代码中的落地方式：代码结构、数据结构、规则解析与校验链路、持久化/加载策略，以及与集群/复制/脚本/模块等子系统的交互点。

目标读者：希望在自己的协议/服务端中复刻 Redis ACL 行为的人（尤其是兼容 `ACL` 命令与权限校验语义）。

范围说明：
- 以本仓库源码为准（核心文件：`src/acl.c`）。
- 强调“实现机制”和“交互边界”，不重复 `ACL` 命令的 RESP3 协议细节（协议细节见 `docs/acl/redis-acl-resp3.md`）。
- 不建议复刻已标记为 deprecated 的 `+<command>|<first-arg>` 能力；本文也不将其作为设计目标。

---

## 1. 代码结构（关键文件与职责）

- `src/acl.c`
  - ACL 核心：用户/selector 数据结构、规则解析与应用、权限校验、ACL 文件 LOAD/SAVE、ACL LOG、`aclCommand()`（`ACL` 容器命令实现）。
- `src/server.h`
  - `user` 结构体、ACL 返回码（`ACL_DENIED_*`）、ACL 类别 bit（`ACL_CATEGORY_*`）、selector/user flags 等常量。
- `src/server.c`
  - 命令表的 ACL 类别推导（`setImplicitACLCategories()`）、命令执行主流程中 ACL 校验位置（`processCommand()`）。
- `src/networking.c`
  - 连接认证状态机：`clientSetDefaultAuth()`、`authRequired()`；TLS 自动认证挂点；`deauthenticateAndCloseClient()`。
- `src/db.c`
  - 从命令参数中抽取 keys/channels 的通用逻辑（`getKeysFromCommandWithSpecs()`、`getChannelsFromCommand()`），ACL 的 key/channel 校验依赖这些抽取结果。
- `src/multi.c`
  - `EXEC` 执行阶段二次 ACL 校验（避免队列期间 ACL 被改）。
- `src/script.c` / `src/script_lua.c`
  - 脚本环境内 ACL 校验与辅助 API（如 `redis.acl_check_cmd()`）。
- `src/module.c`
  - 模块加载/卸载后，若模块命令带 ACL categories，会触发全量重算（`ACLRecomputeCommandBitsFromCommandRulesAllUsers()`）。
- `src/cluster_legacy.c` / `src/cluster_asm.c`
  - Cluster 内部 secret 与 internal auth（内部连接绕过 ACL）的实现与使用。
- `src/replication.c`
  - 复制链路 master client 设置为 `user = NULL`（无限权限）。

---

## 2. 数据模型

### 2.1 `user`（全局 Users 表的值）

定义见 `src/server.h`，核心字段：
- `name`：用户名（SDS，大小写敏感）。
- `flags`：`USER_FLAG_*`（启用/禁用、`nopass`、RESTORE payload sanitization 等），以 atomic 形式存取。
- `passwords`：密码 hash 列表（每个元素为 64 字符小写 hex 的 SHA256）。
- `selectors`：selector 列表（至少包含一个 **root selector**）。
- `acl_string`：缓存的“可复现 ACL 字符串”，用于 `ACL LIST`/`ACL SAVE` 等输出；修改用户规则时会被置空失效。

全局容器：
- `Users`：`rax`（radix tree），key 为用户名，value 为 `user*`。
- `DefaultUser`：默认用户（新连接默认绑定的 user）。

### 2.2 `aclSelector`（用户权限的逻辑单元）

`aclSelector` 是 `src/acl.c` 私有结构（不暴露在头文件），核心字段：
- `flags`：`SELECTOR_FLAG_*`（`ALLCOMMANDS/ALLKEYS/ALLCHANNELS/ROOT` 等）。
- `allowed_commands[]`：命令位图（长度固定 `USER_COMMAND_BITS_COUNT/64`）。
- `allowed_firstargs`：first-arg 例外机制（deprecated，不建议复刻）。
- `patterns`：key pattern 列表（元素类型 `keyPattern`，支持读/写权限）。
- `channels`：频道 pattern 列表（元素为 SDS）。
- `command_rules`：命令/类别规则串（保持“左到右”的应用顺序），用于重算位图。

设计要点：
- 同一个 `user` 可拥有多个 selector。
- 运行时“是否允许执行”采用 **selectors 的 OR**：任意一个 selector 通过即放行。
- root selector 永远存在，保证向后兼容“单 ACL 集合”的行为。

### 2.3 命令 ID 与 “未来命令”语义

为了快速判定“某用户是否允许某命令”，ACL 使用命令位图；每个命令（含子命令）都有一个稳定的 `cmd->id`：
- `ACLGetCommandID()` 为命令全名分配递增 ID，并保持同名命令 ID 稳定（模块卸载再加载仍可复用）。
- `USER_COMMAND_BITS_COUNT-1` 这个 bit 被保留为“future commands”标识：
  - 当 selector 以 `+@all`（或别名 `allcommands`）建立权限时，该保留 bit 会被置位；
  - 表示该用户将自动允许未来通过模块加载的命令。
  - 若 selector 基于 `-@all` 再逐项添加，则未来模块命令默认不允许。

这直接影响 `ACLDescribeSelectorCommandRules()` 在输出规则时选择以 `+@all` 还是 `-@all` 开头。

---

## 3. 认证模型（authentication）与权限模型（authorization）

### 3.1 连接的认证状态

新连接创建时：
- `c->user` 先指向 `DefaultUser`；
- `c->authenticated` 由 `DefaultUser` 是否 `nopass` 且未禁用决定。
  - default 用户 `nopass`：新连接“自动已认证”。
  - 否则需显式 `AUTH` 或 `HELLO ... AUTH ...`。

是否需要认证由 `authRequired(c)` 判定（`src/networking.c`）。

### 3.2 `AUTH` 与用户绑定

`AUTH <password>` 与 `AUTH <username> <password>` 的认证流程在 `src/acl.c`：
- 密码以 SHA256(hex) 形式存储，比较使用常量时间比较函数（避免 timing leak）。
- 成功后：
  - `c->authenticated = 1`
  - `c->user = <对应 user*>`

特殊分支：**内部连接（internal connection）**
- `AUTH "internal connection" <secret>` 会进入 internal auth：
  - 成功后 `c->flags |= CLIENT_INTERNAL`
  - `c->authenticated = 1`
  - `c->user = NULL`（见下文“无限权限用户”）
- 该用户名包含空格，而普通 ACL 用户名禁止包含空白字符，避免冲突。

### 3.3 “无限权限用户”：`c->user == NULL`

ACL 权限校验函数在一开始就有短路：
- 当 `user* u == NULL` 时，ACL 直接返回允许。

因此这些连接天然绕过 ACL：
- 复制链路 master client（`src/replication.c` 中创建后设置 `server.master->user = NULL`）。
- cluster internal auth 成功后的 internal client。
- 部分集群迁移/内部任务也会直接构造 `CLIENT_INTERNAL` 且 `user = NULL` 的 client。

复刻时的建议：
- 把“内部通道/内部 RPC”与外部客户端认证严格区分；内部身份绕过 ACL 是 Redis 的关键设计之一。

---

## 4. 权限校验链路（命令执行时发生了什么）

### 4.1 `processCommand()` 中的顺序非常关键

在 `src/server.c` 的 `processCommand()` 中，关键顺序为：
1. `authRequired(c)`：未认证时，仅允许 `CMD_NO_AUTH` 的命令，否则返回 `NOAUTH`。
2. `ACLCheckAllPerm(c,&acl_errpos)`：ACL 拒绝则返回 `-NOPERM ...` 并写入 ACL LOG。
3. **之后** 才执行 cluster 路由（MOVED/ASK 重定向逻辑）。

影响：
- 对 cluster 客户端而言，请求落到错误节点时，也会先经过“当前节点的 ACL”检查再被重定向。
- 如果各节点 ACL 不一致，可能出现“尚未重定向就被拒绝”的现象。

### 4.2 核心校验函数

入口：
- `ACLCheckAllPerm(client *c, int *idxptr)`

实际逻辑：
- `ACLCheckAllUserCommandPerm(user *u, redisCommand *cmd, robj **argv, int argc, ..., int *idxptr)`

selector 级校验：
- `ACLSelectorCheckCmd(aclSelector *selector, redisCommand *cmd, ...)`
  - 命令权限：命令位图 + `SELECTOR_FLAG_ALLCOMMANDS`
  - key 权限：从参数提取 keys，逐个 glob/prefix 匹配 key patterns
  - channel 权限：从参数提取 channels，逐个 glob/字面规则匹配 channel patterns

多个 selector 的语义：
- 逐个 selector 尝试，只要有一个 selector 返回 `ACL_OK` 即放行；
- 若全失败，需要选择一个“最相关错误”用于日志与报错位置（实现里会在 `ACL_DENIED_CMD/KEY/CHANNEL` 之间择优）。

### 4.3 key 抽取与读写权限

ACL 不“硬编码每个命令哪些参数是 key”，而是依赖命令表的 key-spec 定义：
- `getKeysFromCommandWithSpecs()` 返回 `keyReference[]`（含 key 在 argv 中的位置、以及访问标记 flags）。
- ACL 将这些访问标记映射为 key 权限需求：
  - `CMD_KEY_ACCESS` -> 读权限
  - `CMD_KEY_INSERT/DELETE/UPDATE` -> 写权限
  - `CMD_KEY_PREFIX` -> 使用 prefixmatch（前缀匹配）而不是 glob

因此，复刻 ACL 的一个硬前置条件是：你也要有一套“可从命令元数据抽取 key/channel 参数”的机制，否则无法做到与 Redis 一致的精度。

### 4.4 channel 抽取与匹配规则

类似 key，ACL 对频道也走统一抽取：
- `getChannelsFromCommand()` 返回 channels 的位置与 flags（publish/subscribe/pattern 等）。

匹配要点：
- `SUBSCRIBE/PUBLISH` 这类“普通频道名”允许用 glob pattern 匹配 ACL 列表。
- `PSUBSCRIBE` 传入的是“频道 pattern”，实现要求其与 ACL 列表做 **字面匹配**（防止 pattern 再被 pattern 匹配产生歧义/扩大权限）。

---

## 5. 规则解析与应用（`ACL SETUSER` 的核心设计）

### 5.1 “全量原子更新”策略（避免半成功）

`ACL SETUSER` 实现使用 staging 用户实现原子性：
1. 创建临时 `tempu`（不挂到 `Users` 表）
2. 如果目标用户已存在，先把现有用户内容复制到 `tempu`
3. 依次对 `tempu` 应用所有 modifier；任意一步失败则整体失败
4. 全部成功后，再 `ACLCopyUser(u,tempu)` 覆盖目标用户

效果：不会出现“前半段 modifier 已生效，后半段报错”的风险。

### 5.2 selector 的括号语法与参数合并

selectors 支持括号形式：
- `ACL SETUSER alice (...) (...) ...`
- 实现允许括号内容被拆分在多个 argv 中（为了更易用），会在应用前用 `ACLMergeSelectorArguments()` 合并：
  - 从一个以 `(` 开始但不以 `)` 结尾的 token 开始，持续拼接直到遇到 `)` 结尾的 token。
  - 括号不匹配会直接报错并不生效。

### 5.3 命令重命名与 ACL 的解耦

ACL 在解析 `+cmd/-cmd/+@cat` 时使用 `server.orig_commands` 做查找：
- 即使配置了 `rename-command`，ACL 仍按“原始命令名/子命令名”授权。
- 这样避免“重命名导致 ACL 规则失效或被绕过”的风险。

---

## 6. 对已有连接的影响（动态变更后的处理）

ACL 变更可能影响已建立连接：
- `ACL DELUSER`：删除用户时会遍历所有连接，凡是 `c->user == 被删用户` 的连接调用 `deauthenticateAndCloseClient()`。
- `ACL SETUSER`：当修改的是已有用户时，如果收紧了 pubsub 频道权限，会主动踢掉不再合规的订阅连接（避免继续收发消息）。
- `ACL LOAD`：从文件整体替换 Users 表时：
  - 通过“拷贝新 default 用户配置到旧 DefaultUser 对象”保持 `DefaultUser` 指针稳定；
  - 重新将在线客户端的 `c->user` 指针按用户名映射到新 Users 表里的 `user*`；
  - 若用户消失或 pubsub 权限不再满足，则 `deauthenticateAndCloseClient()`。

复刻时建议把“ACL 变更对长连接的影响策略”作为显式设计点，否则很容易出现安全洞（例如 pubsub 权限收紧后旧连接仍保持订阅）。

---

## 7. ACL LOG 与可观测性

### 7.1 ACL LOG 的语义

ACL 拒绝（命令/键/频道）与认证失败会写入 ACL LOG（可被 `ACL LOG` 查询）。

实现要点：
- 日志条目会做聚合：在有限窗口内（60s）相同 `(reason, context, object, username)` 的事件会合并计数。
- `acllog-max-len` 控制内存上限；若为 0，日志不会保留，但相关 metric 仍会累加。

### 7.2 INFO 统计

`server.acl_info.*` 维护 ACL 拒绝计数（auth/cmd/key/channel/tls-cert），并在 `INFO` 中输出。

---

## 8. 与 Cluster 的交互（最容易踩坑）

### 8.1 ACL 不会通过 cluster bus 自动同步

cluster 协议本身并不传播 Users/ACL 规则。

后果：
- `ACL SETUSER/DELUSER` 在集群里是 **节点本地配置变更**；
- 若你希望集群范围一致，需要你自己的配置分发/集中管理方案（例如下发同一份 `aclfile` 到每个节点并执行 `ACL LOAD`）。

### 8.2 ACL 校验发生在 MOVED/ASK 重定向之前

命令到达错误节点时，该节点仍会先执行 ACL 校验；若本节点 ACL 更严格，可能在重定向前就拒绝请求。

因此：
- 集群对外提供服务时，应保证所有节点 ACL 一致，否则客户端可能出现非预期拒绝。

### 8.3 internal auth：集群内部连接如何绕过 ACL

集群内部使用一个“internal secret”实现节点间认证：
- secret 在 cluster 初始化时生成，并会在 PING 扩展中交换、收敛（实现选择更小的 secret 以便一致）。
- ASM 等内部流程会先向对端发送：
  - `AUTH "internal connection" <secret>`
- internal auth 成功后连接变为 `CLIENT_INTERNAL` 且 `user = NULL`，从而绕过 ACL。

复刻时建议：
- 明确提供内部认证通道，避免内部迁移/复制任务被 ACL 限制而失败；
- 同时要保证内部 secret 的分发/轮转有一致性策略。

---

## 9. 与复制（Replication）/AOF 的关系

ACL 属于“配置/安全策略”，不是数据集的一部分：
- 从命令 flags 看，`ACL` 子命令并不属于写入 keyspace 的写命令（不依赖 `CMD_WRITE/CMD_MAY_REPLICATE`），也未显式 `alsoPropagate()`。
- 复制链路上的 master client 直接设置 `user = NULL`，用于无条件执行来自 master 的数据更新流。

实际工程含义：
- 不应期望 ACL 规则通过复制自动分发；
- 如果你会把 replica 暴露给客户端（读流量），需要确保 replica 的 ACL 也被同样配置。

---

## 10. 与脚本 / 事务 / 模块 的交互点

### 10.1 MULTI/EXEC

事务的 ACL 校验不只发生在排队阶段：
- `EXEC` 执行每条 queued command 时会再次执行 ACLCheck（防止排队后 ACL 被改造成越权）。

### 10.2 Lua / Functions

脚本环境会进行 ACL 校验；且 Lua 侧提供 `redis.acl_check_cmd()`，用于在脚本里做“对某命令的权限模拟判定”（不执行命令，只返回 boolean）。

### 10.3 Modules

模块命令可以带 ACL categories（并可动态增加 category）。
当模块加载/卸载导致命令集合变化时，Redis 会基于 selector 的 `command_rules` 字符串重算位图（保证原规则顺序不丢失，且避免未来命令误放行/误拒绝）。

---

## 11. 复刻实现的关键建议（按风险排序）

1. **先建“命令元数据系统”**：没有 keyspec/channelspec 抽取，就无法正确实现 key/channel ACL。
2. **把 internal identity 设计清楚**：内部连接绕过 ACL 是集群/复制可靠运行的前提之一。
3. **保持原子更新**：`SETUSER`/`LOAD` 必须避免半成功，否则容易形成安全洞。
4. **处理在线连接的历史状态**：尤其是 pubsub 权限收紧后的踢人逻辑。
5. **考虑模块/插件带来的命令集合变化**：要么禁止动态命令，要么实现“规则串 + 重算位图”的机制。

