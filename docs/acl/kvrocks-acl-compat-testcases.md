# Kvrocks ACL 协议兼容测试用例（基于 Redis 源码与集成测试）

## 1. 目标与范围

- 目标：为 `kvrocks` 实现 Redis ACL（含 selectors）提供一份可直接落地的兼容性用例文档。
- 基线来源：`tests/unit/acl.tcl` + `tests/unit/acl-v2.tcl`。
- 当前基线规模：`106 + 34 = 140` 个测试。
- 错误断言策略：以错误类型和关键字为主（`NOPERM`/`WRONGPASS`/`NOAUTH`/`ERR`），不强制完整文案逐字一致。

> 已确认约束（非缺陷）：
> 1) `ACL` 容器命令保持 `admin-only`，普通 ACL 用户不放开执行 `ACL` 子命令；
> 2) `+cmd|sub` / `-cmd|sub` 子命令粒度授权暂缓推进，相关 Redis 兼容用例先不作为当前阶段目标。

## 2. Redis 现有用例矩阵（按能力分组）

> 说明：这里按“实现能力”聚合，便于在 kvrocks 里按模块推进；完整测试名索引见文末附录。

| 分组 | 能力点 | 主要来源 |
|---|---|---|
| 用户与认证基础 | `ACL SETUSER/USERS/WHOAMI`、启用/禁用、密码增删、hash 密码 | `tests/unit/acl.tcl:2` `tests/unit/acl.tcl:67` |
| 命令/Key/Channel 默认拒绝 | 新用户默认无命令/无 key/channel 权限 | `tests/unit/acl.tcl:73` `tests/unit/acl.tcl:142` |
| PubSub 与 Sharded PubSub | `SUBSCRIBE/PSUBSCRIBE/SSUBSCRIBE`、`PUBLISH/SPUBLISH`、`resetchannels/allchannels` | `tests/unit/acl.tcl:158` `tests/unit/acl.tcl:341` |
| 权限变更后连接处理 | 订阅者因权限收紧被踢、阻塞命令重处理拒绝 | `tests/unit/acl.tcl:250` `tests/unit/acl.tcl:346` |
| 命令类别与子命令 | `+@cat/-@cat`、`+cmd/-cmd`（`cmd|sub` 延期）、`ACL CAT`（依赖 ACL 子命令覆盖） | `tests/unit/acl.tcl:368` `tests/unit/acl.tcl:670` |
| ACL LOG 与 metrics | 聚合、context、entry-id、`acllog-max-len`、`acl_access_denied_*` | `tests/unit/acl.tcl:678` `tests/unit/acl.tcl:980` |
| HELLO/AUTH 链式语义 | `HELLO ... AUTH ... SETNAME ...` 优先级与失败回滚 | `tests/unit/acl.tcl:851` `tests/unit/acl.tcl:884` |
| ACL 文件与配置 | `ACL LOAD/SAVE`、重复用户、注释、`acl-pubsub-default`、config rewrite | `tests/unit/acl.tcl:1003` `tests/unit/acl.tcl:1324` |
| Selectors 基础 | 多 selector、删除 selector、selector 语法 | `tests/unit/acl-v2.tcl:3` `tests/unit/acl-v2.tcl:62` |
| `%R/%W/%RW` 读写分离 | key 模式读写权限与命令行为（`SET/BITFIELD/SORT` 等） | `tests/unit/acl-v2.tcl:85` `tests/unit/acl-v2.tcl:513` |
| DRYRUN 深测 | 用户/命令不存在、参数个数、复杂 keyspec 命令 | `tests/unit/acl-v2.tcl:333` `tests/unit/acl-v2.tcl:523` |
| Selector + ACL 文件 | selectors 的 `GETUSER/LIST` 表达与 `ACL LOAD` 行为 | `tests/unit/acl-v2.tcl:297` `tests/unit/acl-v2.tcl:534` |

## 3. 需要额外补充的用例（阅读源码后新增）

