# Kvrocks ACL Current Design (Implementation-Oriented)

This document describes the **current ACL implementation in Kvrocks** based on the code in this repository.
It focuses on runtime behavior and operational interactions, especially with:

- cluster mode
- namespace / token-based multi-tenancy
- replication

The goal is to document what is implemented today, not an ideal future design.

## 1. Scope and Terminology

### 1.1 Scope

This document covers:

- ACL enablement and startup behavior
- in-memory model and persistence format
- authentication and authorization paths
- interactions with replication, cluster, and namespace
- compatibility differences from Redis

### 1.2 Terms used in Kvrocks

- `admin connection`: a connection authenticated by `requirepass` (or by implicit startup behavior when auth is not required)
- `ACL user connection`: a connection authenticated against ACL users

In Kvrocks, these two roles are separated in connection state (`BecomeAdmin()` vs `BecomeUser()`).

## 2. Enablement and Lifecycle

### 2.1 Feature flag

ACL is guarded by `acl-preview-enabled`.

- If disabled, `ACL ...` commands return an error saying the preview feature is disabled.
- If enabled, server startup prints a warning that this is incomplete and not production-ready.

### 2.2 Startup load

During server startup (`Server::Start`):

1. namespace metadata is loaded (`namespace_.LoadAndRewrite()`)
2. ACL metadata is loaded (`acl_.LoadAcl()`)

ACL users are loaded from RocksDB Propagate CF keys with prefix `acl|`.

## 3. Data Model

### 3.1 `AclUser`

`AclUser` contains:

- `enabled`
- `nopass`
- `sanitize_payload`
- `ns` (namespace bound to this ACL user)
- `allowed_commands` (a vector of selectors, selector[0] is root)
- `passwords` (SHA256 hex digests)

### 3.2 `AclSelector`

Each selector has:

- command bitmap (`allowed_commands`)
- category bitmap (`allowed_category`, currently mainly serialization compatibility)
- key patterns (`~`, `%R~`, `%W~`, `%RW~`)
- channel patterns (`&...`)
- flags (`allcommands`, `allkeys`, `allchannels`, root flag)

Authorization is selector-OR: if any selector passes command+key+channel checks, the command is allowed.

### 3.3 User manager limits

`AclUserManager` uses an atomic snapshot slot table (`shared_ptr<const vector<shared_ptr<const AclUser>>>`).

Practical consequence: ACL user slots grow dynamically and are no longer capped at 256.

### 3.4 Command bitmap manager

`AclCommandManager` builds command bitmaps from the registered command table.

- commands are registered when command attributes are inserted into `CommandTable`
- manager is sealed in `Acl::LoadAcl()`
- categories are mapped through `CommandCategory`

## 4. Persistence and Replication Format

### 4.1 Storage layout

ACL users are persisted into Propagate CF as:

- key: `acl|<username>`
- value: JSON (`AclUser::ToJson()`)

Delete operation removes the same key from Propagate CF.

### 4.2 Replica apply path

On replicas, write batches are parsed in replication code.

When a propagated key starts with `acl|`:

- `PutCF`: apply `Acl::ApplyReplicatedUpdate(username, json)`
- `DeleteCF`: apply `Acl::ApplyReplicatedDeletion(username)`

This means ACL user updates are synchronized through replication write batches.

## 5. Authentication Flow

### 5.1 `AUTH <username> <password>`

With ACL preview enabled:

- server looks up this ACL user by exact username
- user must exist and be enabled
- if `nopass`, any provided password is accepted for explicit username auth
- else provided password hash must match
- on success: connection becomes user mode and ACL profile is attached
- connection namespace is set to ACL user `ns`

With ACL preview disabled: this path is not used for ACL users.

### 5.2 `AUTH <password>`

Resolution order in current code:

1. if ACL preview enabled and ACL user `default` exists:
   - authenticate against `default`
   - if `default` is `nopass`, returns `NO_REQUIRE_PASS` (legacy `AUTH <password>` is rejected)
2. otherwise fallback to legacy Kvrocks auth model:
   - namespace token (`NAMESPACE` token) -> user namespace session
   - `requirepass` -> admin session

