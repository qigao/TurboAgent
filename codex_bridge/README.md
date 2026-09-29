# TurboAgent Codex Bridge

`TurboAgent::CodexBridge` connects TurboAgent to a separately running Codex App
Server. It is a client/adapter, not a Codex-compatible replacement server.

```text
TurboAgent workflow / policy
          |
          +-- codex.delegate tool
          |
          v
TurboAgent::CodexBridge
          |
          +-- stdio JSONL (built in)
          +-- custom bidirectional transport
          |
          v
codex app-server
```

## Ownership and concurrency

- A client takes ownership of its configured transport only when creation
  succeeds. Destroy closes and releases it.
- The client is single-owner. `request()`, `pump()` and `run_text()` must not
  overlap. Interleaved notifications and server requests are processed while a
  synchronous request waits for its response.
- Transport frames and accumulated agent text have hard byte limits.
- The delegate tool is registered as `EXCLUSIVE` because one client has one
  protocol reader and request-id lane.
- The delegate tool borrows its client. Destroy the registry before the client.

## Design decision

The bridge follows the official [Codex App Server protocol](https://developers.openai.com/codex/app-server/)
as a client. It does not add Codex method names to TurboAgent Harness and does
not expose a second source of TurboAgent session state.

The alternatives considered were:

- Making TurboAgent Harness wire-compatible with Codex. This couples the local
  runtime to a versioned external protocol and still does not provide Codex's
  server-side execution resources.
- Invoking `codex exec` once per tool call. This loses the bidirectional request,
  event and approval channel needed during a turn.
- Using App Server through a narrow transport adapter. This keeps Codex thread
  and turn state owned by Codex while TurboAgent owns workflow, policy and tool
  admission. This is the selected boundary.

Migration is additive: enable `ENABLE_CODEX_BRIDGE`, create one client and
register `codex.delegate` only in policies that grant the `delegate`
capability. Rollback consists of unregistering that tool or disabling the CMake
option; existing Harness and tool APIs are unchanged.

## Approval boundary

Codex command, patch and permission requests are server-initiated protocol
requests. Install `server_request` to connect them to the host's review UI or
TurboAgent policy boundary. Without a handler, command and patch approvals are
declined. Permission elevation and all other requests receive `Method not
found`, because the permission response has no decline variant. The bridge
never silently approves an operation.

## Minimal use

```c
turbo_codex_stdio_transport_config_t stdio_config;
turbo_codex_transport_t transport;
turbo_codex_client_config_t client_config;
turbo_codex_client_t *client;

turbo_codex_stdio_transport_config_init(&stdio_config);
if (turbo_codex_stdio_transport_create(&stdio_config, &transport) != TURBO_OK)
  return 1;

turbo_codex_client_config_init(&client_config);
client_config.transport = transport;
client = turbo_codex_client_create(&client_config);
if (!client) {
  turbo_codex_transport_release(&transport);
  return 1; /* transport ownership did not transfer */
}

if (turbo_codex_client_initialize(client, NULL) != TURBO_OK) {
  turbo_codex_client_destroy(client);
  return 1;
}

/* Use turbo_codex_client_run_text(), or register codex.delegate. */
turbo_codex_client_destroy(client);
```

The stdio adapter launches `codex app-server --stdio` and inherits the current
environment, including Codex login/configuration. Remote WebSocket transport is
left to the host because authentication, TLS and reconnect policy are deployment
concerns and the Codex WebSocket surface remains experimental.
