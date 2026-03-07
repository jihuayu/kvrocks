# Proposal: First-Class Subcommand Support

## Background

Kvrocks currently models command dispatch around top-level commands.
Many of these commands are effectively command families with multiple subcommands, where behavior, arity, flags,
key-spec, and privilege semantics vary by subcommand.

Today, subcommand logic is implemented ad hoc inside individual `Commander::Parse()` or `Commander::Execute()`
methods, typically by reading some argv position and branching through `if` / `else` trees.
Current examples include `CONFIG`, `COMMAND`, and `NAMESPACE` in `src/commands/cmd_server.cc`, and `CLUSTER` in
`src/commands/cmd_cluster.cc`.

This works for functional behavior, but framework-level consumers still cannot answer a few basic questions reliably:

- What is the canonical identity of the request?
- What is the authoritative list of subcommands?
- How should subcommand-specific flags and key extraction be represented?

This proposal introduces `Subcommand` as a system-level, first-class abstraction in the command registration model.
ACL is a major driver, but the abstraction is not ACL-specific.

## Motivation

### Problems in the current model

#### No authoritative subcommand list

Subcommands are encoded in command code paths, not in shared command metadata.
Framework consumers cannot enumerate or validate subcommands from one authoritative source.

#### No shared resolution step

ACL, cluster routing and concurrency, observability tagging and redaction, and future introspection all want the same
resolved command identity (`cmd` or `cmd|sub`).
Today there is no framework-level resolution object that all of them can use.

#### Metadata is only command-level

`CommandAttributes` is registered for a top-level command, but subcommands frequently differ in arity, flags,
key-specs, and privilege semantics.

### Concrete benefits

#### ACL

ACL needs a stable subcommand identity and subcommand-level metadata.

#### Cluster concurrency and exclusivity

Subcommands under the same root command can have different concurrency semantics.
For example, `CLUSTER` currently resolves those differences in ad hoc logic instead of framework metadata.

#### COMMAND-style introspection

`COMMAND GETKEYS` and `COMMAND INFO` can become more precise for command families once `cmd|sub` is explicit.

#### Observability and safety

Monitor and slowlog labeling, audit tagging, and redaction can all use the canonical full name instead of a root-only
label.

## Goals and non-goals

### Goals

- Introduce a first-class subcommand abstraction in the command system.
- Provide one unified resolved-command identity (`cmd` or `cmd|sub`) for framework consumers.
- Preserve compatibility with existing `REDIS_REGISTER_COMMANDS(...)` registration and existing command behavior.
- Enable incremental adoption one command family at a time.

### Non-goals

- This is not a mass refactor; families can migrate independently.
- This does not change behavior by itself; semantics stay identical unless a follow-up change explicitly modifies them.
- This does not require every subcommand to move into a separate `Commander` subclass immediately.

## Proposed design

### Terminology

- **root command**: the top-level command name, for example `config`
- **subcommand**: the identifier inside a command family, for example `get`
- **canonical full name**: `root|sub`, for example `config|get`

Canonical names are internal identities used for:

- ACL rules (`+cmd|sub`)
- logging, audit, and metrics labeling
- future introspection metadata

### Registration-time metadata

The design is intentionally incremental.
It reuses `CommandAttributes` as the base metadata record and adds subcommand-family metadata around it:

```text
root command -> SubcommandFamily
```

`SubcommandFamily` contains:

- a `SubcommandResolver`, which knows how to locate and interpret the subcommand token from argv
- a subcommand table, mapping `sub` to `const CommandAttributes *`

#### `SubcommandResolver`

Not every command family places the subcommand in `argv[1]`:

- container commands: `COMMAND <sub> ...` -> sub at `argv[1]`
- key-first families: `XGROUP <key> <sub> ...` -> sub at `argv[2]`

The resolver must be command-family-specific, but centrally registered.

### Runtime output: `ResolvedCommand`

Introduce a framework-level `ResolvedCommand` result that can be shared across subsystems:

- `root` (lowercased)
- optional `sub` (lowercased)
- `attributes` (the effective `const CommandAttributes *` used for arity, flags, and key extraction)
- `FullName()`, computed as `root|sub` when a subcommand is present

This object becomes the authoritative answer to "what command is this request?" for command execution, ACL, routing,
observability, and future introspection work.

### Dispatch model

To avoid forcing large refactors up front, the framework should support two migration modes:

#### Metadata-only mode

The framework resolves to `root|sub` for identity and metadata purposes, but the instantiated handler can still reuse
the existing root-command implementation.
This allows subcommand-specific metadata to land before command-family code is split apart.

#### Flat dispatch mode

The framework resolves to `root|sub` and instantiates the handler registered for that specific subcommand.

Both modes use the same registry and the same resolution logic.

