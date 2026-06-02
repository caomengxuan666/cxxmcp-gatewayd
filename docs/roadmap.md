# Roadmap

## Phase 0: Feasibility Daemon

- run one or more gateway profiles;
- expose one MCP endpoint per profile;
- expose a separate admin endpoint;
- load a JSON config file;
- validate config without starting endpoints;
- report health, upstreams, and tool catalog state;
- update in-memory upstream enabled state;
- restart a profile to apply changed desired config.

## Phase 1: Local Middleware MVP

- stable config file discovery;
- explicit persisted enable/disable upstream operations;
- reload validated config;
- status and event APIs;
- CLI commands for `run`, `status`, `reload`, and upstream management;
- documented loopback-only defaults.

## Phase 2: Operational Service

- Windows service support;
- Linux systemd support;
- macOS launchd support;
- packaged binaries;
- crash-safe shutdown and restart behavior;
- logs and diagnostics.

## Phase 3: Policy And Security

- downstream authentication when binding outside loopback;
- upstream credential handling;
- allow/deny policy for tools and other capability families;
- audit event schema;
- rate-limit hooks.

## Phase 4: UI

- admin API-backed dashboard;
- profile editor;
- upstream enable/disable controls;
- catalog inspection;
- logs/events view.

The UI should remain a consumer of gatewayd admin APIs. UI lifecycle must not
own the MCP data-plane lifecycle.