下面这些是当前 `acl.tcl/acl-v2.tcl` 覆盖较弱或未覆盖，但源码里有明确语义，建议补到 kvrocks 兼容套件中。

### EXT-01 `AUTH <password>` 在 default user 为 `nopass` 时的错误路径

- 依据：`src/acl.c:3270`
- 步骤：
  1. `ACL SETUSER default on nopass +@all ~* &*`
  2. 发送 `AUTH anypass`（两参数旧式）
- 期望：
  - 返回 `ERR`，关键字包含 `AUTH <password> called without any password configured`

### EXT-02 `skip-sanitize-payload`/`sanitize-payload` 切换与 `reset` 回归

- 依据：`src/acl.c:1297` `src/acl.c:1299` `src/acl.c:1370`
- 步骤：
  1. `ACL SETUSER u on >p +@all ~* &* skip-sanitize-payload`
  2. `ACL GETUSER u`，检查 `flags`
  3. `ACL SETUSER u sanitize-payload`
  4. `ACL SETUSER u reset`
- 期望：
  - 第 2 步含 `skip-sanitize-payload`
  - 切回后不再含 `skip-sanitize-payload`
  - `reset` 后恢复默认 `sanitize-payload`

### EXT-03 `ACL SETUSER` 对已存在用户的“全量原子更新”

- 依据：`src/acl.c:2109` `src/acl.c:2123`（临时用户 staging 后再覆盖）
- 步骤：
  1. `ACL SETUSER atom on >p +get ~*`
  2. 执行 `ACL SETUSER atom +set +not-a-command`（混合合法+非法）
  3. `ACL DRYRUN atom GET k`
  4. `ACL DRYRUN atom SET k v`
- 期望：
  - 第 2 步返回 `ERR`（未知命令）
  - 第 3 步 `OK`
  - 第 4 步仍然拒绝（证明无部分生效）

### EXT-04 `ACL LOAD` 行首关键字校验（非 `user`）

- 依据：`src/acl.c:2361`
- 步骤：
  1. 在 ACL 文件写入一行：`foo alice on nopass +@all ~*`
  2. 执行 `ACL LOAD`
- 期望：
  - 返回 `ERR`，包含 `should start with user keyword`
  - ACL 规则整体不变（回滚）

### EXT-05 `ACL LOAD` 引号不平衡

- 依据：`src/acl.c:2347`
- 步骤：
  1. ACL 文件加入不平衡引号行（例如 `user alice on >"abc`）
  2. `ACL LOAD`
- 期望：
  - 返回 `ERR`，包含 `unbalanced quotes in acl line`
  - 规则不变

### EXT-06 `ACL LOAD` 用户名非法字符（制表符）

- 依据：`src/acl.c:2371`
- 步骤：
  1. ACL 文件写入：`user bad<TAB>name on nopass +@all ~*`
  2. `ACL LOAD`
- 期望：
  - 返回 `ERR`，包含 `contains invalid characters`
  - 规则不变

### EXT-07 `ACL LOAD` 非重复用户场景下的原子回滚

- 依据：`src/acl.c:2438`（成功才替换 Users；失败保留 old users）
- 步骤：
  1. 先确认 `alice` 当前权限（如仅 `+get`）
  2. ACL 文件同时包含：`alice` 合法更新 + `bob` 非法规则（如 `%~`）
  3. `ACL LOAD`
  4. 再查 `ACL GETUSER alice` 与 `ACL GETUSER bob`
- 期望：
  - `ACL LOAD` 报错
  - `alice` 保持旧值、`bob` 不存在

### EXT-08 `allkeys/allchannels` 后继续追加 pattern 的错误路径（root selector）

- 依据：`src/acl.c:1046` `src/acl.c:1051` `src/acl.c:1393` `src/acl.c:1397`
- 步骤：
  1. `ACL SETUSER u1 on nopass allkeys`
  2. `ACL SETUSER u1 ~foo:*`
  3. `ACL SETUSER u2 on nopass allchannels`
  4. `ACL SETUSER u2 &foo:*`