## Compatibility and impact on existing code

### Existing registration remains valid

All existing `REDIS_REGISTER_COMMANDS(...)` registrations continue to work.
Only command families that explicitly register subcommand metadata opt into the new resolution path.

### Performance expectations

The runtime overhead should stay bounded:

- a small constant amount of lowercasing and map lookup when a family is registered
- no extra work for commands without subcommand metadata
- optional caching of `ResolvedCommand` in the request loop if repeated resolution becomes visible on hot paths

## Incremental migration plan

### Phase 1: framework scaffolding

Add the subcommand registry types and registration API under `src/commands/`.
Add `ResolvedCommand` and `CommandTable::Resolve(...)`.
Add argv-based command lookup in `Server` while keeping the existing string-based overload.

### Phase 2: dual-path resolution

Switch `Connection::ExecuteCommands` to use argv-based lookup.
Default all command families to compatibility behavior until they opt in.

### Phase 3: opt-in families

For each command family, for example `COMMAND`, `CLIENT`, `CONFIG`, `CLUSTER`, and later `ACL` in downstream work:

- register the subcommand family resolver and authoritative subcommand list
- keep the existing implementation in metadata-only mode first
- optionally split into per-subcommand handlers later and move to flat dispatch

## Illustrative examples

The following canonical names are examples only and are not required in this change:

- `command|count`
- `command|getkeys`
- `command|info`
- `config|get`
- `config|set`
- `config|rewrite`

## Code touch points

This section is meant for reviewers.
It lists the framework-level areas that would need to change to introduce this abstraction without requiring immediate
command-family refactors.

### Add a subcommand registry under `src/commands/`

Suggested new files:

- `src/commands/subcommand_registry.h`
- `src/commands/subcommand_registry.cc`

Suggested responsibilities:

- define a `SubcommandResolver` type that operates on `std::vector<std::string> argv`
- define `SubcommandFamily` with:
  - `resolver`
  - `std::map<std::string, const CommandAttributes *>` (or `unordered_map` if preferred)
- provide registry APIs such as:
  - `RegisterFamily(parent, resolver)`
  - `RegisterSubcommand(parent, sub, attributes*)`
  - `GetFamily(parent)`
  - `LookupSubcommand(parent, sub)`

### Add `ResolvedCommand`

Suggested location:

- `src/commands/commander.h`

Suggested responsibilities:

- carry `root`, optional `sub`, and the resolved `const CommandAttributes *`
- provide `FullName()` computed on demand instead of preallocating a string on the hot path

### Extend `CommandTable` with resolution

Files:

- `src/commands/commander.h`
- `src/commands/commander.cc`

Change:

- add `CommandTable::Resolve(const std::vector<std::string> &cmd_tokens)` returning `ResolvedCommand`

Implementation sketch:

1. Look up the root command in `CommandTable::Get()` as today.
2. If no family metadata exists for that root, return the root attributes.
3. Otherwise:
   - call the family resolver to obtain an optional subcommand
   - if the subcommand exists and is registered, return its attributes
   - if the subcommand exists but is unknown, return an unknown-subcommand error
   - if the resolver does not produce a subcommand, fall back to the root attributes

### Add registration helpers for subcommands

Suggested file:

- `src/commands/commander.h`

Suggested additions:

- a macro similar to `REDIS_REGISTER_COMMANDS`, for example `REDIS_REGISTER_SUBCOMMANDS(parent, ...)`
- a helper similar to `MakeCmdAttr<T>`, for example `MakeSubCmdAttr<T>(parent, sub, ...)`

The helper should set `CommandAttributes::name` to the canonical full name (`parent|sub`) so that the resolved
attributes carry a stable identity for ACL, logging, and observability consumers.

### Add argv-based lookup in `Server`

Files:

- `src/server/server.h`
- `src/server/server.cc`

Change:

- add `StatusOr<std::unique_ptr<redis::Commander>> LookupAndCreateCommand(const std::vector<std::string> &cmd_tokens)`
- keep the existing `LookupAndCreateCommand(const std::string &cmd_name)` overload for backward compatibility

Suggested responsibilities:

- call `CommandTable::Resolve(cmd_tokens)` to obtain the effective `CommandAttributes *`
- instantiate the command via `attributes->factory()`
- call `SetAttributes(attributes)` on the created commander

### Switch request execution to argv-based lookup

File:

- `src/server/redis_connection.cc`

Current baseline:

- `Connection::ExecuteCommands` still calls `Server::LookupAndCreateCommand(cmd_tokens.front())`

Change:

- replace that call with `Server::LookupAndCreateCommand(cmd_tokens)`

Rationale:

Subcommand-specific arity, flags, and key extraction need the effective attributes to be selected before the existing
arity and execution checks run.
