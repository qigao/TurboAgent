# Provider wire adapters

Link `TurboAgent::Provider` for request encoding and response/SSE conversion
without linking Agent or CHttp. Include `turbo_provider.h` for the
format/request API, `turbo_prompt.h` for canonical messages and templates,
`turbo_agent_sse.h` for the existing SSE aggregation API, and `turbo_embedding.h`
for the existing embedding helpers. The historical header/function names remain
available through Agent's public dependency for source compatibility.

The host selects a wire format, projects input and tool arrays for that format,
and fills a zero-initialized `turbo_provider_request_t`. `turbo_provider_build_request`
borrows every input until return and produces an owned JSON serialization.
Release that serialization with `json_serialize_free`. Inputs remain owned by
the host; no transport, credentials, retries, or durable state are managed here.

```c
turbo_provider_request_t request = {0};
request.format = TURBO_PROVIDER_CHAT;
request.model = "my-model";
request.input = projected_messages; /* borrowed Chat wire array */
request.tools = projected_tools;    /* borrowed Chat tool array, possibly empty */
char *text = NULL;
int rc = turbo_provider_build_request(&request, &text);
if (rc == 0) {
  /* Consume text synchronously or transfer it to an owning transport. */
  json_serialize_free(text);
}
```

Returns `-1` and NULL output on invalid input or allocation/serialization failure.
Response conversion returns owned JSON events, released with `json_free`.
Event emit callbacks receive borrowed events valid only for the callback; clone
them before retaining them. SSE conversion preserves the existing aggregation
and incremental delivery behavior.

The [module decision](../agent/docs/module-separation.md#provider-boundary)
documents Agent projection, custom callback compatibility, and ownership tradeoffs.
`test_turbo_provider`, prompt, embedding, and SSE tests link only this target.
