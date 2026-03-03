# Redis ACL (RESP3) Protocol Spec (Based On `src/acl.c`)

本文档描述 `ACL` 容器命令的 RESP3 协议层行为：请求/响应的类型、字段结构、典型输出以及错误文案。

范围与约束：
- 仅覆盖 `ACL` 子命令（`ACL HELP/LOAD/SAVE/LIST/USERS/CAT/SETUSER/DELUSER/GETUSER/GENPASS/WHOAMI/LOG/DRYRUN`）。
- 仅按 RESP3 描述（不包含 RESP2 的 map/number 兼容输出）。
- 不包含已标记为 deprecated 的 `+<command>|<first-arg>` 用法。
- 以本仓库源码实现为准（主要参考 `src/acl.c` 与基础 reply API 的 `src/networking.c`）。

## 1. RESP3 基本编码约定

Redis 命令请求在 RESP3 下仍然是「Array of Blob String」：

```text
*<n>\r\n
$<len1>\r\n<arg1>\r\n
$<len2>\r\n<arg2>\r\n
...
```

本规范里会用到的 RESP3 类型：
- Simple String: `+OK\r\n`
- Blob String: `$3\r\nfoo\r\n`
- Integer: `:123\r\n`
- Null: `_\r\n`
- Array: `*2\r\n...`
- Map: `%3\r\n...`（注意：Redis 在本命令中使用 **Blob String** 作为 map key）
- Double: `,12.34\r\n`
- Error: `-ERR <msg>\r\n`、`-NOPERM <msg>\r\n`（如果传入的 error 文本不以 `-` 开头，Redis 会自动加 `-ERR ` 前缀）

## 2. `ACL` 子命令分发与通用错误

`ACL` 的子命令在服务器端按 `c->argv[1]` 分发；当子命令未知或参数数量不匹配时，返回：

```text
-ERR unknown subcommand or wrong number of arguments for '<sub>'. Try ACL HELP.\r\n
```

## 3. 子命令规范

### 3.1 `ACL HELP`

**请求**
- `ACL HELP`

**响应**
- Array（元素是 Simple String）。
- 第一行是头部：`ACL <subcommand> [<arg> [value] [opt] ...]. Subcommands are:`
- 之后为各子命令的用法行（每行一个元素）
- 结尾固定包含 `HELP` 与 `    Print this help.`

**RESP3 示例**
```text
*<m>\r\n
+ACL <subcommand> [<arg> [value] [opt] ...]. Subcommands are:\r\n
+CAT [<category>]\r\n
+    List all commands that belong to <category>, or all command categories\r\n
...\r\n
+HELP\r\n
+    Print this help.\r\n
```

### 3.2 `ACL CAT [<category>]`

**请求**
- `ACL CAT`
- `ACL CAT <category>`（注意：不带 `@`，例如 `read`、`admin`）

**响应**
- `ACL CAT`：Array（Blob String），每个元素是一个 category 名（例如 `read`）。
- `ACL CAT <category>`：Array（Blob String），每个元素是命令全名 `cmd->fullname`。
  - 子命令使用 `parent|sub` 形式，例如：`config|get`、`acl|setuser`。

**错误**
- category 不存在：
  ```text
  -ERR Unknown category '<category>'\r\n
  ```

### 3.3 `ACL USERS`

**请求**
- `ACL USERS`

**响应**
- Array（Blob String），元素为用户名。
- 顺序为用户名的字典序（rax 迭代顺序）。

### 3.4 `ACL LIST`

**请求**
- `ACL LIST`

**响应**
- Array（Blob String），每个元素是一行“配置文件格式”的用户定义：
  - `user <username> <rules...>`
  - `<rules...>` 由 `ACLDescribeUser()` 生成，包含 flags、密码 hash、selectors（root selector 与额外 selector）。

**输出格式要点（便于实现兼容）**
- `passwords` 在 LIST 中永远以 `#<hash>` 输出（hash 为 64 字符小写 hex）。
- root selector 直接输出；额外 selector 用括号包裹：` ( ... )`
- selector 内部输出顺序固定：
  1) keys（`~*` 或 `%R~/%W~` 规则集合）
  2) channels（`&*` 或以 `resetchannels` 开头的规则集合）
  3) commands（总是以 `+@all` 或 `-@all` 开头）

### 3.5 `ACL GETUSER <username>`

**请求**
- `ACL GETUSER <username>`

