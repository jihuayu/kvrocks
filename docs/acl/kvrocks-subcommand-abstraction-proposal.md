# Proposal: Subcommand as a First-Class Abstraction in Kvrocks Command System
Date: 2026-03-04
Status: Draft (Reorganized to be upstream-based)

## 1. Background

Kvrocks currently models command dispatch around **top-level commands**.
Many of these commands are effectively **command families** with multiple subcommands, where behavior, arity, flags,
key-spec, and privilege semantics vary by subcommand.

In upstream today, subcommand logic is implemented ad-hoc inside each `Commander::Parse()` or `Commander::Execute()`,
typically by parsing some argv position (often `args[1]`) and branching via `if/else` trees.

This works for functional behavior, but system-level features cannot reliably answer:

- What is the canonical identity of the request?
- What is the authoritative list of subcommands?
- How should subcommand-specific flags and key extraction be represented?

This proposal introduces `Subcommand` as a **system-level, first-class abstraction** as an upgrade to the command
registration model. ACL is a major driver, but the abstraction is not ACL-specific.

## 2. Motivation

### 2.1 Problems in the current model

1. No authoritative subcommand list
   - Subcommands are encoded in code paths, not in command metadata.
2. System consumers cannot share one resolution step
   - ACL, cluster routing/concurrency, observability tagging/redaction, and future introspection all want "resolved
     command" (`cmd|sub`), but there is no framework-level object to provide it.
3. Metadata is only command-level
   - `CommandAttributes` is registered for a top-level command, but subcommands often differ in arity/flags/keys.
4. Strict validation is hard
   - Without system metadata, `+cmd|sub` cannot be validated in a principled way.

### 2.2 Concrete benefit examples

1. Cluster concurrency / exclusivity
   - `CLUSTER` subcommands have different concurrency semantics; this is currently encoded as ad-hoc logic.
2. `COMMAND`-style introspection
   - `COMMAND GETKEYS` and `COMMAND INFO` can become more precise for command families once `cmd|sub` is explicit.
3. Observability and safety
   - Monitor/slowlog redaction, audit tagging, and stats can label events by canonical `cmd|sub`.
4. Acl need subcommands metadata.

## 3. Goals and Non-goals

### 3.1 Goals

1. Introduce a first-class `Subcommand` abstraction in the command system.
2. Provide a unified "resolved command" identity (`cmd` or `cmd|sub`) for system consumers.
3. Preserve compatibility for existing command registration (`REDIS_REGISTER_COMMANDS`) and existing command behavior.
4. Enable incremental adoption per command family.

### 3.2 Non-goals

1. Not a mass refactor: command families can migrate one by one.
2. Not a behavior change by itself: semantics must remain identical unless explicitly changed in follow-up PRs.
3. Not a requirement to split all subcommands into separate `Commander` classes immediately.

## 4. Proposed Design

### 4.1 Terminology

- `root command`: the top-level command name (e.g. `config`)
- `subcommand`: the subcommand identifier within a command family (e.g. `get`)
- `canonical full name`: `root|sub` (e.g. `config|get`)

Canonical names are internal identities used for:

- ACL rules (`+cmd|sub`)
- logging/audit/metrics labeling
- future introspection metadata

### 4.2 Data Model (Registration-Time Metadata)

The design is intentionally incremental: it reuses existing `CommandAttributes` as the base metadata record, and adds
subcommand-family metadata:

- `root command` -> `SubcommandFamily`
- `SubcommandFamily` contains:
  - `SubcommandResolver`: how to locate/interpret the sub token from argv
  - `subcommand table`: `sub` -> `CommandAttributes*` for that subcommand
  - optional `strict` toggle to control error behavior for unknown subcommands

#### 4.2.1 SubcommandResolver

Not every family uses `argv[1]`:

- Container commands: `COMMAND <sub> ...` => sub at `argv[1]`
- Key-first families: `XGROUP <key> <sub> ...` => sub at `argv[2]`

The resolver must be command-family-specific, but centrally registered.

### 4.3 ResolvedCommand (Runtime Output)

Introduce a framework-level `ResolvedCommand` result that can be used across subsystems:

- `root` (lowercased)
- optional `sub` (lowercased)
- `attributes` (the effective `CommandAttributes*` used for arity/flags/key extraction)
- `FullName()` (computed as `root|sub` when `sub` is present)

### 4.4 Dispatch Model (Two modes)

To avoid requiring large refactors, the framework supports two modes during migration:

1. **Metadata-only mode**
   - The framework resolves to `root|sub` for identity/metadata purposes.
   - The handler instantiated may still be the root command implementation (legacy internal dispatch).
2. **Flat dispatch mode**
   - The framework resolves to `root|sub` and directly instantiates the handler registered for that subcommand.

Both are compatible with the same registry and resolution logic.

### 4.5 Unknown subcommand behavior (per-family)

Per command family, support:

- `compat` (default): if subcommand cannot be resolved/recognized, fall back to root command behavior.
- `strict`: unknown subcommand returns a dedicated error (once the family has complete metadata coverage).

This avoids breaking existing behaviors while enabling strictness where needed.

## 5. Compatibility and Impact on Existing Code

### 5.1 Existing command registration remains valid

- All existing `REDIS_REGISTER_COMMANDS(...)` registrations continue to work.
- Only command families that explicitly register subcommand metadata opt into the new resolution path.

### 5.2 Performance expectations

The runtime overhead should be bounded:

