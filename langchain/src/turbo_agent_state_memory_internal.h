#ifndef TURBO_AGENT_STATE_MEMORY_INTERNAL_H
#define TURBO_AGENT_STATE_MEMORY_INTERNAL_H

#include "turbo_agent_state_core_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

CXX_C_API int turbo_agent_state_set_memory_json_impl(json_value_t *state, const char *key,
                                                     const char *value_json);
CXX_C_API const json_value_t *turbo_agent_state_memory_json_impl(const json_value_t *state,
                                                                 const char *key);
CXX_C_API int turbo_agent_state_load_memory_impl(turbo_agent_t *agent, json_value_t *state,
                                                 const char *key);
CXX_C_API int turbo_agent_state_save_memory_impl(turbo_agent_t *agent, json_value_t *state,
                                                 const char *key);
CXX_C_API int turbo_agent_state_add_memory_context_layer_impl(json_value_t *state,
                                                              const char *scope,
                                                              const char *path,
                                                              const char *text);
CXX_C_API const json_value_t *turbo_agent_state_memory_context_impl(
    const json_value_t *state);
CXX_C_API const json_value_t *turbo_agent_state_memory_layers_impl(
    const json_value_t *state);
CXX_C_API size_t turbo_agent_state_memory_layer_count_impl(const json_value_t *state);
CXX_C_API const json_value_t *turbo_agent_state_memory_layer_at_impl(
    const json_value_t *state, size_t index);
CXX_C_API char *turbo_agent_state_memory_context_text_impl(const json_value_t *state);

#ifdef TURBO_AGENT_INTERNAL_STATE_IMPL_REMAP
#define turbo_agent_state_set_memory_json turbo_agent_state_set_memory_json_impl
#define turbo_agent_state_memory_json turbo_agent_state_memory_json_impl
#define turbo_agent_state_load_memory turbo_agent_state_load_memory_impl
#define turbo_agent_state_save_memory turbo_agent_state_save_memory_impl
#define turbo_agent_state_add_memory_context_layer turbo_agent_state_add_memory_context_layer_impl
#define turbo_agent_state_memory_context turbo_agent_state_memory_context_impl
#define turbo_agent_state_memory_layers turbo_agent_state_memory_layers_impl
#define turbo_agent_state_memory_layer_count turbo_agent_state_memory_layer_count_impl
#define turbo_agent_state_memory_layer_at turbo_agent_state_memory_layer_at_impl
#define turbo_agent_state_memory_context_text turbo_agent_state_memory_context_text_impl
#endif

#ifdef __cplusplus
}
#endif

#endif
