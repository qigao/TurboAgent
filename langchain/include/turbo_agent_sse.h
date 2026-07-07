#ifndef TURBO_AGENT_SSE_H
#define TURBO_AGENT_SSE_H

#include <platform.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Aggregate Chat Completions SSE frames into one JSON response object string.
 * @param sse_data Raw SSE payload bytes.
 * @param sse_len Byte length of sse_data.
 * @param out_response_json Allocated JSON string on success; free with turbo_json_serialize_free().
 * @return 0 on success, non-zero on parse/aggregation failure.
 */
CXX_C_API int turbo_agent_chat_sse_to_json(const char *sse_data, size_t sse_len,
                                            char **out_response_json);

/**
 * @brief Extract the completed `response` object from Responses API SSE frames.
 * @param sse_data Raw SSE payload bytes.
 * @param sse_len Byte length of sse_data.
 * @param out_response_json Allocated JSON string on success; free with turbo_json_serialize_free().
 * @return 0 on success, non-zero on parse/aggregation failure.
 */
CXX_C_API int turbo_agent_responses_sse_to_json(const char *sse_data, size_t sse_len,
                                                 char **out_response_json);

/**
 * @brief Aggregate Anthropic Messages SSE frames into one JSON response object string.
 * @param sse_data Raw SSE payload bytes.
 * @param sse_len Byte length of sse_data.
 * @param out_response_json Allocated JSON string on success; free with turbo_json_serialize_free().
 * @return 0 on success, non-zero on parse/aggregation failure.
 */
CXX_C_API int turbo_agent_anthropic_messages_sse_to_json(const char *sse_data, size_t sse_len,
                                                          char **out_response_json);

#ifdef __cplusplus
}
#endif

#endif


