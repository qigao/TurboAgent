#ifndef TURBO_PROMPT_H
#define TURBO_PROMPT_H

#include <turbo_agent_api.h>
#include <json_parser.h>

#include "turbo_runtime_json.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  TURBO_PROMPT_OK = 0,
  TURBO_PROMPT_INVALID_ARGUMENT = -1,
  TURBO_PROMPT_OUT_OF_MEMORY = -2
} turbo_prompt_status_t;

/**
 * @brief Return the canonical TurboParser JSON-native schema for one prompt message object.
 * @return TurboParser JSON schema tree owned by caller, or NULL on failure.
 */
CXX_C_API json_value_t *turbo_prompt_message_schema_json_value(void);

/**
 * @brief Return the canonical TurboParser JSON-native schema for one prompt message array.
 * @return TurboParser JSON schema tree owned by caller, or NULL on failure.
 */
CXX_C_API json_value_t *turbo_prompt_messages_schema_json_value(void);

/**
 * @brief Return the canonical TurboParser JSON-native schema for one tool-call object.
 * @return TurboParser JSON schema tree owned by caller, or NULL on failure.
 */
CXX_C_API json_value_t *turbo_prompt_tool_call_schema_json_value(void);

/**
 * @brief Return the canonical TurboParser JSON-native schema for one rich-content part object.
 * @return TurboParser JSON schema tree owned by caller, or NULL on failure.
 */
CXX_C_API json_value_t *turbo_prompt_content_part_schema_json_value(void);

/**
 * @brief Validate one TurboParser JSON-native prompt message against the canonical shape.
 * @param message Prompt message value.
 * @return `TURBO_PROMPT_OK` on success.
 */
CXX_C_API turbo_prompt_status_t
turbo_prompt_message_validate_json_value(const json_value_t *message);

/**
 * @brief Convert canonical TurboParser JSON-native messages into OpenAI chat wire JSON.
 * @param messages Canonical message array.
 * @return JSON array owned by caller, or NULL on validation/allocation failure.
 */
CXX_C_API json_value_t *
turbo_prompt_messages_to_openai_chat_json(
    const json_value_t *messages);

/**
 * @brief Convert canonical TurboParser JSON-native messages into OpenAI Responses input JSON.
 * @param messages Canonical message array.
 * @return JSON array owned by caller, or NULL on validation/allocation failure.
 */
CXX_C_API json_value_t *
turbo_prompt_messages_to_openai_responses_json(
    const json_value_t *messages);

/**
 * @brief Convert canonical TurboParser JSON-native messages into Anthropic wire JSON.
 * @param messages Canonical message array.
 * @param out_messages Converted non-system message array owned by caller.
 * @param out_system Optional concatenated system prompt string owned by caller.
 * @return `TURBO_PROMPT_OK` on success.
 */
CXX_C_API turbo_prompt_status_t
turbo_prompt_messages_to_anthropic_json(
    const json_value_t *messages, json_value_t **out_messages,
    char **out_system);

CXX_C_API char *turbo_prompt_render_template(const char *template_text,
                                             const json_value_t *input);

CXX_C_API char *turbo_prompt_render_template_json_value(
    const char *template_text, const json_value_t *input);

CXX_C_API json_value_t *turbo_prompt_messages_create(void);

CXX_C_API json_value_t *turbo_prompt_messages_create_json_value(void);

CXX_C_API json_value_t *turbo_prompt_message_create(const char *role, const char *content);

CXX_C_API json_value_t *turbo_prompt_message_create_json_value(const char *role,
                                                                            const char *content);

CXX_C_API json_value_t *turbo_prompt_chat_assistant_message_create_json_value(
    const char *content, json_value_t *tool_calls);

CXX_C_API json_value_t *turbo_prompt_chat_tool_message_create_json_value(
    const char *tool_call_id, const char *content);

CXX_C_API json_value_t *turbo_prompt_chat_tool_call_create_json_value(
    const char *id, const char *type, const char *name, const char *arguments);

CXX_C_API json_value_t *turbo_prompt_message_with_content_create_json_value(
    const char *role, json_value_t *content);

CXX_C_API json_value_t *turbo_prompt_content_text_part_create_json_value(
    const char *text);

CXX_C_API json_value_t *turbo_prompt_tool_use_part_create_json_value(
    const char *id, const char *name, json_value_t *input);

CXX_C_API json_value_t *turbo_prompt_tool_result_part_create_json_value(
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
turbo_prompt_messages_append_json_value(json_value_t *messages, const char *role,
                                  const char *content);

#ifdef __cplusplus
}
#endif

#endif
