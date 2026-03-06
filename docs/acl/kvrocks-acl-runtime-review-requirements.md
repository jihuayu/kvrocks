# Kvrocks ACL Runtime Review Requirements (2026-03-05)

This document converts the ACL runtime code review conclusions into explicit engineering requirements.

## 1. Scope

These requirements apply to the ACL runtime and ACL-related benchmark tooling in the following areas:

- ACL cache refresh logic in connection command checks.
- ACL channel permission semantics, especially for `PSUBSCRIBE`.
- ACL benchmark script maintainability and execution determinism.
- Regression tests for externally observable ACL behavior.

## 2. Confirmed Constraints

The following constraints were confirmed and must be treated as baseline assumptions:

1. Deployment target is current x86 environments only.
2. No ARM/weak-memory-model deployment target is required at this stage.
3. `PSUBSCRIBE` ACL behavior must remain Redis-compatible.

## 3. Functional Requirements

### R-ACL-001: Preserve Redis-compatible channel ACL semantics

The ACL runtime must preserve command-specific channel matching behavior:

1. `PUBLISH`, `SPUBLISH`, `SUBSCRIBE`, and `SSUBSCRIBE` use glob-style channel matching.
2. `PSUBSCRIBE` uses literal-pattern ACL matching (no glob expansion of the command argument in ACL check).
3. Any ACL fast path optimization must not bypass the rule in (2).

Acceptance criteria:

1. Existing and new ACL tests for `PSUBSCRIBE` continue to pass with no behavior regression.
2. A user permitted by `&news:*` can `PSUBSCRIBE news:*`.
3. The same user is denied for `PSUBSCRIBE news:1` unless ACL rules explicitly allow it.

### R-ACL-002: ACL cache invalidation must be safe and immediate on next command

After ACL mutation, authenticated connections must observe updated ACL identity/permissions on the next ACL-checked command.

ACL mutation includes:

- `ACL SETUSER` updates.
- `ACL DELUSER` deletions.
- ACL load/reload operations.
- Replicated ACL update/deletion application.

Acceptance criteria:

1. If a user is deleted, subsequent commands on a previously authenticated connection fail closed (`NOAUTH` or connection closed by policy), never silent allow.
2. If a user is downgraded (commands/keys/channels changed), previously authenticated connections must observe the downgraded permissions on the next command.
3. Disabling a user (`off`) blocks new authentication but does not forcibly deauthenticate existing authenticated sessions (Redis-compatible behavior).
4. Version-based invalidation tests must cover both direct mutations and replicated mutations.

### R-ACL-003: ACL unrestricted fast path must be semantics-safe

The unrestricted shortcut is allowed only when semantics are fully preserved.

1. The shortcut must not weaken command/key/channel checks.
2. The shortcut must not alter `PSUBSCRIBE` behavior defined in R-ACL-001.
3. If proof is unclear, fallback to normal ACL checks.

Acceptance criteria:

1. Add targeted regression tests where unrestricted path and normal path produce identical allow/deny decisions.
2. Test matrix includes mixed selector configurations (`allcommands`, explicit channels, key patterns, and `PSUBSCRIBE`).

## 4. Non-Functional Requirements

### R-ACL-NFR-001: Memory-order portability policy (deferred by scope)

Given current confirmed constraints (x86-only), no immediate release blocker is raised for relaxed-memory ordering in ACL version counters.

However, this must be documented as a portability guardrail:

1. If non-x86 (or weak memory model) support is introduced, ACL version synchronization must be upgraded to explicit acquire/release semantics (or stronger).
2. Multi-threaded regression tests must be added before enabling such targets.

## 5. Tooling and Maintainability Requirements

### R-ACL-TOOL-001: Benchmark script functions must be single-source

ACL benchmark scenario functions must not be duplicated in the same script.

1. Every scenario function name (for example, `run_s11_admin`) must have exactly one definition.
2. Performance timing helper logic must be centralized and reused.

Acceptance criteria:

1. Script grep check confirms a single definition for each `run_sXX_*` function.
2. Running the script executes the intended (documented) implementation path without shadowed function overrides.

## 6. Required Test Additions/Updates

The ACL test suite must include or preserve coverage for:

1. `PSUBSCRIBE` literal ACL matching compatibility.
2. ACL cache invalidation after user delete/disable/update.
3. Selector OR semantics under command/key/channel checks.
4. Unrestricted fast path equivalence against normal ACL checks.
5. Replicated ACL mutation invalidation behavior.

## 7. Out of Scope

The following are intentionally out of scope for this requirement set:

1. Full ACL feature parity expansion beyond reviewed runtime paths.
2. ARM/weak-memory production hardening in the current milestone.
3. Redesign of ACL data model or command grammar.