- 期望：
  - 第 2 步报错，包含 `resetkeys`
  - 第 4 步报错，包含 `resetchannels`

### EXT-09 `PSUBSCRIBE` 权限按“字面量 pattern”匹配

- 依据：`src/acl.c:1652` `src/acl.c:1660`
- 步骤：
  1. `ACL SETUSER pat on nopass +acl +@pubsub resetchannels &news:*`
  2. 以 `pat` 认证
  3. `PSUBSCRIBE news:*`
  4. `PSUBSCRIBE news:1`
- 期望：
  - 第 3 步允许
  - 第 4 步 `NOPERM`（pattern 场景按字面量比较，不按 glob 展开）

### EXT-10 显式覆盖 `HELLO` 未认证错误路径

- 依据：`src/networking.c:4782`
- 步骤：
  1. `ACL SETUSER default off`
  2. 新连接直接 `HELLO 3`
- 期望：
  - 返回 `NOAUTH`，关键字包含 `HELLO must be called with the client already authenticated`

### EXT-11 非 cluster 下 `AUTH "internal connection"` 错误路径

- 依据：`src/acl.c:3221`
- 步骤：
  1. 在非 cluster 实例执行：`AUTH "internal connection" "x"`
- 期望：
  - 返回 `ERR`，包含 `Cannot authenticate as an internal connection on non-cluster instances`

### EXT-12（可选）TLS 证书自动鉴权失败日志与 metrics

- 依据：`src/networking.c:1586` `src/acl.c:2662` `src/acl.c:3108`
- 步骤：
  1. 启用 TLS + 客户端证书用户名映射
  2. 用不存在/禁用 ACL 用户名的证书连接
  3. 查询 `ACL LOG` 与 `INFO` ACL metrics
- 期望：
  - `ACL LOG` 最新项 `reason=tls-cert`
  - `acl_access_denied_tls_cert` 递增

### EXT-13 `ACL LOAD` selector 括号不匹配（文件加载路径）

- 依据：`src/acl.c:2394`
- 步骤：
  1. ACL 文件写入 `user s1 on (+get ~*`
  2. `ACL LOAD`
- 期望：
  - 返回 `ERR`，包含 `Unmatched parenthesis in selector definition`
  - 原 ACL 不变

## 4. 建议优先级（给 kvrocks 实施）

- P0（必须）：现有 140 用例中与 `AUTH/SETUSER/GETUSER/DELUSER`、命令与 key/channel 权限、`ACL LOG`、`HELLO` 直接协议行为相关的全部 + EXT-01/02/03/07/09/10。
- P1（建议）：ACL 文件边界与错误处理（EXT-04/05/06/08/13）。
- P2（可选，依赖环境）：cluster internal auth、TLS cert ACL metrics（EXT-11/12）。
- Deferred：`+cmd|sub` / `-cmd|sub` 子命令粒度授权及其 Redis 基线条目（如 `acl.tcl` 中 subcommand 相关用例）暂缓。

## 5. 运行参考（Redis 侧）

```bash
./runtest --single unit/acl
./runtest --single unit/acl-v2
```

## 6. 附录：完整测试名索引（Redis 基线 140）

### 6.1 `tests/unit/acl.tcl`（106）

