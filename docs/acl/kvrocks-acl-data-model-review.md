# Kvrocks ACL 数据模型审阅与优化建议

Date: 2026-03-04
Scope: Current ACL data model and its runtime interactions with cluster, replication, namespace, and observability.

## 1. Executive Summary

当前 ACL 数据模型主结构是合理的：

1. `AclUser` + selector OR 语义的表达能力足够覆盖常见 ACL 场景。
2. `AclUserManager` 使用 snapshot（`shared_ptr<const UserSlots>`）读路径无锁化，适合高并发读取。
3. ACL 规则落盘到 Propagate CF（`acl|<username>`）能够与复制通路集成。

但目前仍有高优先级正确性与安全风险，建议优先处理 P0 项，再做性能和整洁性优化。

## 2. Current Model Snapshot

### 2.1 Core Entities

1. `AclUser`
   - Fields: `enabled`, `nopass`, `sanitize_payload`, `ns`, `allowed_commands`（selectors）, `passwords`.
2. `AclSelector`
   - Fields: `flags`, `allowed_commands` bitmap, `allowed_category`, `key_patterns`, `channels`.
3. `AclUserManager`
   - username -> slot index map + slot table snapshot + free slot reuse.

### 2.2 Persistence and Distribution

1. Storage key/value:
   - key: `acl|<username>`
   - value: JSON (`AclUser::ToJson()`).
2. Replication apply:
   - Propagate PUT -> `ApplyReplicatedUpdate`
   - Propagate DELETE -> `ApplyReplicatedDeletion`
   - Replicated delete path additionally triggers `KillClientByAclUser`.

### 2.3 Runtime Enforcement

1. ACL check is done per command in connection execution pipeline.
2. Selector evaluation is OR across selectors.
3. A selector must pass command + key + channel checks together.

## 3. Findings (Prioritized)

## 3.1 P0 - Correctness/Security Must-Fix

### P0-1: Merge conflict markers exist in ACL critical paths

Impact:

1. Build/behavior instability.
2. Data-model behavior can diverge from design depending on unresolved branch content.

Evidence:

1. `src/server/redis_connection.cc`
2. `src/config/config.h`
3. `src/config/config.cc`
4. `tests/gocase/unit/acl/acl_test.go`

Recommendation:

1. Resolve conflicts before any further ACL evolution.
2. Freeze branch as a known-good baseline after conflict resolution.

### P0-2: Connection identity anchored by slot index has reuse correctness risk

Impact:

1. Connection keeps `acl_user_index`; slots are reusable after delete.
2. In race windows, deleted-user connection can observe a new user in reused slot before TCP close fully takes effect.
3. Potential privilege confusion or policy mismatch.

Evidence:

1. `Connection` stores `acl_user_index` and resolves user by index at runtime.
2. `AclUserManager` recycles free slots.
3. `KillClientByAclUser` is asynchronous close-after-reply, not synchronous removal.

Recommendation:

1. Replace raw index identity with `(index, generation)` or `(index + username verification)` on each ACL check.
2. Fail closed (`NOAUTH`/disconnect) if identity mismatch is detected.

### P0-3: `ACL LOAD` and namespace strict behavior are not model-consistent

Impact:

1. `ACL SETUSER` path supports strict namespace policy via config.
2. `ACL LOAD` currently calls `HandleSetUser(nullptr, ...)`, which can break namespaced users under strict mode.
3. File-based lifecycle semantics become inconsistent with command-based lifecycle.

Recommendation:

1. Make `ACL LOAD` call `HandleSetUser(ns_mgr, ..., strict_namespace=config.acl_namespace_strict)`.
2. Keep namespace validation semantics identical across `SETUSER` and `LOAD`.

## 3.2 P1 - Important Gaps

### P1-1: `ACL LOAD` apply is not atomic

Current behavior:

1. Delete users missing in file.
2. Apply users from file one by one.
3. Midway failure leads to partially applied ACL state.

Risk:

1. Operational rollback difficulty.
2. Temporary policy gaps.

Recommendation:

1. Parse all lines -> build staged user map -> validate all -> single swap/commit.
2. Keep old ACL set untouched on any parsing or validation failure.

### P1-2: Channel permission tightening does not actively converge existing subscribers

Current behavior:

1. `DELUSER` kills user connections.
2. `SETUSER` permission tightening does not sweep or kill already subscribed connections.