- A small constant amount of lowercasing + map lookup when a family is registered.
- No extra work for commands without subcommand metadata.
- Optional caching of `ResolvedCommand` in the request loop to avoid repeated resolution.

## 6. Incremental Migration Plan 

### Phase 1: Framework scaffolding (no behavior changes)

1. Add subcommand registry types and registration API in `src/commands/`.
2. Add `ResolvedCommand` and `CommandTable::Resolve(...)`.
3. Add argv-based command lookup in `Server` and keep the existing string-based overload.

### Phase 2: Dual-path resolution

1. Switch `Connection::ExecuteCommands` to use argv-based lookup.
2. Default all families to `compat` behavior until opted-in.

### Phase 3: Opt-in families

For each command family (e.g. `COMMAND`, `CLIENT`, `CONFIG`, `CLUSTER`, later `ACL` in downstream branches):

1. Register subcommand family resolver and subcommand list.
2. Keep the existing implementation (metadata-only mode).
3. Optionally split into per-subcommand handlers and switch to flat dispatch later.

## 7. Example (Illustrative; not required in this change)

Example canonical names:

- `command|count`, `command|getkeys`, `command|info`
- `config|get`, `config|set`, `config|rewrite`


## 8. Code Touch Points 

This section is intended for reviewers. It lists concrete code locations in upstream and the minimal set of changes
needed to introduce this abstraction (without requiring command-family refactors).

### 8.1 New code to add (framework-level)

1. Add a subcommand registry implementation under `src/commands/`
   - Suggested new files:
     - `src/commands/subcommand_registry.h`
     - `src/commands/subcommand_registry.cc`
   - Responsibilities:
     - Define `SubcommandResolver` type that operates on `std::vector<std::string>` argv.
     - Define `SubcommandFamily` with:
       - resolver
       - `std::map<std::string, const CommandAttributes*>` (or `unordered_map` if preferred)
       - `strict` toggle (optional)
     - Provide a registry singleton with APIs:
       - `RegisterFamily(parent, resolver, strict_mode)`
       - `RegisterSubcommand(parent, sub, attributes*)`
       - `GetFamily(parent)` / `LookupSubcommand(parent, sub)`

2. Add a `ResolvedCommand` representation
   - Suggested location:
     - `src/commands/commander.h` (type declaration near `CommandAttributes`)
   - Responsibilities:
     - Carry `root`, optional `sub`, and resolved `const CommandAttributes*`.
     - Provide `FullName()` computed on demand (do not allocate on hot path).

### 8.2 Modify existing code (command registration and resolution)

1. Extend `CommandTable` with a resolution helper
   - Files:
     - `src/commands/commander.h`
     - `src/commands/commander.cc`
   - Change:
     - Add `CommandTable::Resolve(const std::vector<std::string> &cmd_tokens)` returning `ResolvedCommand`.
   - Implementation sketch:
     - Lookup root in `CommandTable::Get()` (as today).
     - If no family metadata exists for this root, return root attributes.
     - Else:
       - call resolver to get optional `sub`
       - if `sub` exists and is registered, return its attributes
       - otherwise:
         - if family is strict: return unknown-subcommand error
         - else: return root attributes

2. Add a subcommand registration API/macro (optional but recommended for ergonomics)
   - File:
     - `src/commands/commander.h`
   - Change:
     - Add a macro similar to `REDIS_REGISTER_COMMANDS`:
       - `REDIS_REGISTER_SUBCOMMANDS(parent, ...)`
     - Add a helper similar to `MakeCmdAttr<T>`:
       - `MakeSubCmdAttr<T>(parent, sub, ...)`
   - Notes:
     - The helper should set the `CommandAttributes::name` to the canonical full name (`parent|sub`) so the attributes
       carry a stable identity usable by system consumers.

### 8.3 Modify existing code (core dispatch path)

1. Add argv-based lookup in `Server`
   - Files:
     - `src/server/server.h`
     - `src/server/server.cc`
   - Change:
     - Add:
       - `StatusOr<std::unique_ptr<redis::Commander>> LookupAndCreateCommand(const std::vector<std::string> &cmd_tokens);`
     - Keep existing:
       - `LookupAndCreateCommand(const std::string &cmd_name)`
       - It may delegate to the argv-based overload for backward compatibility.
   - Responsibilities:
     - Call `CommandTable::Resolve(cmd_tokens)` to obtain effective `CommandAttributes*`.
     - Instantiate the command via `attributes->factory()` and `SetAttributes(attributes)`.

2. Switch request execution to argv-based lookup
   - File:
     - `src/server/redis_connection.cc`
   - Baseline reference (upstream):
     - `Connection::ExecuteCommands` currently calls `Server::LookupAndCreateCommand(cmd_tokens.front())`.
   - Change:
     - Replace with `Server::LookupAndCreateCommand(cmd_tokens)`.
   - Rationale:
     - Subcommand-specific arity/flags/key extraction requires selecting effective attributes before arity checks.

### 8.4 Optional follow-ups (explicitly not required for the framework PR)

1. `COMMAND GETKEYS` can resolve subcommands for accuracy
   - File:
     - `src/commands/cmd_server.cc` (`CommandCommand` subcommand `getkeys`)
   - Idea:
     - Introduce `CommandTable::GetKeysFromCommand(const std::vector<std::string> &cmd_tokens)` that resolves to the
       effective attributes before parsing.
2. Introspection output can include subcommands
   - Can be added once enough families have metadata coverage.
