# Audit And Event Schema

`cxxmcp-gatewayd` exposes local daemon events through the `gatewayd.events`
admin tool and the `diagnostics` CLI command.

The current event stream is an in-memory ring buffer. It is intended for local
diagnostics and lightweight audit visibility, not as a durable audit warehouse.

## Event Object

```json
{
  "id": 1,
  "unixMs": 1780000000000,
  "type": "daemon.started",
  "detail": {}
}
```

Fields:

- `id`: monotonically increasing daemon-local event id;
- `unixMs`: Unix timestamp in milliseconds;
- `type`: stable event type string;
- `detail`: event-specific JSON object.

## Current Event Types

- `daemon.started`
- `config.reloaded`
- `config.reload.failed`
- `profile.restarted`
- `profile.restart.failed`
- `upstream.enabled`
- `upstream.disabled`
- `upstream.enable.failed`
- `upstream.disable.failed`

## Boundary

These events cover daemon and admin/control-plane operations. Data-plane audit
for downstream MCP `tools/list`, `tools/call`, resources, prompts, completions,
and policy decisions requires an explicit `cxxmcp-gateway` runtime hook. It
should not be simulated in `gatewayd` without actual routing enforcement.