Risk:

1. Subscription session may keep receiving channels that should now be denied.

Recommendation:

1. On `SETUSER` update, detect channel scope tightening.
2. Kill affected user connections (Redis-aligned conservative behavior), or force unsubscribe with strict correctness proof.

### P1-3: `sanitize_payload` exists in model but is not wired to all observability paths

Current behavior:

1. Model has `sanitize_payload`.
2. Redaction helper currently centers on `AUTH/HELLO`.
3. `SLOWLOG` path still writes raw args.

Risk:

1. Password-like tokens in ACL commands may leak into monitor/slowlog.

Recommendation:

1. Expand redaction scope to ACL command family.
2. Reuse one shared redaction policy in monitor + slowlog.
3. Bind behavior explicitly to `sanitize_payload` semantics.

## 3.3 P2 - Maintainability/Model Clarity

### P2-1: `allowed_category` is persisted but not first-class in runtime decision path

Observation:

1. Runtime checks are command bitmap centric.
2. Category bitmap is partly a compatibility/serialization artifact.

Risk:

1. Field meaning drifts over time.
2. Increases model cognitive load for maintainers.

Recommendation:

1. Define explicit policy:
   - either runtime-authoritative field, or
   - compatibility-only derived field.
2. Enforce that policy in parser/serializer and tests.

## 4. Performance Opportunities

### 4.1 User manager write amplification

Current behavior:

1. Set/Add/Delete each copies the entire slots vector snapshot.

Cost:

1. ACL write-heavy workloads may incur O(N) copy cost repeatedly.

Suggestion:

1. Keep read lock-free, but optimize write path with chunked slots or indirection table.
2. Benchmark before/after on large user counts.

### 4.2 ACL log aggregation complexity

Current behavior:

1. Log aggregation linearly scans deque for matching tuple.

Cost:

1. Acceptable under short max length, but degrades when max length grows.

Suggestion:

1. Keep deque for order, add hash index for `(reason, context, object, username)` -> iterator.

## 5. Architecture Cleanliness Recommendations

### 5.1 Introduce explicit ACL session identity object

Suggestion:

1. Encapsulate `username`, `index`, `generation`, `authenticated_ns`.
2. Use one verifier function in command path and transaction/script recheck path.

### 5.2 Unify auth state transition

Suggestion:

1. Share one helper for `AUTH` and `HELLO AUTH`.
2. Make success/failure state transition atomic and identical.

### 5.3 Separate ACL file lifecycle from command parser

Suggestion:

1. `LoadAclFromFile` should be parse/validate/apply pipeline.
2. Do not mix file parsing logic with direct mutable operations in a non-atomic loop.

## 6. Recommended Execution Plan

### Phase 0 (stabilize)

1. Resolve all merge conflicts in ACL-related files.
2. Re-run ACL baseline tests.

### Phase 1 (correctness/security)

1. Fix identity reuse risk (`index+generation` or username verification).
2. Make `ACL LOAD` namespace semantics consistent with `SETUSER`.
3. Make `ACL LOAD` atomic.
4. Add subscriber convergence on ACL tightening.
5. Finish observability redaction wiring.

### Phase 2 (performance/cleanliness)

1. Reduce `AclUserManager` write amplification.
2. Optimize ACL log aggregation structure.
3. Clarify `allowed_category` model role and simplify if possible.

## 7. Acceptance Criteria

1. No unresolved conflict markers in ACL paths.
2. No possibility of connection identity drift under slot reuse.
3. `ACL LOAD` is atomic and namespace-policy-consistent.
4. Tightening channel ACL invalidates or deauthenticates impacted subscribers.
5. No cleartext sensitive ACL payload in monitor/slowlog.
6. Performance regression budget for ACL hot path is controlled with benchmark evidence.

## 8. Key Code Pointers

1. `src/server/acl/acl_user.h`
2. `src/server/acl/acl_user.cc`
3. `src/server/acl/acl.cc`
4. `src/server/acl/acl_actions.cc`
5. `src/server/acl/acl_command_manager.{h,cc}`
6. `src/server/redis_connection.{h,cc}`
7. `src/commands/cmd_server.cc`
8. `src/server/server.cc`
9. `src/server/worker.cc`
10. `src/cluster/replication.cc`
11. `src/config/config.{h,cc}`