```text
2:    test {Connections start with the default user} {
6:    test {It is possible to create new users} {
10:    test {Coverage: ACL USERS} {
14:    test {Usernames can not contain spaces or null characters} {
19:    test {New users start disabled} {
25:    test {Enabling the user allows the login} {
31:    test {Only the set of correct passwords work} {
41:    test {It is possible to remove passwords from the set of valid ones} {
47:    test {Test password hashes can be added} {
53:    test {Test password hashes validate input} {
61:    test {ACL GETUSER returns the password hash instead of the actual password} {
67:    test {Test hashed passwords removal} {
73:    test {By default users are not able to access any command} {
78:    test {By default users are not able to access any key} {
84:    test {It's possible to allow the access of a subset of keys} {
93:    test {By default, only default user is able to publish to any channel} {
102:    test {By default, only default user is not able to publish to any shard channel} {
110:    test {By default, only default user is able to subscribe to any channel} {
126:    test {By default, only default user is able to subscribe to any shard channel} {
142:    test {By default, only default user is able to subscribe to any pattern} {
158:    test {It's possible to allow publishing to a subset of channels} {
166:    test {It's possible to allow publishing to a subset of shard channels} {
174:    test {Validate subset of channels is prefixed with resetchannels flag} {
198:    test {In transaction queue publish/subscribe/psubscribe to unauthorized channel will fail} {
211:    test {It's possible to allow subscribing to a subset of channels} {
224:    test {It's possible to allow subscribing to a subset of shard channels} {
237:    test {It's possible to allow subscribing to a subset of channel patterns} {
250:    test {Subscribers are killed when revoked of channel permission} {
270:    test {Subscribers are killed when revoked of channel permission} {
284:    test {Subscribers are killed when revoked of channel permission} {
298:    test {Subscribers are killed when revoked of pattern permission} {
312:    test {Subscribers are killed when revoked of allchannels permission} {
326:    test {Subscribers are pardoned if literal permissions are retained and/or gaining allchannels} {
346:    test {blocked command gets rejected when reprocessed after permission change} {
363:    test {Users can be configured to authenticate with any password} {
368:    test {ACLs can exclude single commands} {
375:    test {ACLs can include or exclude whole classes of commands} {
386:    test {ACLs can include single subcommands} {
398:    test {ACLs can exclude single subcommands, case 1} {
408:    test {ACLs can exclude single subcommands, case 2} {
418:    test {ACLs cannot include a subcommand with a specific arg} {
424:    test {ACLs cannot exclude or include a container commands with a specific arg} {
432:    test {ACLs cannot exclude or include a container command with two args} {
440:    test {ACLs including of a type includes also subcommands} {
447:    test {ACLs can block SELECT of all but a specific DB} {
456:    test {ACLs can block all DEBUG subcommands except one} {
467:    test {ACLs set can include subcommands, if already full command exists} {
490:    test {ACLs set can exclude subcommands, if already full command exists} {
528:    test {ACL SETUSER RESET reverting to default newly created user} {
551:    test {ACL GETUSER is able to translate back command permissions} {
573:    test {ACL GETUSER provides reasonable results} {
592:    test {ACL GETUSER provides correct results} {
671:    test {ACL #5998 regression: memory leaks adding / removing subcommands} {
678:    test {ACL LOG aggregates similar errors together and assigns unique entry-id to new errors} {
701:    test {ACL LOG shows failed command executions at toplevel} {
731:    test {ACL LOG is able to test similar events} {
742:    test {ACL LOG is able to log keys access violations and key name} {
751:    test {ACL LOG is able to log channel access violations and channel name} {
760:    test {ACL LOG RESET is able to flush the entries in the log} {
765:    test {ACL LOG can distinguish the transaction context (1)} {
776:    test {ACL LOG can distinguish the transaction context (2)} {
795:    test {ACL can log errors in the context of Lua scripting} {
805:    test {ACL LOG can accept a numerical argument to show less entries} {
816:    test {ACL LOG can log failed auth attempts} {
825:    test {ACLLOG - zero max length is correctly handled} {
835:    test {ACL LOG entries are limited to a maximum amount} {
846:    test {ACL LOG entries are still present on update of max len config} {
851:    test {When default user is off, new connections are not authenticated} {
858:    test {When default user has no command permission, hello command still works for other users} {
866:    test {When an authentication chain is used in the HELLO cmd, the last auth cmd has precedence} {
876:    test {When a setname chain is used in the HELLO cmd, the last setname cmd has precedence} {
884:    test {When authentication fails in the HELLO cmd, the client setname should not be applied} {
891:    test {ACL HELP should not have unexpected options} {
896:    test {Delete a user that the client doesn't use} {
903:    test {Delete a user that the client is using} {
913:    test {ACL GENPASS command failed test} {
920:    test {Default user can not be removed} {
925:    test {ACL load non-existing configured ACL file} {
931:    test {ACL-Metrics user AUTH failure} {
948:    test {ACL-Metrics invalid command accesses} {
964:    test {ACL-Metrics invalid key accesses} {
980:    test {ACL-Metrics invalid channels accesses} {
1003:    test {default: load from include file, can access any channels} {
1011:    test {default: with config acl-pubsub-default allchannels after reset, can access any channels} {
1020:    test {default: with config acl-pubsub-default resetchannels after reset, can not access any channels} {
1029:    test {Alice: can execute all command} {
1035:    test {Bob: just execute @set and acl command} {
1043:    test {ACL LOAD only disconnects affected clients} {
1074:    test {ACL LOAD disconnects affected subscriber} {
1097:    test {ACL LOAD disconnects clients of deleted users} {
1128:    test {ACL load and save} {
1144:    test {ACL load and save with restricted channels} {
1168:        test {First server should have role slave after SLAVEOF} {
1177:        test {ACL load on replica when connected to replica} {
1188:    test {Default user has access to all channels irrespective of flag} {
1195:    test {Update acl-pubsub-default, existing users shouldn't get affected} {
1207:    test {Single channel is valid} {
1214:    test {Single channel is not valid with allchannels} {
1227:    test {Only default user has access to all channels irrespective of flag} {
1237:    test {default: load from config file, without channel permission default user can't access any channels} {
1244:    test {default: load from config file with all channels permissions} {
1258:    test {Test loading an ACL file with duplicate users} {
1276:    test {Test loading an ACL file with duplicate default user} {
1294:    test {Test loading duplicate users in config on startup} {
1302:    test {Test loading an ACL file with comments} {
1324:    test {ACL from config file and config rewrite} {
```