**响应**
- 用户不存在：Null
- 用户存在：Map，key 均为 Blob String。字段及类型如下（顺序与实现一致）：
  1) `flags` -> Array(Blob String)
     - 可能值集合：`on`, `off`, `nopass`, `skip-sanitize-payload`, `sanitize-payload`
  2) `passwords` -> Array(Blob String)
     - 元素为 64 字符小写 hex hash（不带 `#` 前缀）
  3) `commands` -> Blob String
     - root selector 的命令规则串（总以 `+@all` 或 `-@all` 开头）
  4) `keys` -> Blob String
     - `~*` 或以空格分隔的 key 规则（可能包含 `%R~`/`%W~`）
  5) `channels` -> Blob String
     - `&*` 或以空格分隔的 `&pattern` 列表（注意：这里 **不输出** `resetchannels`）
  6) `selectors` -> Array(Map)
     - 不包含 root selector；每个 selector map 固定 3 个字段：
       - `commands` -> Blob String
       - `keys` -> Blob String
       - `channels` -> Blob String

**RESP3 示例（结构示意，不含实际 hash）**
```text
%6\r\n
$5\r\nflags\r\n
*2\r\n$2\r\non\r\n$16\r\nsanitize-payload\r\n
$9\r\npasswords\r\n
*1\r\n$64\r\n<hash>\r\n
$8\r\ncommands\r\n
$...\r\n+@all -@dangerous +get\r\n
$4\r\nkeys\r\n
$2\r\n~*\r\n
$8\r\nchannels\r\n
$2\r\n&*\r\n
$9\r\nselectors\r\n
*0\r\n
```

### 3.6 `ACL WHOAMI`

**请求**
- `ACL WHOAMI`

**响应**
- 当前连接存在 ACL 用户：Blob String（用户名）
- `c->user == NULL`（例如内部连接或“无限权限”连接）：Null

### 3.7 `ACL GENPASS [<bits>]`

**请求**
- `ACL GENPASS`
- `ACL GENPASS <bits>`

**响应**
- Blob String：随机 hex 字符串
- 默认 `bits=256`，输出 64 个 hex 字符
- `chars = ceil(bits/4)`（源码实现为 `(bits+3)/4`）

**错误**
- `<bits>` 非整数或超出范围（来自通用整数解析）：
  - `-ERR value is not an integer or out of range\r\n`
  - `-ERR value is out of range\r\n`
- `<bits> <= 0` 或 `> 4096`：
  ```text
  -ERR ACL GENPASS argument must be the number of bits for the output password, a positive number up to 4096\r\n
  ```

### 3.8 `ACL LOG [<count> | RESET]`

**请求**
- `ACL LOG`
- `ACL LOG <count>`
- `ACL LOG RESET`（大小写不敏感）

**响应**
- `ACL LOG RESET`：`+OK`
- 否则：Array(Map)
  - 默认 `count=10`；若 `count` 大于当前日志条目数，返回全部
  - 每条日志是 Map，固定 10 个字段（key 为 Blob String）：
    - `count` -> Integer（聚合次数）
    - `reason` -> Blob String（`command`/`key`/`channel`/`auth`/`tls-cert`/`unknown`）
    - `context` -> Blob String（`toplevel`/`multi`/`lua`/`module`/`unknown`）
    - `object` -> Blob String
    - `username` -> Blob String
    - `age-seconds` -> Double
    - `client-info` -> Blob String
    - `entry-id` -> Integer
    - `timestamp-created` -> Integer（毫秒）
    - `timestamp-last-updated` -> Integer（毫秒）

**错误**
- `<count>` 非整数：`-ERR value is not an integer or out of range\r\n`

### 3.9 `ACL DRYRUN <username> <command> [<arg> ...]`

**请求**
- `ACL DRYRUN <username> <command> [args...]`

**响应**
- 允许执行：`+OK`
- 不允许执行：**Blob String**（不是 Error Reply）
  - 命令被拒：`User <user> has no permissions to run the '<cmd>' command`
  - key 被拒：`User <user> has no permissions to access the '<key>' key`
  - channel 被拒：`User <user> has no permissions to access the '<channel>' channel`

**错误**
- 用户不存在：
  ```text
  -ERR User '<user>' not found\r\n
  ```
- 命令不存在：
  ```text
  -ERR Command '<cmd>' not found\r\n
  ```
- 参数个数与目标命令 arity 不匹配：
  ```text
  -ERR wrong number of arguments for '<cmd>' command\r\n
  ```

### 3.10 `ACL LOAD` / `ACL SAVE`

这两条依赖配置项 `aclfile`（内部为 `server.acl_filename`）。

**请求**
- `ACL LOAD`
- `ACL SAVE`

**响应**
- 成功：`+OK`
- 失败：Error Reply（通常为 `-ERR ...`）

