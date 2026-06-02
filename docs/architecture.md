# Architecture

`cxxmcp-gatewayd` is a daemon around the `cxxmcp-gateway` runtime.

```text
apps / IDEs / agents
  -> MCP data-plane endpoint
  -> gatewayd profile runtime
  -> cxxmcp-gateway runtime
  -> upstream MCP servers
```

Admin/control-plane traffic is separate:

```text
CLI / GUI / local operator
  -> admin endpoint
  -> gatewayd state
  -> validated runtime actions
```

## Repository Boundary

`cxxmcp-gateway` owns:

- gateway config model;
- namespace rules;
- catalog aggregation;
- request routing;
- gateway-level error mapping;
- hosted gateway runtime.

`cxxmcp-gatewayd` owns:

- daemon process lifecycle;
- profile-to-endpoint layout;
- admin API surface;
- config reload policy;
- local service and packaging behavior;
- future user-facing CLI and GUI workflows.

The dependency direction is:

```text
gatewayd CLI / GUI / admin API -> cxxmcp-gateway runtime -> cxxmcp-gateway core -> cxxmcp SDK
```

## Endpoint Split

The MCP data-plane endpoint and admin endpoint must stay separate. The MCP
endpoint only handles MCP client traffic. The admin endpoint reports state and
performs management actions.

The MVP uses an admin MCP endpoint because it is the fastest way to validate
the shape with existing SDK primitives. A production admin API can later use
HTTP routes, events, or another local control-plane protocol.
