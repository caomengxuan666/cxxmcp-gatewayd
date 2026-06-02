# cxxmcp-gatewayd

`cxxmcp-gatewayd` is a local MCP middleware daemon built on top of
`cxxmcp-gateway`.

The daemon is the product-shaped operational shell. The gateway library remains
the data-plane kernel that aggregates and routes MCP capabilities. This split
keeps daemon lifecycle, admin APIs, profile management, packaging, and future
UI work out of the reusable gateway core/runtime libraries.

## Shape

```text
Codex / Claude / IDE / agent
  -> one local MCP endpoint
  -> cxxmcp-gatewayd
  -> multiple upstream MCP servers
```

The MVP exposes separate endpoints:

```text
MCP data plane:
  http://127.0.0.1:39931/mcp/default

Admin MCP endpoint:
  http://127.0.0.1:39932/admin
```

The admin endpoint is intentionally MCP-based for the first feasibility pass. A
future version can replace or complement it with REST, events, CLI integration,
or a GUI without changing `cxxmcp-gateway` data-plane APIs.

## Build

Use an installed `cxxmcp` SDK and a local `cxxmcp-gateway` source tree while
this daemon is being developed:

```powershell
cmake -S . -B build -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -Dcxxmcp_DIR=C:\Users\cmx\repo\cxxmcp-sdk-perf-install\lib\cmake\cxxmcp `
  -DCXXMCP_GATEWAYD_GATEWAY_SOURCE_DIR=C:\Users\cmx\repo\cxxmcp-gateway

cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

When `cxxmcp-gateway` is installed as a package, omit
`CXXMCP_GATEWAYD_GATEWAY_SOURCE_DIR` and pass `cxxmcp-gateway_DIR` instead.

## Run

Validate the sample config:

```powershell
build\cxxmcp-gatewayd.exe validate --config examples\gatewayd.example.json
```

Run the daemon:

```powershell
build\cxxmcp-gatewayd.exe run --config examples\gatewayd.example.json
```

Without `--config`, `run` and `validate` discover config in this order:

1. `CXXMCP_GATEWAYD_CONFIG`
2. `gatewayd.json`
3. `gatewayd.config.json`

The sample upstreams are disabled, so the daemon can start without external MCP
servers. Enable real upstreams to test end-to-end routing.

The default admin CLI target is `http://127.0.0.1:39932/admin`. Override it with
`--admin-url <url>` or `CXXMCP_GATEWAYD_ADMIN_URL`. When bearer auth is enabled,
pass `--bearer-token <token>` or set `CXXMCP_GATEWAYD_ADMIN_TOKEN`.

```powershell
build\cxxmcp-gatewayd.exe status
build\cxxmcp-gatewayd.exe profiles
build\cxxmcp-gatewayd.exe upstreams
build\cxxmcp-gatewayd.exe catalog tools
build\cxxmcp-gatewayd.exe catalog resources
build\cxxmcp-gatewayd.exe catalog prompts
build\cxxmcp-gatewayd.exe dashboard
build\cxxmcp-gatewayd.exe dashboard --html build\gatewayd-dashboard.html
build\cxxmcp-gatewayd.exe events
build\cxxmcp-gatewayd.exe diagnostics
build\cxxmcp-gatewayd.exe reload
build\cxxmcp-gatewayd.exe upstream enable default filesystem
build\cxxmcp-gatewayd.exe upstream disable default filesystem
build\cxxmcp-gatewayd.exe profile runtime set default `
  --session-mode persistent --pool-size 2 --prewarm true
