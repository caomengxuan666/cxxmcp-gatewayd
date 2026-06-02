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

Current status:

- config discovery is implemented for `CXXMCP_GATEWAYD_CONFIG`,
  `gatewayd.json`, and `gatewayd.config.json`;
- upstream enable/disable operations persist to the loaded config file and
  update desired in-memory config;
- `gatewayd.reload` reloads and validates profile config, while admin endpoint
  changes require daemon restart;
- `gatewayd.health`, `gatewayd.upstreams`, and `gatewayd.events` provide the
  first status/event surface;
- CLI management commands cover `status`, `upstreams`, `events`, `reload`, and
  upstream enable/disable, plus `diagnostics`;
- loopback defaults are documented for the current local middleware mode.

## Phase 2: Operational Service

- Windows service support;
- Linux systemd support;
- macOS launchd support;
- packaged binaries;
- crash-safe shutdown and restart behavior;
- logs and diagnostics.

Current status:

- CPack ZIP/TGZ packaging is configured;
- Windows service scripts, systemd unit, and launchd plist templates are
  included under `packaging/`;
- `diagnostics` aggregates status, upstreams, and events through the admin
  endpoint;
- service-manager restart templates provide the first restart-on-failure shape.

## Phase 3: Policy And Security

- downstream authentication when binding outside loopback;
- upstream credential handling;
- allow/deny policy for tools and other capability families;
- audit event schema;
- rate-limit hooks.

Current status:

- loopback-only binding is enforced by default for admin and profile endpoints;
- configs must set `security.allowNonLoopback=true` before binding outside
  loopback;
- downstream auth, credential handling, policy, audit, and rate-limit hooks
  remain future work.

## Phase 4: UI

- admin API-backed dashboard;
- profile editor;
- upstream enable/disable controls;
- catalog inspection;
- logs/events view.

The UI should remain a consumer of gatewayd admin APIs. UI lifecycle must not
own the MCP data-plane lifecycle.