### 5.3 New connection default behavior

When first command arrives and connection has no namespace yet:

- if ACL preview is enabled and `default` ACL user is enabled + `nopass`, connection auto-binds to that ACL user
- otherwise auth may be required depending on `requirepass`
- if auth is not required, connection defaults to admin mode in `__namespace`

### 5.4 `ACL SETUSER ... off` behavior

Kvrocks follows Redis behavior for disabling users:

- `off` blocks future authentication attempts for that user.
- existing authenticated connections for that user are not forcibly deauthenticated by `off` alone.
- ACL permission updates (commands/keys/channels) still take effect on the next command via ACL version refresh.

## 6. Authorization Flow

### 6.1 Where ACL is enforced

ACL checks are done in `Connection::ExecuteCommands` only when all are true:

- `acl-preview-enabled` is on
- connection is not admin
- connection has ACL profile

So admin sessions bypass ACL checks.

### 6.2 Check order in command execution path

Current high-level order:

1. parse command
2. admin flag check (`kCmdAdmin`)
3. ACL check (`CheckAclCommandAllowed`)
4. cluster self-execution/routing check (`cluster->CanExecByMySelf`)

### 6.3 ACL decision logic

For the current ACL user profile:

- user must still exist in cache by index
- selectors are evaluated with OR semantics
- each selector requires all of:
  - command allowed
  - all extracted command keys allowed by key pattern and R/W permission
  - channel constraints satisfied for Pub/Sub commands

If none passes, command is denied with `NOPERM`.

Note:

- `enabled` is checked during authentication, not as a runtime gate for already-authenticated sessions.

## 7. Namespace Interaction

Namespace interaction is one of the main Kvrocks-specific ACL behaviors.

### 7.1 Namespace binding at `ACL SETUSER`

Kvrocks derives ACL user namespace from username suffix:

- username format: `<name>#<namespace>`
- if suffix is absent -> `__namespace`
- if suffix namespace does not exist in namespace manager -> fallback `__namespace`

This resolution happens when creating a new ACL user.

### 7.2 Stored namespace vs runtime namespace manager

`AclUser.ns` is persisted in ACL JSON and used at authentication time directly.

Runtime auth does not re-validate this namespace against namespace token table on each login.

### 7.3 Namespace command and ACL

`NAMESPACE` command remains an administrative command (except `NAMESPACE CURRENT` flag adjustment).

So typical ACL user sessions cannot manage namespace mappings.

### 7.4 Constraints with other features

Kvrocks config constraints enforce:

- cluster mode and non-default namespace tokens cannot coexist
- `redis-databases > 0` and namespace tokens cannot coexist
- cluster mode and `redis-databases > 0` cannot coexist

Therefore, in cluster mode, ACL users effectively operate in default namespace semantics.

### 7.5 Interaction with `repl-namespace-enabled`

Namespace mapping replication (`__namespace_keys__`) is controlled by `repl-namespace-enabled`.

ACL user replication (`acl|...`) is independent and always follows replication write batches.

When replicated ACL deletion is applied, Kvrocks also disconnects local connections authenticated as that deleted user.

Operationally, this can create divergence risk if namespace mapping distribution policy and ACL user distribution policy are not managed together.

## 8. Cluster Interaction

### 8.1 Cluster routing vs ACL check order

Kvrocks checks ACL before cluster routing/self-check in the command pipeline.

Result: ACL-denied requests return `NOPERM` before cluster redirection is evaluated.

### 8.2 ACL categories in cluster environment

Cluster commands are categorized under `@cluster` via command registration categories.

However, `admin` command flag is still a separate gate and is checked before ACL permission matching.

### 8.3 Namespace in cluster mode

Cluster mode disables namespace token usage at config level.

So ACL namespace suffix (`user#ns`) generally degrades to default namespace behavior unless namespace definitions are valid in current runtime state.

## 9. ACL Command Surface in Kvrocks

Implemented subcommands include:

- `SETUSER`, `GETUSER`, `USERS`, `WHOAMI`, `HELP`, `CAT`, `LIST`, `DELUSER`, `GENPASS`, `LOG`, `DRYRUN`, `LOAD`, `SAVE`

