# Kvrocks ACL Redis-Aligned Remediation Plan (Items 1-7)

This document defines a concrete remediation plan for seven review findings, with Redis behavior used as the baseline.
The focus is security, correctness, performance, and architecture cleanliness.

## 1. Scope and Goals

Goals:

1. Close high-risk security gaps first, without introducing behavior regressions.
2. Align runtime ACL behavior with Redis where Redis has explicit semantics.
3. Keep Kvrocks-specific behavior explicit when Redis has no equivalent concept (for example, namespace binding).
4. Make each change testable with clear acceptance criteria.

Out of scope for this plan:

1. Full Redis ACL file lifecycle implementation (`ACL LOAD` / `ACL SAVE`) beyond what is needed for items 1-7.
2. Reworking all ACL preview feature boundaries.

## 2. Baseline References

Redis baseline references:

1. `docs/acl/redis-acl-design.md`
2. Redis command metadata:
   - `ACL SETUSER`: `request_policy: all_nodes`, `response_policy: all_succeeded`
   - `ACL DELUSER`: `request_policy: all_nodes`, `response_policy: all_succeeded`
3. Redis `src/acl.c`:
   - `ACL SETUSER` / `ACL DELUSER` argument redaction
   - `ACLFreeUserAndKillClients()`

Kvrocks implementation references:

1. `src/commands/cmd_server.cc`
2. `src/server/redis_connection.cc`
3. `src/server/server.cc`
4. `src/server/acl/*`
5. `src/cluster/cluster.cc`

## 3. Item-by-Item Plan

### 3.1 Item #1: Authentication failure must not clear valid ACL session state

Problem:

1. `AUTH` / `HELLO AUTH` currently clears ACL profile before authentication result is known.
2. On authentication failure, an already authenticated user session can lose ACL enforcement context.

Redis baseline:

1. Failed authentication does not grant new privileges and should not silently weaken existing authorization state.

Target behavior:

1. Authentication state transitions are atomic:
   - Success: replace identity/profile.
   - Failure: keep prior identity/profile unchanged.

Implementation steps:

1. Introduce a shared helper for auth state transition used by both `AUTH` and `HELLO AUTH`.
2. Move `ClearAclProfile()` from pre-check path to success-only path.
3. Preserve `is_admin`, `namespace`, and ACL profile on `INVALID_PASSWORD` and `NO_REQUIRE_PASS`.
4. Keep error response unchanged (`Invalid password`, etc.).

Files:

1. `src/commands/cmd_server.cc`
2. `src/server/redis_connection.h` (if helper is added)
3. `src/server/redis_connection.cc` (if helper is implemented there)

Tests:

1. `AUTH` failure after successful user auth keeps ACL restrictions active.
2. `HELLO ... AUTH` failure after successful user auth keeps ACL restrictions active.
3. Same validation for admin session and default-user session.

Acceptance criteria:

1. No command can execute with weakened ACL enforcement after failed auth.

### 3.2 Item #2: Sensitive ACL payload must be redacted in observability paths

Problem:

1. `ACL SETUSER` plaintext password modifiers (`>pass`, `<pass`) can leak through `MONITOR` and `SLOWLOG`.

Redis baseline:

1. Redis ACL command handling redacts user-related ACL arguments (`redactClientCommandArgument` in `acl.c`).

Target behavior:

1. No raw ACL password material appears in monitor stream or slowlog entries.

Implementation steps:

1. Extend `Server::RedactSensitiveTokens(...)` to cover:
   - `ACL SETUSER ...` (redact all modifiers after username, or at minimum password-bearing modifiers).
   - `ACL DELUSER ...` and `ACL GETUSER ...` username redaction parity with Redis behavior.
2. Reuse the same redaction helper before writing slowlog arguments.
3. Keep command execution semantics unchanged.

Files:

1. `src/server/server.cc`
2. `src/server/server.h`
3. `src/server/redis_connection.cc` (slowlog path)

Tests:

1. `MONITOR` output for `ACL SETUSER u >p` does not include `p`.
2. `SLOWLOG GET` output for slow `ACL SETUSER` does not include raw password token.
3. `AUTH` / `HELLO AUTH` existing redaction behavior remains unchanged.

Acceptance criteria:

1. All credential-like ACL modifiers are redacted in monitor and slowlog.

### 3.3 Item #3: Cluster ACL management semantics should follow Redis all-nodes policy

Problem:

1. ACL mutation on a single cluster node can cause policy drift across masters.

Redis baseline:

1. ACL commands are operationally all-nodes commands (`request_policy: all_nodes`), but each node still applies locally.
2. Redis does not auto-broadcast ACL mutations through cluster bus.

Target behavior:

1. Keep Redis-compatible local apply semantics by default.
2. Make cluster-wide operation requirements explicit and verifiable.

Implementation steps:

1. Keep current local command semantics for compatibility.
2. Add explicit cluster safety option:
   - `acl-require-cluster-all-nodes` (default `no`).
   - If `yes`, reject local ACL mutation commands in cluster mode unless explicitly forced by an administrative flag/path.
3. Add clear operation guidance in `ACL HELP` and docs:
   - Run ACL mutations on all masters.
4. Add an integration test that demonstrates:
   - Single-node mutation is local (Redis-compatible baseline).
   - Cluster-wide consistency requires multi-node execution.

Files:

1. `src/config/config.cc`
2. `src/config/config.h`
3. `src/commands/cmd_server.cc`
4. `docs/acl/kvrocks-acl-current-design.md`
5. `tests/gocase/unit/acl/acl_test.go` (or cluster integration suite)

Acceptance criteria:

1. Default behavior remains Redis-compatible.
2. Operators can enable strict mode to prevent accidental single-node ACL mutation.

### 3.4 Item #4: Dynamic key references in `SORT` must not bypass ACL key checks

Problem:

1. `SORT BY` / `SORT GET` dynamic pattern references are not fully represented in ACL key checks.

Redis baseline:

1. ACL key permission checks rely on complete key extraction logic.
2. Commands with dynamic key access must not silently bypass ACL key constraints.

Target behavior:

1. No command path can read/write derived keys that were not ACL-authorized.

Implementation steps:

1. Phase 1 (safe guardrail):
   - In ACL-enabled mode, reject `SORT` forms with dynamic `BY`/`GET` patterns unless the user has full key access (`allkeys`).
2. Phase 2 (full alignment):
   - Extend key extraction for `SORT` to emit derived key accesses for ACL validation.
3. Keep cluster slot safety checks aligned with extracted key set.

Files:

1. `src/commands/cmd_key.cc`
2. `src/server/redis_connection.cc` (ACL validation integration if needed)
3. Potentially `src/commands/commander.h` (if key range metadata extensions are needed)

Tests:

1. ACL user without `allkeys` is denied on dynamic `SORT BY/GET` patterns.
2. ACL user with `allkeys` still works.
3. Cluster mode cross-slot cases remain safe.

Acceptance criteria:

1. No dynamic key path bypasses ACL key enforcement.

### 3.5 Item #5: Missing ACL runtime context must fail closed

Problem:

1. If ACL context lookup fails at runtime, current behavior clears profile and returns `NOPERM`, which can leave a weakened long-lived session.

Redis baseline:

1. Redis closes or deauthenticates clients when ACL user identity becomes invalid (for example on user deletion).

Target behavior:

1. ACL context mismatch/absence must fail closed:
   - connection deauthenticated and forced to re-authenticate, or
   - connection closed.

Implementation steps:

1. Replace `ClearAclProfile()` on context-miss path with fail-closed handling:
   - clear session auth state (`namespace` empty, profile cleared) and return `NOAUTH`, or
   - close connection after reply.
2. Keep `DELUSER` kill path unchanged (already aligned).
3. Ensure replicated ACL deletion path remains equivalent.

Files:

1. `src/server/redis_connection.cc`
2. `src/server/redis_connection.h`

Tests:

1. Simulate ACL user deletion/reload while connection is active, then issue command:
   - expected: re-auth required or connection closed.
2. Ensure no post-miss command can execute without re-authentication.

Acceptance criteria:

1. ACL context loss can never degrade into permissive execution.

### 3.6 Item #6: Namespace derivation must not silently broaden access

Problem:

1. Kvrocks-specific `username#namespace` parsing currently falls back to default namespace on unknown namespace.

Redis baseline:

1. Redis has no namespace coupling in ACL user identity.
2. Invalid user rule inputs should fail fast instead of silently changing security scope.

Target behavior:

1. Namespace-binding failures are explicit by default.

Implementation steps:

1. Add strict namespace validation mode:
   - `acl-namespace-strict` (default `yes`).
2. Under strict mode, `ACL SETUSER user#ns ...` fails when `ns` does not exist.
3. Keep optional compatibility mode (`no`) to preserve current fallback for controlled migrations.

Files:

1. `src/config/config.cc`
2. `src/config/config.h`
3. `src/server/acl/acl.cc`

Tests:

1. Strict mode: unknown namespace in username suffix is rejected.
2. Compatibility mode: legacy fallback behavior preserved.
3. Existing namespace token constraints with cluster mode remain valid.

Acceptance criteria:

1. Default configuration is fail-fast and does not silently broaden namespace scope.

### 3.7 Item #7: Performance and architecture cleanup without semantic drift

Problem:

1. ACL runtime path still has avoidable overhead and mixed responsibilities.
2. ACL user manager write path copies full slot vector on each update.

Redis baseline:

1. Redis ACL runtime is optimized around command IDs/bitmaps and clear separation between parse/apply/check paths.

Target behavior:

1. Preserve behavior while reducing per-command overhead and tightening component boundaries.

Implementation steps:

1. Command fast path:
   - Cache ACL command bit/index in command metadata so `IsCommandAllowed` avoids map lookup/lock on hot path.
2. ACL user storage write path:
   - Move from full-vector copy to chunked snapshot copy or segmented RCU-like structure.
3. Architecture split:
   - Extract pure `AclEvaluator` (selector/command/key/channel checks) from `Connection` class.
   - Keep connection state transitions and ACL policy evaluation as separate units.

Files:

1. `src/server/acl/acl_command_manager.{h,cc}`
2. `src/commands/commander.h` (or command metadata structure)
3. `src/server/acl/acl_user.{h,cc}`
4. `src/server/redis_connection.cc`

Tests and validation:

1. Unit tests for evaluator behavior parity before/after refactor.
2. Microbenchmark on ACL check hot path.
3. No change in functional ACL compatibility tests.

Acceptance criteria:

1. Measurable reduction in ACL check overhead under high QPS workloads.
2. No behavior regression in existing ACL integration tests.

## 4. Delivery Order

Priority order:

1. P0: Item #1, #2, #5
2. P1: Item #4, #6
3. P2: Item #3, #7

Rationale:

1. P0 closes direct security exposures.
2. P1 closes correctness and policy-scope ambiguity.
3. P2 addresses operational safety and long-term maintainability/performance.

## 5. Test Matrix (Minimum)

Core suites to update:

1. `tests/gocase/unit/acl/acl_test.go`
2. Cluster ACL integration tests (add if missing)
3. Optional C++ unit tests for parser/evaluator internals in `tests/cppunit/`

Mandatory new cases:

1. Failed `AUTH` does not weaken existing ACL session.
2. `ACL SETUSER` secrets are redacted in monitor and slowlog.
3. Context-miss fail-closed behavior.
4. `SORT` dynamic pattern ACL guardrail.
5. Strict namespace mode behavior.
6. Cluster single-node ACL mutation behavior documentation test (and strict mode behavior if enabled).

## 6. Compatibility Notes

1. Items #1, #2, #5 are security hardening and should be backward-compatible for valid clients.
2. Item #6 changes default behavior only if strict mode is enabled by default; communicate in release notes.
3. Item #3 strict cluster mode is opt-in to preserve Redis-compatible default local semantics.

