#ifndef TURBO_PROVIDER_H
#define TURBO_PROVIDER_H

#include <turbo_agent_api.h>
#include <turbo_event.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Wire format only: no Agent, transport, credentials, or durable state owner. */
typedef enum turbo_provider_format_e {
  TURBO_PROVIDER_UNKNOWN = 0,
  TURBO_PROVIDER_RESPONSES = 1,
  TURBO_PROVIDER_CHAT = 2,
  TURBO_PROVIDER_COMPATIBLE_CHAT = 3,
  TURBO_PROVIDER_ANTHROPIC = 4
} turbo_provider_format_t;

/**
 * One projected request. Initialize to zero before assigning fields.
 * All strings/JSON values are borrowed until build_request returns. input and
 * tools are required arrays already projected into the selected wire format;
 * tools may be empty. Provider neither retains nor changes any input.
 * instructions is Responses instructions or Anthropic system text; Chat embeds
 * system messages in input. previous_response_id applies only to Responses.
 * Structured output is omitted for Anthropic, preserving the existing adapter.
 */
typedef struct turbo_provider_request_s {
  turbo_provider_format_t format;
  const char *model;
  int stream;
  int parallel_tool_calls;
  const json_value_t *input;
  const json_value_t *tools;
  const char *instructions;
  const char *previous_response_id;
  const char *structured_output_name;
  const char *structured_output_schema_json;
  int structured_output_strict;
} turbo_provider_request_t;

/**
 * Encode request fields for the selected format. Returns 0 on success, -1 for
 * invalid input or allocation/serialization failure. *out_json is NULL on
 * failure (when out_json is non-NULL); release success with json_serialize_free.
 * No network I/O occurs. Example:
 *   turbo_provider_request_t r = {0};
 *   r.format = TURBO_PROVIDER_CHAT; r.model = "model";
 *   r.input = messages; r.tools = tools;
 *   if (turbo_provider_build_request(&r, &text) == 0) {
 *     // send text through the caller's transport
 *     json_serialize_free(text);
 *   }
 */
CXX_C_API int turbo_provider_build_request(const turbo_provider_request_t *request,
                                            char **out_json);

/**
 * Convert borrowed canonical messages into an owned wire array, or NULL on
 * invalid format/data or allocation failure. Optional out_system is initialized
 * to NULL and receives owned Anthropic system text; release it with free().
 * Release the returned array with json_free().
 */
CXX_C_API json_value_t *turbo_provider_messages_to_wire_json(
    turbo_provider_format_t format, const json_value_t *messages, char **out_system);

/**
 * Convert a borrowed wire response to an owned canonical model event, or NULL
 * on invalid format/data or allocation failure. Release with json_free().
 * The json_value variant additionally validates the canonical event shape.
 */
CXX_C_API json_value_t *turbo_provider_response_to_event_json(
    turbo_provider_format_t format, const json_value_t *response);
CXX_C_API json_value_t *turbo_provider_response_to_event_json_value(
    turbo_provider_format_t format, const json_value_t *response);

/**
 * Aggregate borrowed SSE bytes into an owned canonical event, or NULL on
 * invalid format/data or allocation failure. Release with json_free().
 * The json_value variant additionally validates the canonical event shape.
 */
CXX_C_API json_value_t *turbo_provider_sse_to_event_json(
    turbo_provider_format_t format, const char *data, size_t size);
CXX_C_API json_value_t *turbo_provider_sse_to_event_json_value(
    turbo_provider_format_t format, const char *data, size_t size);

/**
 * Synchronously emit borrowed events: the callback must clone to retain them.
 * No input/callback/user_data is retained after return. Returns 0 on success,
 * -1 on invalid arguments/format or conversion/allocation failure. SSE delivery
 * is incremental: earlier callbacks are not rolled back on a later failure.
 */
CXX_C_API int turbo_provider_response_emit_json_value(
    turbo_provider_format_t format, const json_value_t *response,
    turbo_event_sink_json_value_fn sink, void *user_data);
CXX_C_API int turbo_provider_sse_emit_json_value(
    turbo_provider_format_t format, const char *data, size_t size,
    turbo_event_sink_json_value_fn sink, void *user_data);

#ifdef __cplusplus
}
#endif
#endif