**错误：未配置 aclfile**
```text
-ERR This Redis instance is not configured to use an ACL file. You may want to specify users via the ACL SETUSER command and then issue a CONFIG REWRITE (assuming you have a Redis configuration file set) in order to store users in the Redis configuration.\r\n
```

**`ACL LOAD` 错误：打开文件失败**
```text
-ERR Error loading ACLs, opening file '<file>': <strerror>\r\n
```

**`ACL LOAD` 错误：文件内容校验失败（会拼接多段信息到同一行）**
- 典型片段包括：
  - `<file>:<line>: unbalanced quotes in acl line.`
  - `<file>:<line> should start with user keyword followed by the username.`
  - `'<file>:<line>: username '<name>' contains invalid characters.`
  - `WARNING: Duplicate user '<name>' found on line <line>.`
  - `<file>:<line>: Unmatched parenthesis in selector definition.`
  - `<file>:<line>: Error in applying operation '<op>': <errmsg>.`
- 最后统一追加：
  - `WARNING: ACL errors detected, no change to the previously active ACL rules was performed`

**`ACL SAVE` 错误：保存失败**
```text
-ERR There was an error trying to save the ACLs. Please check the server logs for more information\r\n
```

### 3.11 `ACL DELUSER <username> [<username> ...]`

**请求**
- `ACL DELUSER <u1> [u2 ...]`

**响应**
- Integer：成功删除的用户数（不存在的不计数）

**错误**
- 任意参数为 `default`：
  ```text
  -ERR The 'default' user cannot be removed\r\n
  ```

### 3.12 `ACL SETUSER <username> <attribute> [<attribute> ...]`

**请求**
- `ACL SETUSER <username> <attr> [attr ...]`

**响应**
- 成功：`+OK`
- 失败：Error Reply（`-ERR ...`）

**用户名约束**
- 用户名不能包含空白字符或 `\0`；否则：
  ```text
  -ERR Usernames can't contain spaces or null characters\r\n
  ```

**原子性**
- `SETUSER` 会先在临时 user 上应用所有 modifier，任意一步失败则整体不生效（不会“部分成功”）。

**attr 语法（不含 deprecated first-arg）**

用户级 modifier（直接作用于 user）：
- `on` / `off`
- `sanitize-payload` / `skip-sanitize-payload`
- `nopass` / `resetpass`
- `>cleartext`：添加密码（服务端存储为 hash）
- `#<hash>`：添加 hash（必须为 64 字符小写 hex）
- `<cleartext`：删除密码（按 cleartext 计算 hash 后删除）
- `!<hash>`：删除 hash
- `reset`：重置为初始状态（包含 `resetpass/resetkeys/resetchannels/off/...` 等组合）
- `clearselectors`：移除所有“非 root selector”
- `(<selector-op> [<selector-op> ...])`：创建一个新 selector 并附加到用户

selector 级 modifier（作用于 root selector 或括号 selector）：
- commands：
  - `+@all` / `-@all`（别名：`allcommands` / `nocommands`）
  - `+@<category>` / `-@<category>`（例如 `+@read`）
  - `+<command>` / `-<command>`
  - `+<command>|<subcommand>` / `-<command>|<subcommand>`（子命令权限）
- keys：
  - `~*` / `allkeys`
  - `resetkeys`
  - `~<pattern>`
  - `%R~<pattern>` / `%W~<pattern>`
- channels：
  - `&*` / `allchannels`
  - `resetchannels`
  - `&<pattern>`

**错误形态**
- 单个 modifier 失败会返回（整体不生效）：
  ```text
  -ERR Error in ACL SETUSER modifier '<attr>': <errmsg>\r\n
  ```
- selector 括号未闭合（`(...)` 被拆在多个 argv 时合并失败）：
  ```text
  -ERR Unmatched parenthesis in acl selector starting at '<arg>'.\r\n
  ```

`<errmsg>` 常见取值（来自实现映射）：
- `Unknown command or category name in ACL`
- `Syntax error`
- `Adding a pattern after the * pattern (or the 'allkeys' flag) is not valid and does not have any effect. Try 'resetkeys' to start with an empty list of patterns`
- `Adding a pattern after the * pattern (or the 'allchannels' flag) is not valid and does not have any effect. Try 'resetchannels' to start with an empty list of channels`
- `The password you are trying to remove from the user does not exist`
- `The password hash must be exactly 64 characters and contain only lowercase hexadecimal characters`

## 4. 明确不包含的 deprecated 行为

Redis 源码仍允许在“无子命令的命令”上写 `+<command>|<first-arg>`（并会打印 deprecation warning），但该用法已被标记为未来可能移除，因此本文档不将其作为兼容目标。