Current completeness status:

- `LOG`: placeholder behavior (empty list / reset OK), no Redis-style event log pipeline yet
- `LOAD` / `SAVE`: placeholder error path (ACL file not configured), no full ACL file workflow yet
- `DELUSER`: removes the ACL user and disconnects existing connections authenticated as that user

## 10. Differences From Redis (Current)

This section lists practical differences that matter for behavior compatibility.

Review status for tracked differences in this document:

- item #1: accepted behavior
- item #2: accepted behavior
- item #3: accepted behavior
- item #9: accepted behavior
- item #10: fixed (ACL check now runs before cluster routing/self-check)

### 10.1 Feature maturity and switches

- Kvrocks ACL is feature-gated by `acl-preview-enabled`.
- Redis ACL is a core feature, not a preview gate.
- Status: accepted by design (review item #1).

### 10.2 Privilege model

- Kvrocks has an explicit `admin` connection role that bypasses ACL checks.
- Redis ACL model is centered on user permissions without a separate Kvrocks-style admin bypass role.
- Status: accepted by design (review item #2).

### 10.3 ACL command accessibility

- In Kvrocks, `ACL` command itself is registered with admin flag, so regular ACL users cannot run ACL management commands.
- Redis allows ACL command access according to ACL permissions.
- Status: accepted by design (review item #3).

### 10.4 ACL LOG and ACL file lifecycle

- Kvrocks `ACL LOG` is currently placeholder-only.
- Kvrocks `ACL LOAD` / `ACL SAVE` are placeholder paths without full ACL-file lifecycle.
- Redis provides full ACL LOG and ACL file load/save behavior.

### 10.5 Subcommand-level ACL

- Kvrocks currently stores command permissions at command bitmap granularity.
- Tokens like `+cmd|sub` are accepted syntactically but effectively collapse to command-level allow/deny in current implementation.
- Redis supports real subcommand-level ACL control.

### 10.6 DRYRUN response shape

- Kvrocks `ACL DRYRUN` returns `OK` or an error (`NOPERM`).
- Redis DRYRUN denial response semantics differ (documented as non-OK message result in Redis ACL docs).

### 10.7 User scaling limit

- Kvrocks ACL user slots are dynamically expandable and are not capped at 256.
- Redis also does not document a fixed ACL user slot cap.

### 10.8 Namespace coupling (Kvrocks-specific)

- Kvrocks ACL users carry namespace binding in user metadata (`ns`) and support `username#namespace` derivation.
- Redis ACL has no Kvrocks namespace/token coupling model.
- Status: accepted by design (review item #9).

### 10.9 Cluster ordering behavior

- Kvrocks now checks ACL before cluster routing/self-check.
- Redis behavior also checks ACL earlier in the command pipeline.
- Status: fixed (review item #10).

### 10.10 Advanced key extraction edge cases

- Kvrocks has TODOs for ACL-sensitive dynamic key references in some command options (for example some SORT pattern cases).
- Redis ACL implementation uses a more complete key/channel extraction path for those command forms.

## 11. Operational Recommendations

For production-like environments (even in preview), treat ACL + namespace + cluster as one policy package:

1. keep ACL user rollout and namespace mapping rollout synchronized
2. avoid relying on subcommand-level ACL granularity in Kvrocks today
3. do not assume Redis ACL LOG or ACL file workflows are available
4. validate cluster behavior with ACL enabled, especially ASKING/importing flows with key-pattern ACL rules

## 12. Source Pointers

Core files for this design:

- `src/server/acl/acl.cc`
- `src/server/acl/acl_actions.cc`
- `src/server/acl/acl_user.cc`
- `src/server/acl/acl_command_manager.cc`
- `src/server/redis_connection.cc`
- `src/server/server.cc`
- `src/commands/cmd_server.cc`
- `src/cluster/replication.cc`
- `src/server/namespace.cc`
- `src/config/config.cc`

Related remediation plan:

- `docs/acl/kvrocks-acl-redis-aligned-remediation-plan.md`
