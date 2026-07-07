#ifndef TURBO_PROMPT_H
#define TURBO_PROMPT_H

#include <platform.h>
#include <turbo_parser.h>

#include "turbo_runtime_data_bind.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  TURBO_PROMPT_OK = 0,
  TURBO_PROMPT_INVALID_ARGUMENT = -1,
  TURBO_PROMPT_OUT_OF_MEMORY = -2
} turbo_prompt_status_t;

/**
 * @brief Return the canonical bind-native schema for one prompt message object.
 * @return Runtime data-bind schema tree owned by caller, or NULL on failure.
 */
CXX_C_API turbo_runtime_data_bind_value_t *turbo_prompt_message_schema_bind(void);

/**
 * @brief Return the canonical bind-native schema for one prompt message array.
 * @return Runtime data-bind schema tree owned by caller, or NULL on failure.
 */
CXX_C_API turbo_runtime_data_bind_value_t *turbo_prompt_messages_schema_bind(void);

/**
 * @brief Return the canonical bind-native schema for one tool-call object.
 * @return Runtime data-bind schema tree owned by caller, or NULL on failure.
 */
CXX_C_API turbo_runtime_data_bind_value_t *turbo_prompt_tool_call_schema_bind(void);

/**
 * @brief Return the canonical bind-native schema for one rich-content part object.
 * @return Runtime data-bind schema tree owned by caller, or NULL on failure.
 */
CXX_C_API turbo_runtime_data_bind_value_t *turbo_prompt_content_part_schema_bind(void);

/**
 * @brief Validate one bind-native prompt message against the canonical shape.
 * @param message Prompt message value.
 * @return `TURBO_PROMPT_OK` on success.
 */
CXX_C_API turbo_prompt_status_t
turbo_prompt_message_validate_bind(const turbo_runtime_data_bind_value_t *message);

/**
 * @brief Convert canonical bind-native messages into OpenAI chat wire JSON.
 * @param messages Canonical message array.
 * @return JSON array owned by caller, or NULL on validation/allocation failure.
 */
CXX_C_API json_value_t *
turbo_prompt_messages_to_openai_chat_json(
    const turbo_runtime_data_bind_value_t *messages);

/**
 * @brief Convert canonical bind-native messages into OpenAI Responses input JSON.
 * @param messages Canonical message array.
 * @return JSON array owned by caller, or NULL on validation/allocation failure.
 */
CXX_C_API json_value_t *
turbo_prompt_messages_to_openai_responses_json(
    const turbo_runtime_data_bind_value_t *messages);

/**
 * @brief Convert canonical bind-native messages into Anthropic wire JSON.
 * @param messages Canonical message array.
 * @param out_messages Converted non-system message array owned by caller.
 * @param out_system Optional concatenated system prompt string owned by caller.
 * @return `TURBO_PROMPT_OK` on success.
 */
CXX_C_API turbo_prompt_status_t
turbo_prompt_messages_to_anthropic_json(
    const turbo_runtime_data_bind_value_t *messages, json_value_t **out_messages,
    char **out_system);

CXX_C_API char *turbo_prompt_render_template(const char *template_text,
                                             const json_value_t *input);

CXX_C_API char *turbo_prompt_render_template_bind(
    const char *template_text, const turbo_runtime_data_bind_value_t *input);

CXX_C_API json_value_t *turbo_prompt_messages_create(void);

CXX_C_API turbo_runtime_data_bind_value_t *turbo_prompt_messages_create_bind(void);

CXX_C_API json_value_t *turbo_prompt_message_create(const char *role, const char *content);

CXX_C_API turbo_runtime_data_bind_value_t *turbo_prompt_message_create_bind(const char *role,
                                                                            const char *content);

CXX_C_API turbo_runtime_data_bind_value_t *turbo_prompt_chat_assistant_message_create_bind(
    const char *content, turbo_runtime_data_bind_value_t *tool_calls);

CXX_C_API turbo_runtime_data_bind_value_t *turbo_prompt_chat_tool_message_create_bind(
    const char *tool_call_id, const char *content);

CXX_C_API turbo_runtime_data_bind_value_t *turbo_prompt_chat_tool_call_create_bind(
    const char *id, const char *type, const char *name, const char *arguments);

CXX_C_API turbo_runtime_data_bind_value_t *turbo_prompt_message_with_content_create_bind(
    const char *role, turbo_runtime_data_bind_value_t *content);

CXX_C_API turbo_runtime_data_bind_value_t *turbo_prompt_content_text_part_create_bind(
    const char *text);

CXX_C_API turbo_runtime_data_bind_value_t *turbo_prompt_tool_use_part_create_bind(
    const char *id, const char *name, turbo_runtime_data_bind_value_t *input);

CXX_C_API turbo_runtime_data_bind_value_t *turbo_prompt_tool_result_part_create_bind(
    const char *tool_use_id, const char *content);

CXX_C_API json_value_t *turbo_prompt_chat_assistant_message_create(const char *content,
                                                                   json_value_t *tool_calls);

CXX_C_API json_value_t *turbo_prompt_chat_tool_message_create(const char *tool_call_id,
                                                              const char *content);

CXX_C_API json_value_t *turbo_prompt_chat_tool_call_create(const char *id, const char *type,
                                                           const char *name,
                                                           const char *arguments);

CXX_C_API json_value_t *turbo_prompt_message_with_content_create(const char *role,
                                                                 json_value_t *content);

CXX_C_API json_value_t *turbo_prompt_content_text_part_create(const char *text);

CXX_C_API json_value_t *turbo_prompt_tool_use_part_create(const char *id, const char *name,
                                                          json_value_t *input);

CXX_C_API json_value_t *turbo_prompt_tool_result_part_create(const char *tool_use_id,
                                                             const char *content);

CXX_C_API turbo_prompt_status_t
turbo_prompt_messages_append(json_value_t *messages, const char *role, const char *content);

CXX_C_API turbo_prompt_status_t
turbo_prompt_messages_append_bind(turbo_runtime_data_bind_value_t *messages, const char *role,
                                  const char *content);

#ifdef __cplusplus
}
#endif

#endif
