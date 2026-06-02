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
  loopback, and non-loopback binds also require `security.bearerTokens`;
- daemon/admin audit events have a stable schema in `docs/audit_events.md`;
- tool data-plane audit events cover `tools/list`, `tools/call`, and policy
  denies through the `cxxmcp-gateway` runtime observer;
- data-plane exact-match tool allow/deny policy is enforced by the
  `cxxmcp-gateway` runtime for `tools/list` and `tools/call`;
- exact-match resource and prompt allow/deny policy is enforced by the
  `cxxmcp-gateway` runtime for concrete `resources` and `prompts` operations;
- downstream static bearer auth is wired through the SDK auth provider for
  admin and profile MCP endpoints via `security.bearerTokens`;
- fixed-window request admission is wired through the SDK rate limiter hook via
  `security.rateLimit`;
- upstream credential strings can use the `cxxmcp-gateway` `${ENV_NAME}`
  expansion path, and admin tools avoid returning configured headers or
  child-process environment maps;
- resource-template-specific and broader policy families remain future work.

## Phase 4: UI

- admin API-backed dashboard;
- profile editor;
- upstream enable/disable controls;
- catalog inspection;
- logs/events view.

The UI should remain a consumer of gatewayd admin APIs. UI lifecycle must not
own the MCP data-plane lifecycle.

Current status:

- admin API-backed dashboard data is available through `gatewayd.health`,
  `gatewayd.profiles`, `gatewayd.upstreams`, and `gatewayd.events`;
- profile runtime editing is available through
  `gatewayd.profile.runtime.set`;
- upstream enable/disable controls are available through
  `gatewayd.upstream.enable` and `gatewayd.upstream.disable`;
- catalog inspection is available through `gatewayd.catalog.tools`,
  `gatewayd.catalog.resources`, and `gatewayd.catalog.prompts`;
- a minimal admin API-backed CLI dashboard is available through
  `cxxmcp-gatewayd dashboard`;
- a static graphical dashboard snapshot can be generated with
  `cxxmcp-gatewayd dashboard --html <file>`;
- a live graphical dashboard remains future work.