### 6.2 `tests/unit/acl-v2.tcl`（34）

```text
3:    test {Test basic multiple selectors} {
23:    test {Test ACL selectors by default have no permissions} {
32:    test {Test deleting selectors} {
43:    test {Test selector syntax error reports the error in the selector context} {
62:    test {Test flexible selector definition} {
85:    test {Test separate read permission} {
97:    test {Test separate write permission} {
109:    test {Test separate read and write permissions} {
119:    test {Validate read and write permissions format - empty permission} {
124:    test {Validate read and write permissions format - empty selector} {
129:    test {Validate read and write permissions format - empty pattern} {
137:    test {Validate read and write permissions format - no pattern} {
145:    test {Test separate read and write permissions on different selectors are not additive} {
172:    test {Test SET with separate read permission} {
186:    test {Test SET with separate write permission} {
204:    test {Test SET with read and write permissions} {
224:    test {Test BITFIELD with separate read permission} {
237:    test {Test BITFIELD with separate write permission} {
249:    test {Test BITFIELD with read and write permissions} {
261:    test {Test ACL log correctly identifies the relevant item when selectors are used} {
297:    test {Test ACL GETUSER response information} {
314:    test {Test ACL list idempotency} {
327:    test {Test R+W is the same as all permissions} {
333:    test {Test basic dry run functionality} {
344:    test {Test various commands for command permissions} {
350:    test {Test various odd commands for key permissions} {
403:    test {Existence test commands are not marked as access} {
420:    test {Intersection cardinaltiy commands are access commands} {
438:    test {Test general keyspace commands require some type of permission to execute} {
460:    test {Cardinality commands require some type of permission to execute} {
470:    test {Test sharded channel permissions} {
482:    test {Test sort with ACL permissions} {
514:    test {Test DRYRUN with wrong number of arguments} {
534:    test {Test behavior of loading ACLs} {
```