```

## Current Admin Tools

- `gatewayd.health`
- `gatewayd.profiles`
- `gatewayd.upstreams`
- `gatewayd.catalog.tools`
- `gatewayd.catalog.resources`
- `gatewayd.catalog.prompts`
- `gatewayd.events`
- `gatewayd.reload`
- `gatewayd.upstream.enable`
- `gatewayd.upstream.disable`
- `gatewayd.profile.restart`
- `gatewayd.profile.runtime.set`

`gatewayd.upstream.enable` and `gatewayd.upstream.disable` update the loaded
config file and the in-memory desired config for one profile, then return
`runtimeRestartRequired: true`. `gatewayd.profile.restart` applies the desired
config by restarting that profile's MCP endpoint. `gatewayd.reload` reloads the
config file and hot-replaces profile runtimes after validation. Admin endpoint
host, port, or path changes require restarting the daemon.

Example admin tool arguments:

```json
{
  "profile": "default",
  "upstream": "filesystem"
}
```

Restart argument:

```json
{
  "profile": "default"
}
```

Runtime edit argument:

```json
{
  "profile": "default",
  "upstreamSessionMode": "persistent",
  "persistentSessionPoolSize": 2,
  "prewarmCapabilities": true
}
```

## Boundary

`cxxmcp-gatewayd` may own daemon/product concerns:

- profile endpoint layout;
- config discovery and reload policy;
- admin/control-plane API;
- local service lifecycle;
- operational status and events;
- packaging and install scripts;
- future CLI and GUI workflows.

It must not reimplement MCP protocol machinery or gateway data-plane routing.
Those stay in `cxxmcp` and `cxxmcp-gateway`.

## Local Security Defaults

The daemon examples bind MCP and admin endpoints to `127.0.0.1`. Binding either
endpoint outside loopback is a deployment/security decision and should not be
treated as the default local middleware mode.

Non-loopback binds are rejected unless the config explicitly opts in and
configures bearer auth:

```json
{
  "security": {
    "allowNonLoopback": true,
    "bearerTokens": [
      {"tokenEnv": "CXXMCP_GATEWAYD_TOKEN", "subject": "local-admin"}
    ]
  }
}
```

Bearer auth can be enabled for the admin endpoint and all profile MCP endpoints
with `security.bearerTokens`. Tokens may be inline for local smoke tests or read
from environment variables for normal use:

```json
{
  "security": {
    "bearerTokens": [
      {"tokenEnv": "CXXMCP_GATEWAYD_TOKEN", "subject": "local-admin"}
    ]
  }
}
```

When bearer auth is configured, clients must send
`Authorization: Bearer <token>`. `gatewayd.health` reports only whether bearer
auth is enabled; token values and upstream credentials are not returned by the
admin tools. Changing `security` or the admin endpoint requires restarting the
daemon because the admin auth provider is installed when the endpoint starts.

The same `security` block can install a fixed-window rate limiter on the admin
endpoint and all profile MCP endpoints:

```json
{
  "security": {
    "rateLimit": {
      "requestsPerWindow": 120,
      "windowMs": 60000
    }
  }
}
```

This uses the SDK `RateLimiter` hook. It is intentionally small: the window is
process-wide for the current daemon process and is not a per-user or
distributed quota store.

Profile upstream strings support the `cxxmcp-gateway` `${ENV_NAME}` expansion
rules. Use this for upstream credentials such as HTTP authorization headers or
stdio child-process environment values:

```json
{
  "profiles": [
    {
      "id": "default",
      "upstreams": [
        {
          "id": "remote-tools",
          "enabled": true,
          "transport": "http",
          "uri": "https://example.invalid/mcp",
          "headers": {
            "Authorization": "Bearer ${REMOTE_MCP_TOKEN}"
          }
        }
      ]
    }
  ]
}
```

Missing environment variables fail config validation. Admin tools report
upstream ids, enabled state, transport kind, runtime status, and errors, but do
not return configured headers or child-process environment maps.

## Packaging

The build installs the daemon, sample config, docs, and service templates.
CPack produces a ZIP package on Windows and a TGZ package elsewhere:

```powershell
cmake --build build --target package
```

Service templates live under `packaging/`:

- `packaging/windows/install-service.ps1`
- `packaging/windows/uninstall-service.ps1`
- `packaging/systemd/cxxmcp-gatewayd.service`
- `packaging/launchd/com.cxxmcp.gatewayd.plist`

## Audit Events

`gatewayd.events` exposes an in-memory daemon event stream. The event schema and
current event types are documented in `docs/audit_events.md`.

Data-plane audit and allow/deny enforcement require a real
`cxxmcp-gateway` runtime hook. `gatewayd` does not pretend to enforce tool
policy unless the data-plane library can actually route with that policy.

## Tool Policy

Profiles can use the `cxxmcp-gateway` data-plane tool policy:

```json
{
  "profiles": [
    {
      "id": "default",
      "policy": {
        "allowTools": ["workspace.workspace_scan"],
        "denyTools": ["workspace.workspace_read_file"],
        "allowResources": ["workspace.file:///safe/readme.md"],
        "denyPrompts": ["workspace.secret_prompt"]
      },
      "upstreams": []
    }
  ]
}
```

`denyTools` wins over `allowTools`. When `allowTools` is non-empty, only listed
exposed tool names are visible and callable. Resource and prompt policy uses the
same exact-match shape through `allowResources` / `denyResources` and
`allowPrompts` / `denyPrompts`. Tool policy applies to `tools/list` and
`tools/call`; resource/prompt policy applies to concrete `resources` and
`prompts` operations.
