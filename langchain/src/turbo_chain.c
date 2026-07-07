#include "turbo_chain.h"
#include "turbo_agent_util_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *turbo_chain_strdup(const char *src) {
  return (char *)turbo_agent_util_strdup(src);
}

typedef enum {
  TURBO_CHAIN_STEP_CUSTOM = 0,
  TURBO_CHAIN_STEP_PROMPT = 1,
  TURBO_CHAIN_STEP_MODEL = 2,
  TURBO_CHAIN_STEP_TOOL = 3
} turbo_chain_step_kind_t;

typedef struct {
  char *role;
  char *template_text;
} turbo_chain_prompt_step_t;

typedef struct {
  turbo_model_t model;
  const turbo_tool_registry_t *tools;
} turbo_chain_model_step_t;

typedef struct {
  const turbo_tool_registry_t *tools;
} turbo_chain_tool_step_t;

typedef struct {
  char *name;
  turbo_chain_step_kind_t kind;
  turbo_chain_step_fn json_fn;
  turbo_chain_bind_step_fn bind_fn;
  void *user_data;
  turbo_chain_user_data_free_fn user_data_free;
} turbo_chain_step_entry_t;

struct turbo_chain_s {
  char *name;
  turbo_chain_step_entry_t *steps;
  size_t step_count;
  size_t step_capacity;
};



static int turbo_chain_emit_bind_event(turbo_chain_exec_ctx_t *ctx,
                                       turbo_runtime_data_bind_value_t *event) {
  if (!event) {
    return TURBO_CHAIN_OUT_OF_MEMORY;
  }
  if (ctx && ctx->event_sink) {
    ctx->event_sink(event, ctx->event_sink_user_data);
  }
  turbo_runtime_data_bind_value_destroy(event);
  return TURBO_CHAIN_OK;
}

static char *turbo_chain_make_tool_call_id(const turbo_chain_exec_ctx_t *ctx) {
  char buffer[64];
#ifdef _MSC_VER
  _snprintf(buffer, sizeof(buffer), "%s:%u", ctx && ctx->step_name ? ctx->step_name : "tool",
            (unsigned)(ctx ? ctx->step : 0));
#else
  snprintf(buffer, sizeof(buffer), "%s:%u", ctx && ctx->step_name ? ctx->step_name : "tool",
           (unsigned)(ctx ? ctx->step : 0));
#endif
  buffer[sizeof(buffer) - 1] = '\0';
  return turbo_chain_strdup(buffer);
}

static turbo_runtime_data_bind_value_t *turbo_chain_build_model_tool_calls_bind(
    const turbo_chain_exec_ctx_t *ctx, const char *tool_name,
    const turbo_runtime_data_bind_value_t *tool_arguments, const char *tool_arguments_json) {
  turbo_runtime_data_bind_value_t *tool_calls;

  tool_calls = turbo_runtime_data_bind_value_create_array();
  if (!tool_calls) {
    return NULL;
  }

  if (tool_name && tool_name[0] != '\0') {
    turbo_runtime_data_bind_value_t *tool_call = turbo_runtime_data_bind_value_create_object();
    turbo_runtime_data_bind_value_t *call_id_value = NULL;
    turbo_runtime_data_bind_value_t *name_value = NULL;
    turbo_runtime_data_bind_value_t *arguments_value = NULL;
    char *call_id = turbo_chain_make_tool_call_id(ctx);
    char *arguments_json_owned = NULL;

    if (tool_arguments) {
      json_value_t *arguments_json_root = turbo_runtime_data_bind_value_to_json(tool_arguments);
      if (!arguments_json_root) {
        turbo_runtime_data_bind_value_destroy(tool_call);
        turbo_runtime_data_bind_value_destroy(tool_calls);
        tstr_free(call_id);
        return NULL;
      }
      arguments_json_owned = turbo_json_serialize(arguments_json_root, NULL);
      turbo_free_json(&arguments_json_root);
      if (!arguments_json_owned) {
        turbo_runtime_data_bind_value_destroy(tool_call);
        turbo_runtime_data_bind_value_destroy(tool_calls);
        tstr_free(call_id);
        return NULL;
      }
      tool_arguments_json = arguments_json_owned;
    }

    call_id_value = turbo_runtime_data_bind_value_create_string(call_id ? call_id : "");
    name_value = turbo_runtime_data_bind_value_create_string(tool_name);
    arguments_value =
        turbo_runtime_data_bind_value_create_string(tool_arguments_json ? tool_arguments_json : "{}");
    free(arguments_json_owned);
    tstr_free(call_id);
    if (!tool_call || !call_id_value || !name_value || !arguments_value ||
        turbo_runtime_data_bind_object_set(tool_call, "call_id", call_id_value) !=
            TURBO_RUNTIME_DATA_BIND_OK ||
        turbo_runtime_data_bind_object_set(tool_call, "name", name_value) !=
            TURBO_RUNTIME_DATA_BIND_OK ||
        turbo_runtime_data_bind_object_set(tool_call, "arguments", arguments_value) !=
            TURBO_RUNTIME_DATA_BIND_OK ||
        turbo_runtime_data_bind_array_append(tool_calls, tool_call) != TURBO_RUNTIME_DATA_BIND_OK) {
      turbo_runtime_data_bind_value_destroy(tool_call);
      turbo_runtime_data_bind_value_destroy(tool_calls);
      return NULL;
    }
  }

  return tool_calls;
}

static int turbo_chain_emit_model_event_bind(turbo_chain_exec_ctx_t *ctx, const char *output_text,
                                             const char *tool_name,
                                             const turbo_runtime_data_bind_value_t *tool_arguments,
                                             const char *tool_arguments_json) {
  turbo_runtime_data_bind_value_t *tool_calls;
  turbo_runtime_data_bind_value_t *event;

  if (!ctx || !ctx->event_sink) {
    return TURBO_CHAIN_OK;
  }

  tool_calls = turbo_chain_build_model_tool_calls_bind(ctx, tool_name, tool_arguments,
                                                       tool_arguments_json);
  if (!tool_calls) {
    return TURBO_CHAIN_OUT_OF_MEMORY;
  }

  event = turbo_event_model_create_bind("", output_text ? output_text : "", tool_calls);
  turbo_runtime_data_bind_value_destroy(tool_calls);
  return turbo_chain_emit_bind_event(ctx, event);
}

static int turbo_chain_emit_tool_result_event_bind(
    turbo_chain_exec_ctx_t *ctx, const char *name, const char *arguments_json, const char *output,
    const turbo_runtime_data_bind_value_t *output_value, int64_t status) {
  turbo_runtime_data_bind_value_t *event;

  if (!ctx || !ctx->event_sink) {
    return TURBO_CHAIN_OK;
  }

  event = turbo_event_tool_result_create_bind(name, arguments_json, output, output_value, status);
  return turbo_chain_emit_bind_event(ctx, event);
}
static void turbo_chain_free_step(turbo_chain_step_entry_t *step) {
  if (!step) {
    return;
  }

  tstr_free(step->name);
  if (step->user_data_free) {
    step->user_data_free(step->user_data);
  }
}

static turbo_chain_status_t turbo_chain_ensure_capacity(turbo_chain_t *chain) {
  turbo_chain_step_entry_t *resized;
  size_t new_capacity;

  if (chain->step_count < chain->step_capacity) {
    return TURBO_CHAIN_OK;
  }

  new_capacity = chain->step_capacity == 0 ? 4 : chain->step_capacity * 2;
  resized =
      (turbo_chain_step_entry_t *)realloc(chain->steps, new_capacity * sizeof(*chain->steps));
  if (!resized) {
    return TURBO_CHAIN_OUT_OF_MEMORY;
  }

  chain->steps = resized;
  chain->step_capacity = new_capacity;
  return TURBO_CHAIN_OK;
}

static int turbo_chain_find_step(const turbo_chain_t *chain, const char *name) {
  size_t i;

  if (!chain || !name) {
    return -1;
  }

  for (i = 0; i < chain->step_count; ++i) {
    if (chain->steps[i].name && strcmp(chain->steps[i].name, name) == 0) {
      return (int)i;
    }
  }

  return -1;
}

static json_value_t *turbo_chain_state_require_object(json_value_t *state, const char *key) {
  json_value_t *value;

  if (!state || turbo_json_type(state) != TURBO_JSON_OBJECT || !key) {
    return NULL;
  }

  value = turbo_json_object_get(state, key);
  if (value && turbo_json_type(value) == TURBO_JSON_OBJECT) {
    return value;
  }

  if (value) {
    turbo_json_object_set_null(state, key);
  }

  value = turbo_json_create_object();
  if (!value) {
    return NULL;
  }

  turbo_json_object_add(state, key, value);
  return value;
}

static json_value_t *turbo_chain_state_require_array(json_value_t *state, const char *key) {
  json_value_t *value;

  if (!state || turbo_json_type(state) != TURBO_JSON_OBJECT || !key) {
    return NULL;
  }

  value = turbo_json_object_get(state, key);
  if (value && turbo_json_type(value) == TURBO_JSON_ARRAY) {
    return value;
  }

  if (value) {
    turbo_json_object_set_null(state, key);
  }

  value = turbo_json_create_array();
  if (!value) {
    return NULL;
  }

  turbo_json_object_add(state, key, value);
  return value;
}

static turbo_chain_status_t turbo_chain_bind_state_to_json(
    const turbo_runtime_data_bind_value_t *state, json_value_t **out_json) {
  if (!out_json) {
    return TURBO_CHAIN_INVALID_ARGUMENT;
  }

  *out_json = NULL;
  if (!state) {
    *out_json = turbo_chain_state_create();
    return *out_json ? TURBO_CHAIN_OK : TURBO_CHAIN_OUT_OF_MEMORY;
  }

  *out_json = turbo_runtime_data_bind_value_to_json(state);
  if (!*out_json) {
    return TURBO_CHAIN_OUT_OF_MEMORY;
  }

  if (turbo_json_type(*out_json) != TURBO_JSON_OBJECT) {
    turbo_free_json(out_json);
    return TURBO_CHAIN_INVALID_ARGUMENT;
  }

  return TURBO_CHAIN_OK;
}

static turbo_runtime_data_bind_value_t *
turbo_chain_bind_state_require_object(turbo_runtime_data_bind_value_t *state, const char *key) {
  turbo_runtime_data_bind_value_t *value;

  if (!state || !key) {
    return NULL;
  }

  value = turbo_runtime_data_bind_object_get(state, key);
  if (value &&
      turbo_runtime_data_bind_value_kind(value) == TURBO_RUNTIME_DATA_BIND_VALUE_OBJECT) {
    return (turbo_runtime_data_bind_value_t *)value;
  }

  value = turbo_runtime_data_bind_value_create_object();
  if (!value) {
    return NULL;
  }

  if (turbo_runtime_data_bind_object_set(state, key, value) != TURBO_RUNTIME_DATA_BIND_OK) {
    turbo_runtime_data_bind_value_destroy(value);
    return NULL;
  }

  return (turbo_runtime_data_bind_value_t *)turbo_runtime_data_bind_object_get(state, key);
}

static turbo_runtime_data_bind_value_t *
turbo_chain_bind_state_require_array(turbo_runtime_data_bind_value_t *state, const char *key) {
  turbo_runtime_data_bind_value_t *value;

  if (!state || !key) {
    return NULL;
  }

  value = turbo_runtime_data_bind_object_get(state, key);
  if (value &&
      turbo_runtime_data_bind_value_kind(value) == TURBO_RUNTIME_DATA_BIND_VALUE_ARRAY) {
    return (turbo_runtime_data_bind_value_t *)value;
  }

  value = turbo_runtime_data_bind_value_create_array();
  if (!value) {
    return NULL;
  }

  if (turbo_runtime_data_bind_object_set(state, key, value) != TURBO_RUNTIME_DATA_BIND_OK) {
    turbo_runtime_data_bind_value_destroy(value);
    return NULL;
  }

  return (turbo_runtime_data_bind_value_t *)turbo_runtime_data_bind_object_get(state, key);
}

static turbo_runtime_data_bind_value_t *
turbo_chain_bind_state_input(turbo_runtime_data_bind_value_t *state) {
  return turbo_chain_bind_state_require_object(state, "input");
}

static turbo_runtime_data_bind_value_t *
turbo_chain_bind_state_messages(turbo_runtime_data_bind_value_t *state) {
  return turbo_chain_bind_state_require_array(state, "messages");
}

static turbo_runtime_data_bind_value_t *
turbo_chain_bind_state_model_outputs(turbo_runtime_data_bind_value_t *state) {
  return turbo_chain_bind_state_require_array(state, "model_outputs");
}

static turbo_runtime_data_bind_value_t *
turbo_chain_bind_state_tool_requests(turbo_runtime_data_bind_value_t *state) {
  return turbo_chain_bind_state_require_array(state, "tool_requests");
}

static turbo_runtime_data_bind_value_t *
turbo_chain_bind_state_tool_results(turbo_runtime_data_bind_value_t *state) {
  return turbo_chain_bind_state_require_array(state, "tool_results");
}

static turbo_runtime_data_bind_value_t *turbo_chain_bind_parse_json_value(const char *json_text) {
  json_value_t *json_value = NULL;
  turbo_runtime_data_bind_value_t *bind_value;

  if (!json_text) {
    return NULL;
  }

  if (turbo_parse_json((const uint8_t *)json_text, strlen(json_text), &json_value) != 0) {
    return NULL;
  }

  bind_value = turbo_runtime_data_bind_value_from_json(json_value);
  turbo_free_json(&json_value);
  return bind_value;
}

json_value_t *turbo_chain_state_model_outputs(json_value_t *state) {
  return turbo_chain_state_require_array(state, "model_outputs");
}

json_value_t *turbo_chain_state_tool_requests(json_value_t *state) {
  return turbo_chain_state_require_array(state, "tool_requests");
}

json_value_t *turbo_chain_state_tool_results(json_value_t *state) {
  return turbo_chain_state_require_array(state, "tool_results");
}

static turbo_chain_status_t turbo_chain_append_message(json_value_t *state, const char *role,
                                                       const char *content) {
  json_value_t *messages;

  messages = turbo_chain_state_messages(state);
  if (!messages) {
    return TURBO_CHAIN_OUT_OF_MEMORY;
  }

  return turbo_prompt_messages_append(messages, role, content) == TURBO_PROMPT_OK
             ? TURBO_CHAIN_OK
             : TURBO_CHAIN_OUT_OF_MEMORY;
}

static turbo_chain_status_t turbo_chain_append_message_bind(
    turbo_runtime_data_bind_value_t *state, const char *role, const char *content) {
  turbo_runtime_data_bind_value_t *messages;

  messages = turbo_chain_bind_state_messages(state);
  if (!messages) {
    return TURBO_CHAIN_OUT_OF_MEMORY;
  }

  return turbo_prompt_messages_append_bind(messages, role, content) == TURBO_PROMPT_OK
             ? TURBO_CHAIN_OK
             : TURBO_CHAIN_OUT_OF_MEMORY;
}

static turbo_chain_status_t
turbo_chain_store_tool_request_json(json_value_t *state, const char *tool_name,
                                    const char *arguments_json) {
  json_value_t *tool_requests;
  json_value_t *tool_request;

  tool_requests = turbo_chain_state_tool_requests(state);
  if (!tool_requests) {
    return TURBO_CHAIN_OUT_OF_MEMORY;
  }

  tool_request = turbo_json_create_object();
  if (!tool_request) {
    return TURBO_CHAIN_OUT_OF_MEMORY;
  }

  turbo_json_object_set_string(tool_request, "name", tool_name);
  turbo_json_object_set_string(tool_request, "arguments_json",
                               arguments_json ? arguments_json : "{}");
  turbo_json_array_add(tool_requests, tool_request);
  return TURBO_CHAIN_OK;
}

static turbo_chain_status_t
turbo_chain_store_tool_request_bind(json_value_t *state, const char *tool_name,
                                    const turbo_runtime_data_bind_value_t *arguments) {
  json_value_t *tool_requests;
  json_value_t *tool_request;
  json_value_t *arguments_value = NULL;
  char *arguments_json = NULL;

  tool_requests = turbo_chain_state_tool_requests(state);
  if (!tool_requests) {
    return TURBO_CHAIN_OUT_OF_MEMORY;
  }

  tool_request = turbo_json_create_object();
  if (!tool_request) {
    return TURBO_CHAIN_OUT_OF_MEMORY;
  }

  turbo_json_object_set_string(tool_request, "name", tool_name);
  if (arguments) {
    arguments_value = turbo_runtime_data_bind_value_to_json(arguments);
    if (!arguments_value) {
      turbo_free_json(&tool_request);
      return TURBO_CHAIN_OUT_OF_MEMORY;
    }
    arguments_json = turbo_json_serialize(arguments_value, NULL);
    if (!arguments_json) {
      turbo_free_json(&arguments_value);
      turbo_free_json(&tool_request);
      return TURBO_CHAIN_OUT_OF_MEMORY;
    }
    turbo_json_object_add(tool_request, "arguments", arguments_value);
    turbo_json_object_set_string(tool_request, "arguments_json", arguments_json);
    turbo_json_serialize_free(arguments_json);
  } else {
    turbo_json_object_set_string(tool_request, "arguments_json", "{}");
  }

  turbo_json_array_add(tool_requests, tool_request);
  return TURBO_CHAIN_OK;
}

static turbo_chain_status_t
turbo_chain_store_tool_request_bind_state(turbo_runtime_data_bind_value_t *state,
                                          const char *tool_name,
                                          const turbo_runtime_data_bind_value_t *arguments) {
  turbo_runtime_data_bind_value_t *tool_requests;
  turbo_runtime_data_bind_value_t *tool_request;
  turbo_runtime_data_bind_value_t *name_value;
  turbo_runtime_data_bind_value_t *arguments_copy = NULL;
  turbo_runtime_data_bind_value_t *arguments_json_value;
  char *arguments_json = NULL;

  tool_requests = turbo_chain_bind_state_tool_requests(state);
  if (!tool_requests) {
    return TURBO_CHAIN_OUT_OF_MEMORY;
  }

  tool_request = turbo_runtime_data_bind_value_create_object();
  name_value = turbo_runtime_data_bind_value_create_string(tool_name);
  if (!tool_request || !name_value) {
    turbo_runtime_data_bind_value_destroy(tool_request);
    turbo_runtime_data_bind_value_destroy(name_value);
    return TURBO_CHAIN_OUT_OF_MEMORY;
  }

  if (turbo_runtime_data_bind_object_set(tool_request, "name", name_value) !=
      TURBO_RUNTIME_DATA_BIND_OK) {
    turbo_runtime_data_bind_value_destroy(tool_request);
    return TURBO_CHAIN_OUT_OF_MEMORY;
  }

  if (arguments) {
    json_value_t *arguments_json_root = turbo_runtime_data_bind_value_to_json(arguments);

    if (!arguments_json_root) {
      turbo_runtime_data_bind_value_destroy(tool_request);
      return TURBO_CHAIN_OUT_OF_MEMORY;
    }

    arguments_json = turbo_json_serialize(arguments_json_root, NULL);
    turbo_free_json(&arguments_json_root);
    if (!arguments_json) {
      turbo_runtime_data_bind_value_destroy(tool_request);
      return TURBO_CHAIN_OUT_OF_MEMORY;
    }

    arguments_copy = turbo_runtime_data_bind_value_clone(arguments);
    if (!arguments_copy) {
      turbo_json_serialize_free(arguments_json);
      turbo_runtime_data_bind_value_destroy(tool_request);
      return TURBO_CHAIN_OUT_OF_MEMORY;
    }

    if (turbo_runtime_data_bind_object_set(tool_request, "arguments", arguments_copy) !=
        TURBO_RUNTIME_DATA_BIND_OK) {
      turbo_runtime_data_bind_value_destroy(arguments_copy);
      turbo_json_serialize_free(arguments_json);
      turbo_runtime_data_bind_value_destroy(tool_request);
      return TURBO_CHAIN_OUT_OF_MEMORY;
    }

    arguments_json_value = turbo_runtime_data_bind_value_create_string(arguments_json);
    turbo_json_serialize_free(arguments_json);
    if (!arguments_json_value) {
      turbo_runtime_data_bind_value_destroy(tool_request);
      return TURBO_CHAIN_OUT_OF_MEMORY;
    }
    if (turbo_runtime_data_bind_object_set(tool_request, "arguments_json", arguments_json_value) !=
        TURBO_RUNTIME_DATA_BIND_OK) {
      turbo_runtime_data_bind_value_destroy(arguments_json_value);
      turbo_runtime_data_bind_value_destroy(tool_request);
      return TURBO_CHAIN_OUT_OF_MEMORY;
    }
  } else {
    arguments_json_value = turbo_runtime_data_bind_value_create_string("{}");
    if (!arguments_json_value) {
      turbo_runtime_data_bind_value_destroy(tool_request);
      return TURBO_CHAIN_OUT_OF_MEMORY;
    }
    if (turbo_runtime_data_bind_object_set(tool_request, "arguments_json", arguments_json_value) !=
        TURBO_RUNTIME_DATA_BIND_OK) {
      turbo_runtime_data_bind_value_destroy(arguments_json_value);
      turbo_runtime_data_bind_value_destroy(tool_request);
      return TURBO_CHAIN_OUT_OF_MEMORY;
    }
  }

  if (turbo_runtime_data_bind_array_append(tool_requests, tool_request) !=
      TURBO_RUNTIME_DATA_BIND_OK) {
    turbo_runtime_data_bind_value_destroy(tool_request);
    return TURBO_CHAIN_OUT_OF_MEMORY;
  }

  return TURBO_CHAIN_OK;
}

static turbo_chain_status_t
turbo_chain_store_tool_result_json(json_value_t *state, const char *name, const char *arguments_json,
                                   const char *output) {
  json_value_t *tool_results;
  json_value_t *result_entry;

  tool_results = turbo_chain_state_tool_results(state);
  if (!tool_results) {
    return TURBO_CHAIN_OUT_OF_MEMORY;
  }

  result_entry = turbo_json_create_object();
  if (!result_entry) {
    return TURBO_CHAIN_OUT_OF_MEMORY;
  }

  turbo_json_object_set_string(result_entry, "name", name);
  turbo_json_object_set_string(result_entry, "arguments_json",
                               arguments_json ? arguments_json : "{}");
  turbo_json_object_set_string(result_entry, "output", output ? output : "");
  turbo_json_array_add(tool_results, result_entry);
  return TURBO_CHAIN_OK;
}

static turbo_chain_status_t
turbo_chain_store_tool_result_bind(json_value_t *state, const char *name,
                                   const turbo_runtime_data_bind_value_t *arguments,
                                   const turbo_runtime_data_bind_value_t *output_value,
                                   const char *output_text) {
  json_value_t *tool_results;
  json_value_t *result_entry;
  json_value_t *arguments_json_value = NULL;
  json_value_t *output_json_value = NULL;
  char *arguments_json = NULL;

  tool_results = turbo_chain_state_tool_results(state);
  if (!tool_results) {
    return TURBO_CHAIN_OUT_OF_MEMORY;
  }

  result_entry = turbo_json_create_object();
  if (!result_entry) {
    return TURBO_CHAIN_OUT_OF_MEMORY;
  }

  turbo_json_object_set_string(result_entry, "name", name);
  if (arguments) {
    arguments_json_value = turbo_runtime_data_bind_value_to_json(arguments);
    if (!arguments_json_value) {
      turbo_free_json(&result_entry);
      return TURBO_CHAIN_OUT_OF_MEMORY;
    }
    arguments_json = turbo_json_serialize(arguments_json_value, NULL);
    if (!arguments_json) {
      turbo_free_json(&arguments_json_value);
      turbo_free_json(&result_entry);
      return TURBO_CHAIN_OUT_OF_MEMORY;
    }
    turbo_json_object_add(result_entry, "arguments", arguments_json_value);
    turbo_json_object_set_string(result_entry, "arguments_json", arguments_json);
    turbo_json_serialize_free(arguments_json);
  } else {
    turbo_json_object_set_string(result_entry, "arguments_json", "{}");
  }

  turbo_json_object_set_string(result_entry, "output", output_text ? output_text : "");
  if (output_value) {
    output_json_value = turbo_runtime_data_bind_value_to_json(output_value);
    if (!output_json_value) {
      turbo_free_json(&result_entry);
      return TURBO_CHAIN_OUT_OF_MEMORY;
    }
    turbo_json_object_add(result_entry, "output_value", output_json_value);
  }

  turbo_json_array_add(tool_results, result_entry);
  return TURBO_CHAIN_OK;
}

static turbo_chain_status_t
turbo_chain_store_tool_result_bind_state(turbo_runtime_data_bind_value_t *state, const char *name,
                                         const turbo_runtime_data_bind_value_t *arguments,
                                         const turbo_runtime_data_bind_value_t *output_value,
                                         const char *output_text) {
  turbo_runtime_data_bind_value_t *tool_results;
  turbo_runtime_data_bind_value_t *result_entry;
  turbo_runtime_data_bind_value_t *name_value;
  turbo_runtime_data_bind_value_t *output_string;
  turbo_runtime_data_bind_value_t *arguments_copy = NULL;
  turbo_runtime_data_bind_value_t *output_copy = NULL;
  turbo_runtime_data_bind_value_t *arguments_json_value;
  char *arguments_json = NULL;

  tool_results = turbo_chain_bind_state_tool_results(state);
  if (!tool_results) {
    return TURBO_CHAIN_OUT_OF_MEMORY;
  }

  result_entry = turbo_runtime_data_bind_value_create_object();
  name_value = turbo_runtime_data_bind_value_create_string(name);
  output_string = turbo_runtime_data_bind_value_create_string(output_text ? output_text : "");
  if (!result_entry || !name_value || !output_string) {
    turbo_runtime_data_bind_value_destroy(result_entry);
    turbo_runtime_data_bind_value_destroy(name_value);
    turbo_runtime_data_bind_value_destroy(output_string);
    return TURBO_CHAIN_OUT_OF_MEMORY;
  }

  if (turbo_runtime_data_bind_object_set(result_entry, "name", name_value) !=
      TURBO_RUNTIME_DATA_BIND_OK) {
    turbo_runtime_data_bind_value_destroy(result_entry);
    return TURBO_CHAIN_OUT_OF_MEMORY;
  }
  if (turbo_runtime_data_bind_object_set(result_entry, "output", output_string) !=
      TURBO_RUNTIME_DATA_BIND_OK) {
    turbo_runtime_data_bind_value_destroy(result_entry);
    return TURBO_CHAIN_OUT_OF_MEMORY;
  }

  if (arguments) {
    json_value_t *arguments_json_root = turbo_runtime_data_bind_value_to_json(arguments);

    if (!arguments_json_root) {
      turbo_runtime_data_bind_value_destroy(result_entry);
      return TURBO_CHAIN_OUT_OF_MEMORY;
    }

    arguments_json = turbo_json_serialize(arguments_json_root, NULL);
    turbo_free_json(&arguments_json_root);
    if (!arguments_json) {
      turbo_runtime_data_bind_value_destroy(result_entry);
      return TURBO_CHAIN_OUT_OF_MEMORY;
    }

    arguments_copy = turbo_runtime_data_bind_value_clone(arguments);
    if (!arguments_copy) {
      turbo_json_serialize_free(arguments_json);
      turbo_runtime_data_bind_value_destroy(result_entry);
      return TURBO_CHAIN_OUT_OF_MEMORY;
    }

    if (turbo_runtime_data_bind_object_set(result_entry, "arguments", arguments_copy) !=
        TURBO_RUNTIME_DATA_BIND_OK) {
      turbo_runtime_data_bind_value_destroy(arguments_copy);
      turbo_json_serialize_free(arguments_json);
      turbo_runtime_data_bind_value_destroy(result_entry);
      return TURBO_CHAIN_OUT_OF_MEMORY;
    }

    arguments_json_value = turbo_runtime_data_bind_value_create_string(arguments_json);
    turbo_json_serialize_free(arguments_json);
    if (!arguments_json_value) {
      turbo_runtime_data_bind_value_destroy(result_entry);
      return TURBO_CHAIN_OUT_OF_MEMORY;
    }
    if (turbo_runtime_data_bind_object_set(result_entry, "arguments_json", arguments_json_value) !=
        TURBO_RUNTIME_DATA_BIND_OK) {
      turbo_runtime_data_bind_value_destroy(arguments_json_value);
      turbo_runtime_data_bind_value_destroy(result_entry);
      return TURBO_CHAIN_OUT_OF_MEMORY;
    }
  } else {
    arguments_json_value = turbo_runtime_data_bind_value_create_string("{}");
    if (!arguments_json_value) {
      turbo_runtime_data_bind_value_destroy(result_entry);
      return TURBO_CHAIN_OUT_OF_MEMORY;
    }
    if (turbo_runtime_data_bind_object_set(result_entry, "arguments_json", arguments_json_value) !=
        TURBO_RUNTIME_DATA_BIND_OK) {
      turbo_runtime_data_bind_value_destroy(arguments_json_value);
      turbo_runtime_data_bind_value_destroy(result_entry);
      return TURBO_CHAIN_OUT_OF_MEMORY;
    }
  }

  if (output_value) {
    json_value_t *output_json_root = turbo_runtime_data_bind_value_to_json(output_value);

    if (!output_json_root) {
      turbo_runtime_data_bind_value_destroy(result_entry);
      return TURBO_CHAIN_OUT_OF_MEMORY;
    }

    output_copy = turbo_runtime_data_bind_value_clone(output_value);
    turbo_free_json(&output_json_root);
    if (!output_copy) {
      turbo_runtime_data_bind_value_destroy(result_entry);
      return TURBO_CHAIN_OUT_OF_MEMORY;
    }
    if (turbo_runtime_data_bind_object_set(result_entry, "output_value", output_copy) !=
        TURBO_RUNTIME_DATA_BIND_OK) {
      turbo_runtime_data_bind_value_destroy(output_copy);
      turbo_runtime_data_bind_value_destroy(result_entry);
      return TURBO_CHAIN_OUT_OF_MEMORY;
    }
  }

  if (turbo_runtime_data_bind_array_append(tool_results, result_entry) !=
      TURBO_RUNTIME_DATA_BIND_OK) {
    turbo_runtime_data_bind_value_destroy(result_entry);
    return TURBO_CHAIN_OUT_OF_MEMORY;
  }

  return TURBO_CHAIN_OK;
}

static int turbo_chain_prompt_step_run(turbo_chain_exec_ctx_t *ctx, void *user_data) {
  turbo_chain_prompt_step_t *prompt = (turbo_chain_prompt_step_t *)user_data;
  char *rendered;
  turbo_chain_status_t status;

  if (!ctx || (!ctx->state && !ctx->bind_state) || !prompt) {
    return TURBO_CHAIN_INVALID_ARGUMENT;
  }

  if (ctx->bind_state) {
    rendered = turbo_prompt_render_template_bind(prompt->template_text,
                                                 turbo_chain_bind_state_input(ctx->bind_state));
  } else {
    rendered = turbo_prompt_render_template(prompt->template_text,
                                            turbo_chain_state_input(ctx->state));
  }
  if (!rendered) {
    return TURBO_CHAIN_OUT_OF_MEMORY;
  }

  status = ctx->bind_state ? turbo_chain_append_message_bind(ctx->bind_state, prompt->role, rendered)
                           : turbo_chain_append_message(ctx->state, prompt->role, rendered);
  free(rendered);
  return status;
}

static int turbo_chain_model_step_run(turbo_chain_exec_ctx_t *ctx, void *user_data) {
  turbo_chain_model_step_t *step = (turbo_chain_model_step_t *)user_data;
  turbo_model_result_t result = {0};
  turbo_model_bind_result_t bind_result = {0};
  turbo_runtime_data_bind_value_t *messages_bind = NULL;
  turbo_runtime_data_bind_value_t *arguments_bind = NULL;
  const turbo_runtime_data_bind_value_t *event_tool_arguments = NULL;
  json_value_t *model_outputs;
  json_value_t *output_entry;
  int status;

  if (!ctx || (!ctx->state && !ctx->bind_state) || !step ||
      (!step->model.invoke && !step->model.invoke_bind)) {
    return TURBO_CHAIN_INVALID_ARGUMENT;
  }

  if (ctx->bind_state) {
    messages_bind = turbo_chain_bind_state_messages(ctx->bind_state);
    if (!messages_bind) {
      return TURBO_CHAIN_OUT_OF_MEMORY;
    }
  } else if (step->model.invoke_bind) {
    messages_bind = turbo_runtime_data_bind_value_from_json(turbo_chain_state_messages(ctx->state));
    if (!messages_bind) {
      return TURBO_CHAIN_OUT_OF_MEMORY;
    }
  }

  if (step->model.invoke_bind) {
    status = step->model.invoke_bind(step->model.user_data, messages_bind, step->tools,
                                     &bind_result);
    if (!ctx->bind_state) {
      turbo_runtime_data_bind_value_destroy(messages_bind);
    }
    if (status != 0) {
      return TURBO_CHAIN_ERROR;
    }
  } else {
    json_value_t *messages_json;

    if (ctx->bind_state) {
      messages_json = turbo_runtime_data_bind_value_to_json(messages_bind);
      if (!messages_json) {
        return TURBO_CHAIN_OUT_OF_MEMORY;
      }
    } else {
      messages_json = turbo_chain_state_messages(ctx->state);
    }

    status = step->model.invoke(step->model.user_data, messages_json, step->tools, &result);
    if (ctx->bind_state) {
      turbo_free_json(&messages_json);
    }
    if (status != 0) {
      return TURBO_CHAIN_ERROR;
    }
  }

  if ((step->model.invoke_bind ? bind_result.output_text : result.output_text)) {
    const char *output_text =
        step->model.invoke_bind ? bind_result.output_text : result.output_text;

    if (ctx->bind_state) {
      turbo_runtime_data_bind_value_t *model_outputs_bind =
          turbo_chain_bind_state_model_outputs(ctx->bind_state);
      turbo_runtime_data_bind_value_t *output_entry_bind = turbo_runtime_data_bind_value_create_object();
      turbo_runtime_data_bind_value_t *text_value = turbo_runtime_data_bind_value_create_string(output_text);

      if (turbo_chain_append_message_bind(ctx->bind_state, "assistant", output_text) != TURBO_CHAIN_OK ||
          !model_outputs_bind || !output_entry_bind || !text_value) {
        turbo_runtime_data_bind_value_destroy(output_entry_bind);
        turbo_runtime_data_bind_value_destroy(text_value);
        return TURBO_CHAIN_OUT_OF_MEMORY;
      }

      if (turbo_runtime_data_bind_object_set(output_entry_bind, "text", text_value) !=
          TURBO_RUNTIME_DATA_BIND_OK) {
        turbo_runtime_data_bind_value_destroy(output_entry_bind);
        return TURBO_CHAIN_OUT_OF_MEMORY;
      }
      if (turbo_runtime_data_bind_array_append(model_outputs_bind, output_entry_bind) !=
          TURBO_RUNTIME_DATA_BIND_OK) {
        turbo_runtime_data_bind_value_destroy(output_entry_bind);
        return TURBO_CHAIN_OUT_OF_MEMORY;
      }
    } else {
      if (turbo_chain_append_message(ctx->state, "assistant", output_text) != TURBO_CHAIN_OK) {
        return TURBO_CHAIN_OUT_OF_MEMORY;
      }

      model_outputs = turbo_chain_state_model_outputs(ctx->state);
      if (!model_outputs) {
        return TURBO_CHAIN_OUT_OF_MEMORY;
      }

      output_entry = turbo_json_create_object();
      if (!output_entry) {
        return TURBO_CHAIN_OUT_OF_MEMORY;
      }

      turbo_json_object_set_string(output_entry, "text", output_text);
      turbo_json_array_add(model_outputs, output_entry);
    }
  }

  if (step->model.invoke_bind ? bind_result.tool_name != NULL : result.tool_name != NULL) {
    const char *tool_name = step->model.invoke_bind ? bind_result.tool_name : result.tool_name;
    turbo_chain_status_t chain_status;

    if (step->model.invoke_bind) {
      event_tool_arguments = bind_result.tool_arguments;
      chain_status =
          ctx->bind_state ? turbo_chain_store_tool_request_bind_state(ctx->bind_state, tool_name,
                                                                      bind_result.tool_arguments)
                          : turbo_chain_store_tool_request_bind(ctx->state, tool_name,
                                                                bind_result.tool_arguments);
    } else {
      if (ctx->bind_state) {
        if (result.tool_arguments_json) {
          arguments_bind = turbo_chain_bind_parse_json_value(result.tool_arguments_json);
          if (!arguments_bind) {
            return TURBO_CHAIN_ERROR;
          }
        }
        event_tool_arguments = arguments_bind;
        chain_status =
            turbo_chain_store_tool_request_bind_state(ctx->bind_state, tool_name, arguments_bind);
      } else {
        chain_status =
            turbo_chain_store_tool_request_json(ctx->state, tool_name, result.tool_arguments_json);
      }
    }
    if (chain_status != TURBO_CHAIN_OK) {
      turbo_runtime_data_bind_value_destroy(arguments_bind);
      arguments_bind = NULL;
      turbo_runtime_data_bind_value_destroy(bind_result.tool_arguments);
      bind_result.tool_arguments = NULL;
      return chain_status;
    }
  }

  if ((step->model.invoke_bind ? bind_result.output_text != NULL : result.output_text != NULL) ||
      (step->model.invoke_bind ? bind_result.tool_name != NULL : result.tool_name != NULL)) {
    status = step->model.invoke_bind
                 ? turbo_chain_emit_model_event_bind(ctx, bind_result.output_text,
                                                     bind_result.tool_name, event_tool_arguments,
                                                     NULL)
                 : turbo_chain_emit_model_event_bind(ctx, result.output_text, result.tool_name,
                                                     event_tool_arguments,
                                                     result.tool_arguments_json);
    if (status != TURBO_CHAIN_OK) {
      turbo_runtime_data_bind_value_destroy(arguments_bind);
      turbo_runtime_data_bind_value_destroy(bind_result.tool_arguments);
      return status;
    }
  }

  turbo_runtime_data_bind_value_destroy(arguments_bind);
  turbo_runtime_data_bind_value_destroy(bind_result.tool_arguments);

  return TURBO_CHAIN_OK;
}

static int turbo_chain_tool_step_run(turbo_chain_exec_ctx_t *ctx, void *user_data) {
  turbo_chain_tool_step_t *step = (turbo_chain_tool_step_t *)user_data;
  json_value_t *tool_requests;
  json_value_t *tool_results;
  json_value_t *tool_request;
  const json_value_t *arguments_value;
  turbo_runtime_data_bind_value_t *arguments_bind = NULL;
  turbo_runtime_data_bind_value_t *output_bind = NULL;
  const char *name;
  const char *arguments_json;
  char *output = NULL;
  turbo_tool_status_t tool_status;
  turbo_chain_status_t chain_status;
  size_t request_index;

  if (!ctx || (!ctx->state && !ctx->bind_state) || !step || !step->tools) {
    return TURBO_CHAIN_INVALID_ARGUMENT;
  }

  if (ctx->bind_state) {
    turbo_runtime_data_bind_value_t *tool_requests_bind =
        turbo_chain_bind_state_tool_requests(ctx->bind_state);
    turbo_runtime_data_bind_value_t *tool_results_bind =
        turbo_chain_bind_state_tool_results(ctx->bind_state);
    const turbo_runtime_data_bind_value_t *tool_request_bind;
    const turbo_runtime_data_bind_value_t *arguments_value_bind;
    json_value_t *output_json = NULL;

    if (!tool_requests_bind || !tool_results_bind) {
      return TURBO_CHAIN_OUT_OF_MEMORY;
    }

    request_index = turbo_runtime_data_bind_value_size(tool_results_bind);
    if (request_index >= turbo_runtime_data_bind_value_size(tool_requests_bind)) {
      return TURBO_CHAIN_OK;
    }

    tool_request_bind = turbo_runtime_data_bind_array_get(tool_requests_bind, request_index);
    if (!tool_request_bind ||
        turbo_runtime_data_bind_value_kind(tool_request_bind) !=
            TURBO_RUNTIME_DATA_BIND_VALUE_OBJECT) {
      return TURBO_CHAIN_INVALID_ARGUMENT;
    }

    name = turbo_runtime_data_bind_value_as_string(
        turbo_runtime_data_bind_object_get(tool_request_bind, "name"));
    arguments_json = turbo_runtime_data_bind_value_as_string(
        turbo_runtime_data_bind_object_get(tool_request_bind, "arguments_json"));
    arguments_value_bind = turbo_runtime_data_bind_object_get(tool_request_bind, "arguments");
    if (!name) {
      return TURBO_CHAIN_INVALID_ARGUMENT;
    }

    if (arguments_value_bind) {
      tool_status =
          turbo_tool_registry_execute_bind(step->tools, name, arguments_value_bind, &output_bind);
      if (tool_status == TURBO_TOOL_OK) {
        output_json = turbo_runtime_data_bind_value_to_json(output_bind);
        if (!output_json) {
          turbo_runtime_data_bind_value_destroy(output_bind);
          return TURBO_CHAIN_OUT_OF_MEMORY;
        }
        output = turbo_json_serialize(output_json, NULL);
        turbo_free_json(&output_json);
        if (!output) {
          turbo_runtime_data_bind_value_destroy(output_bind);
          return TURBO_CHAIN_OUT_OF_MEMORY;
        }

        chain_status = turbo_chain_append_message_bind(ctx->bind_state, "tool", output);
        if (chain_status != TURBO_CHAIN_OK) {
          turbo_runtime_data_bind_value_destroy(output_bind);
          free(output);
          return chain_status;
        }

        chain_status = turbo_chain_store_tool_result_bind_state(
            ctx->bind_state, name, arguments_value_bind, output_bind, output);
        if (chain_status == TURBO_CHAIN_OK) {
          chain_status = turbo_chain_emit_tool_result_event_bind(
              ctx, name, arguments_json ? arguments_json : "{}", output, output_bind, tool_status);
        }
        turbo_runtime_data_bind_value_destroy(output_bind);
        free(output);
        return chain_status;
      }
    }

    tool_status =
        turbo_tool_registry_execute(step->tools, name, arguments_json ? arguments_json : "{}", &output);
    if (tool_status != TURBO_TOOL_OK) {
      return TURBO_CHAIN_ERROR;
    }

    chain_status = turbo_chain_append_message_bind(ctx->bind_state, "tool", output ? output : "");
    if (chain_status != TURBO_CHAIN_OK) {
      free(output);
      return chain_status;
    }

    chain_status = turbo_chain_store_tool_result_bind_state(ctx->bind_state, name, arguments_value_bind,
                                                            NULL, output ? output : "");
    if (chain_status == TURBO_CHAIN_OK) {
      chain_status = turbo_chain_emit_tool_result_event_bind(
          ctx, name, arguments_json ? arguments_json : "{}", output ? output : "", NULL, tool_status);
    }
    free(output);
    return chain_status;
  }

  tool_requests = turbo_chain_state_tool_requests(ctx->state);
  tool_results = turbo_chain_state_tool_results(ctx->state);
  if (!tool_requests || !tool_results) {
    return TURBO_CHAIN_OUT_OF_MEMORY;
  }

  request_index = turbo_json_array_size(tool_results);
  if (request_index >= turbo_json_array_size(tool_requests)) {
    return TURBO_CHAIN_OK;
  }

  tool_request = turbo_json_array_get(tool_requests, request_index);
  if (!tool_request || turbo_json_type(tool_request) != TURBO_JSON_OBJECT) {
    return TURBO_CHAIN_INVALID_ARGUMENT;
  }

  name = turbo_json_get_string(tool_request, "name");
  arguments_json = turbo_json_get_string(tool_request, "arguments_json");
  arguments_value = turbo_json_object_get(tool_request, "arguments");
  if (!name) {
    return TURBO_CHAIN_INVALID_ARGUMENT;
  }

  if (arguments_value) {
    arguments_bind = turbo_runtime_data_bind_value_from_json(arguments_value);
    if (!arguments_bind) {
      return TURBO_CHAIN_OUT_OF_MEMORY;
    }

    tool_status = turbo_tool_registry_execute_bind(step->tools, name, arguments_bind, &output_bind);
    if (tool_status != TURBO_TOOL_OK) {
      turbo_runtime_data_bind_value_destroy(arguments_bind);
      return TURBO_CHAIN_ERROR;
    }

    {
      json_value_t *output_json = turbo_runtime_data_bind_value_to_json(output_bind);
      if (!output_json) {
        turbo_runtime_data_bind_value_destroy(output_bind);
        return TURBO_CHAIN_OUT_OF_MEMORY;
      }
      output = turbo_json_serialize(output_json, NULL);
      turbo_free_json(&output_json);
      if (!output) {
        turbo_runtime_data_bind_value_destroy(output_bind);
        return TURBO_CHAIN_OUT_OF_MEMORY;
      }
    }

    if (turbo_chain_append_message(ctx->state, "tool", output) != TURBO_CHAIN_OK) {
      turbo_runtime_data_bind_value_destroy(arguments_bind);
      turbo_runtime_data_bind_value_destroy(output_bind);
      free(output);
      return TURBO_CHAIN_OUT_OF_MEMORY;
    }

    chain_status =
        turbo_chain_store_tool_result_bind(ctx->state, name, arguments_bind, output_bind, output);
    if (chain_status == TURBO_CHAIN_OK) {
      chain_status = turbo_chain_emit_tool_result_event_bind(
          ctx, name, arguments_json ? arguments_json : "{}", output, output_bind, tool_status);
    }
    turbo_runtime_data_bind_value_destroy(arguments_bind);
    turbo_runtime_data_bind_value_destroy(output_bind);
    free(output);
    return chain_status;
  }

  tool_status =
      turbo_tool_registry_execute(step->tools, name, arguments_json ? arguments_json : "{}",
                                  &output);
  if (tool_status != TURBO_TOOL_OK) {
    return TURBO_CHAIN_ERROR;
  }

  if (turbo_chain_append_message(ctx->state, "tool", output ? output : "") != TURBO_CHAIN_OK) {
    free(output);
    return TURBO_CHAIN_OUT_OF_MEMORY;
  }

  chain_status =
      turbo_chain_store_tool_result_json(ctx->state, name, arguments_json, output ? output : "");
  if (chain_status == TURBO_CHAIN_OK) {
    chain_status = turbo_chain_emit_tool_result_event_bind(
        ctx, name, arguments_json ? arguments_json : "{}", output ? output : "", NULL, tool_status);
  }
  free(output);
  return chain_status;
}

static void turbo_chain_prompt_step_free(void *user_data) {
  turbo_chain_prompt_step_t *prompt = (turbo_chain_prompt_step_t *)user_data;

  if (!prompt) {
    return;
  }

  tstr_free(prompt->role);
  tstr_free(prompt->template_text);
  free(prompt);
}

static void turbo_chain_model_step_free(void *user_data) {
  turbo_chain_model_step_t *step = (turbo_chain_model_step_t *)user_data;

  if (!step) {
    return;
  }
  free(step);
}

turbo_chain_t *turbo_chain_create(const char *name) {
  turbo_chain_t *chain = (turbo_chain_t *)calloc(1, sizeof(*chain));

  if (!chain) {
    return NULL;
  }

  if (name) {
    chain->name = turbo_chain_strdup(name);
    if (!chain->name) {
      free(chain);
      return NULL;
    }
  }

  return chain;
}

void turbo_chain_destroy(turbo_chain_t *chain) {
  size_t i;

  if (!chain) {
    return;
  }

  tstr_free(chain->name);
  for (i = 0; i < chain->step_count; ++i) {
    turbo_chain_free_step(&chain->steps[i]);
  }
  free(chain->steps);
  free(chain);
}

turbo_chain_status_t turbo_chain_add_step(turbo_chain_t *chain, const char *name,
                                          turbo_chain_step_fn fn, void *user_data,
                                          turbo_chain_user_data_free_fn user_data_free) {
  turbo_chain_step_entry_t *step;
  turbo_chain_status_t status;

  if (!chain || !name || !fn) {
    return TURBO_CHAIN_INVALID_ARGUMENT;
  }

  if (turbo_chain_find_step(chain, name) >= 0) {
    return TURBO_CHAIN_DUPLICATE_STEP;
  }

  status = turbo_chain_ensure_capacity(chain);
  if (status != TURBO_CHAIN_OK) {
    return status;
  }

  step = &chain->steps[chain->step_count];
  memset(step, 0, sizeof(*step));
  step->name = turbo_chain_strdup(name);
  if (!step->name) {
    return TURBO_CHAIN_OUT_OF_MEMORY;
  }

  step->kind = TURBO_CHAIN_STEP_CUSTOM;
  step->json_fn = fn;
  step->bind_fn = NULL;
  step->user_data = user_data;
  step->user_data_free = user_data_free;
  chain->step_count++;
  return TURBO_CHAIN_OK;
}

turbo_chain_status_t
turbo_chain_add_bind_step(turbo_chain_t *chain, const char *name, turbo_chain_bind_step_fn fn,
                          void *user_data, turbo_chain_user_data_free_fn user_data_free) {
  turbo_chain_step_entry_t *step;
  turbo_chain_status_t status;

  if (!chain || !name || !fn) {
    return TURBO_CHAIN_INVALID_ARGUMENT;
  }

  if (turbo_chain_find_step(chain, name) >= 0) {
    return TURBO_CHAIN_DUPLICATE_STEP;
  }

  status = turbo_chain_ensure_capacity(chain);
  if (status != TURBO_CHAIN_OK) {
    return status;
  }

  step = &chain->steps[chain->step_count];
  memset(step, 0, sizeof(*step));
  step->name = turbo_chain_strdup(name);
  if (!step->name) {
    return TURBO_CHAIN_OUT_OF_MEMORY;
  }

  step->kind = TURBO_CHAIN_STEP_CUSTOM;
  step->json_fn = NULL;
  step->bind_fn = fn;
  step->user_data = user_data;
  step->user_data_free = user_data_free;
  chain->step_count++;
  return TURBO_CHAIN_OK;
}

turbo_chain_status_t turbo_chain_add_prompt_step(turbo_chain_t *chain, const char *name,
                                                 const char *role,
                                                 const char *template_text) {
  turbo_chain_prompt_step_t *prompt;
  turbo_chain_status_t status;

  if (!chain || !name || !role || !template_text) {
    return TURBO_CHAIN_INVALID_ARGUMENT;
  }

  prompt = (turbo_chain_prompt_step_t *)calloc(1, sizeof(*prompt));
  if (!prompt) {
    return TURBO_CHAIN_OUT_OF_MEMORY;
  }

  prompt->role = turbo_chain_strdup(role);
  prompt->template_text = turbo_chain_strdup(template_text);
  if (!prompt->role || !prompt->template_text) {
    turbo_chain_prompt_step_free(prompt);
    return TURBO_CHAIN_OUT_OF_MEMORY;
  }

  status = turbo_chain_add_step(chain, name, turbo_chain_prompt_step_run, prompt,
                                turbo_chain_prompt_step_free);
  if (status != TURBO_CHAIN_OK) {
    turbo_chain_prompt_step_free(prompt);
    return status;
  }

  chain->steps[chain->step_count - 1].kind = TURBO_CHAIN_STEP_PROMPT;
  chain->steps[chain->step_count - 1].bind_fn = turbo_chain_prompt_step_run;
  return TURBO_CHAIN_OK;
}

turbo_chain_status_t turbo_chain_add_model_step(turbo_chain_t *chain, const char *name,
                                                const turbo_model_t *model,
                                                const turbo_tool_registry_t *tools) {
  turbo_chain_model_step_t *step;
  turbo_chain_status_t status;

  if (!chain || !name || !model || (!model->invoke && !model->invoke_bind)) {
    return TURBO_CHAIN_INVALID_ARGUMENT;
  }

  step = (turbo_chain_model_step_t *)calloc(1, sizeof(*step));
  if (!step) {
    return TURBO_CHAIN_OUT_OF_MEMORY;
  }

  step->model = *model;
  step->tools = tools;

  status = turbo_chain_add_step(chain, name, turbo_chain_model_step_run, step,
                                turbo_chain_model_step_free);
  if (status != TURBO_CHAIN_OK) {
    turbo_chain_model_step_free(step);
    return status;
  }

  chain->steps[chain->step_count - 1].kind = TURBO_CHAIN_STEP_MODEL;
  chain->steps[chain->step_count - 1].bind_fn = turbo_chain_model_step_run;
  return TURBO_CHAIN_OK;
}

turbo_chain_status_t turbo_chain_add_tool_step(turbo_chain_t *chain, const char *name,
                                               const turbo_tool_registry_t *tools) {
  turbo_chain_tool_step_t *step;
  turbo_chain_status_t status;

  if (!chain || !name || !tools) {
    return TURBO_CHAIN_INVALID_ARGUMENT;
  }

  step = (turbo_chain_tool_step_t *)calloc(1, sizeof(*step));
  if (!step) {
    return TURBO_CHAIN_OUT_OF_MEMORY;
  }

  step->tools = tools;
  status =
      turbo_chain_add_step(chain, name, turbo_chain_tool_step_run, step, free);
  if (status != TURBO_CHAIN_OK) {
    free(step);
    return status;
  }

  chain->steps[chain->step_count - 1].kind = TURBO_CHAIN_STEP_TOOL;
  chain->steps[chain->step_count - 1].bind_fn = turbo_chain_tool_step_run;
  return TURBO_CHAIN_OK;
}

turbo_chain_status_t turbo_chain_run(turbo_chain_t *chain, json_value_t *state) {
  size_t i;

  if (!chain || !state || turbo_json_type(state) != TURBO_JSON_OBJECT) {
    return TURBO_CHAIN_INVALID_ARGUMENT;
  }

  for (i = 0; i < chain->step_count; ++i) {
    turbo_chain_exec_ctx_t ctx;
    int status;

    memset(&ctx, 0, sizeof(ctx));
    ctx.chain = chain;
    ctx.state = state;
    ctx.step = i;
    ctx.step_name = chain->steps[i].name;

    if (!chain->steps[i].json_fn) {
      return TURBO_CHAIN_INVALID_ARGUMENT;
    }

    status = chain->steps[i].json_fn(&ctx, chain->steps[i].user_data);
    if (status == TURBO_CHAIN_STOP || ctx.stop) {
      return TURBO_CHAIN_STOP;
    }
    if (status != TURBO_CHAIN_OK) {
      return (turbo_chain_status_t)status;
    }
  }

  return TURBO_CHAIN_OK;
}

turbo_chain_status_t
turbo_chain_run_bind(turbo_chain_t *chain, const turbo_runtime_data_bind_value_t *state,
                     turbo_runtime_data_bind_value_t **out_state) {
  return turbo_chain_run_bind_stream(chain, state, NULL, NULL, out_state);
}

turbo_chain_status_t turbo_chain_run_bind_stream(
    turbo_chain_t *chain, const turbo_runtime_data_bind_value_t *state,
    turbo_event_sink_bind_fn event_sink, void *event_sink_user_data,
    turbo_runtime_data_bind_value_t **out_state) {
  turbo_runtime_data_bind_value_t *bound_state = NULL;
  turbo_chain_exec_ctx_t ctx;
  size_t i;
  turbo_chain_status_t status;

  if (!chain || !out_state) {
    return TURBO_CHAIN_INVALID_ARGUMENT;
  }

  *out_state = NULL;

  if (!state) {
    bound_state = turbo_chain_state_create_bind();
  } else {
    if (turbo_runtime_data_bind_value_kind(state) != TURBO_RUNTIME_DATA_BIND_VALUE_OBJECT) {
      return TURBO_CHAIN_INVALID_ARGUMENT;
    }
    bound_state = turbo_runtime_data_bind_value_clone(state);
  }
  if (!bound_state) {
    return TURBO_CHAIN_OUT_OF_MEMORY;
  }

  for (i = 0; i < chain->step_count; ++i) {
    memset(&ctx, 0, sizeof(ctx));
    ctx.chain = chain;
    ctx.bind_state = bound_state;
    ctx.step = i;
    ctx.step_name = chain->steps[i].name;
    ctx.event_sink = event_sink;
    ctx.event_sink_user_data = event_sink_user_data;

    if (!chain->steps[i].bind_fn) {
      turbo_runtime_data_bind_value_destroy(bound_state);
      return TURBO_CHAIN_INVALID_ARGUMENT;
    }

    status = chain->steps[i].bind_fn(&ctx, chain->steps[i].user_data);
    if (status == TURBO_CHAIN_STOP || ctx.stop) {
      *out_state = bound_state;
      return TURBO_CHAIN_STOP;
    }
    if (status != TURBO_CHAIN_OK) {
      turbo_runtime_data_bind_value_destroy(bound_state);
      return status;
    }
  }

  *out_state = bound_state;
  return TURBO_CHAIN_OK;
}

turbo_chain_status_t
turbo_chain_run_bind_log(turbo_chain_t *chain, const turbo_runtime_data_bind_value_t *state,
                         turbo_event_log_t *log, turbo_runtime_data_bind_value_t **out_state) {
  turbo_chain_status_t status;

  if (!log) {
    return TURBO_CHAIN_INVALID_ARGUMENT;
  }

  status = (turbo_chain_status_t)turbo_event_log_reset(log);
  if (status != TURBO_CHAIN_OK) {
    return status == TURBO_CHAIN_INVALID_ARGUMENT ? TURBO_CHAIN_INVALID_ARGUMENT
                                                  : TURBO_CHAIN_ERROR;
  }

  status =
      turbo_chain_run_bind_stream(chain, state, turbo_event_log_capture_bind, log, out_state);
  if (turbo_event_log_status(log) != TURBO_EVENT_LOG_OK) {
    if (out_state && *out_state) {
      turbo_runtime_data_bind_value_destroy(*out_state);
      *out_state = NULL;
    }
    return TURBO_CHAIN_OUT_OF_MEMORY;
  }

  return status;
}

void turbo_chain_ctx_stop(turbo_chain_exec_ctx_t *ctx) {
  if (ctx) {
    ctx->stop = 1;
  }
}

json_value_t *turbo_chain_state_create(void) {
  json_value_t *state = turbo_json_create_object();
  json_value_t *input;
  json_value_t *messages;
  json_value_t *tool_results;
  json_value_t *model_outputs;
  json_value_t *tool_requests;

  if (!state) {
    return NULL;
  }

  input = turbo_json_create_object();
  messages = turbo_prompt_messages_create();
  tool_results = turbo_json_create_array();
  model_outputs = turbo_json_create_array();
  tool_requests = turbo_json_create_array();
  if (!input || !messages || !tool_results || !model_outputs || !tool_requests) {
    turbo_free_json(&input);
    turbo_free_json(&messages);
    turbo_free_json(&tool_results);
    turbo_free_json(&model_outputs);
    turbo_free_json(&tool_requests);
    turbo_free_json(&state);
    return NULL;
  }

  turbo_json_object_add(state, "input", input);
  turbo_json_object_add(state, "messages", messages);
  turbo_json_object_add(state, "tool_results", tool_results);
  turbo_json_object_add(state, "model_outputs", model_outputs);
  turbo_json_object_add(state, "tool_requests", tool_requests);
  return state;
}

turbo_runtime_data_bind_value_t *turbo_chain_state_create_bind(void) {
  turbo_runtime_data_bind_value_t *state = turbo_runtime_data_bind_value_create_object();
  turbo_runtime_data_bind_value_t *input = turbo_runtime_data_bind_value_create_object();
  turbo_runtime_data_bind_value_t *messages = turbo_runtime_data_bind_value_create_array();
  turbo_runtime_data_bind_value_t *tool_results = turbo_runtime_data_bind_value_create_array();
  turbo_runtime_data_bind_value_t *model_outputs = turbo_runtime_data_bind_value_create_array();
  turbo_runtime_data_bind_value_t *tool_requests = turbo_runtime_data_bind_value_create_array();

  if (!state || !input || !messages || !tool_results || !model_outputs || !tool_requests) {
    turbo_runtime_data_bind_value_destroy(input);
    turbo_runtime_data_bind_value_destroy(messages);
    turbo_runtime_data_bind_value_destroy(tool_results);
    turbo_runtime_data_bind_value_destroy(model_outputs);
    turbo_runtime_data_bind_value_destroy(tool_requests);
    turbo_runtime_data_bind_value_destroy(state);
    return NULL;
  }

  if (turbo_runtime_data_bind_object_set(state, "input", input) != TURBO_RUNTIME_DATA_BIND_OK ||
      turbo_runtime_data_bind_object_set(state, "messages", messages) != TURBO_RUNTIME_DATA_BIND_OK ||
      turbo_runtime_data_bind_object_set(state, "tool_results", tool_results) != TURBO_RUNTIME_DATA_BIND_OK ||
      turbo_runtime_data_bind_object_set(state, "model_outputs", model_outputs) != TURBO_RUNTIME_DATA_BIND_OK ||
      turbo_runtime_data_bind_object_set(state, "tool_requests", tool_requests) !=
          TURBO_RUNTIME_DATA_BIND_OK) {
    turbo_runtime_data_bind_value_destroy(state);
    return NULL;
  }

  return state;
}

json_value_t *turbo_chain_state_input(json_value_t *state) {
  return turbo_chain_state_require_object(state, "input");
}

turbo_chain_status_t turbo_chain_state_input_set_string(json_value_t *state, const char *key,
                                                        const char *value) {
  json_value_t *input;

  if (!key || !value) {
    return TURBO_CHAIN_INVALID_ARGUMENT;
  }

  input = turbo_chain_state_input(state);
  if (!input) {
    return TURBO_CHAIN_OUT_OF_MEMORY;
  }

  turbo_json_object_set_string(input, key, value);
  return TURBO_CHAIN_OK;
}

json_value_t *turbo_chain_state_messages(json_value_t *state) {
  return turbo_chain_state_require_array(state, "messages");
}

const char *turbo_chain_state_last_output_text(const json_value_t *state) {
  json_value_t *outputs;
  json_value_t *last;

  if (!state || turbo_json_type(state) != TURBO_JSON_OBJECT) {
    return NULL;
  }

  outputs = turbo_json_object_get(state, "model_outputs");
  if (!outputs || turbo_json_type(outputs) != TURBO_JSON_ARRAY ||
      turbo_json_array_size(outputs) == 0) {
    return NULL;
  }

  last = turbo_json_array_get(outputs, turbo_json_array_size(outputs) - 1);
  return last && turbo_json_type(last) == TURBO_JSON_OBJECT ? turbo_json_get_string(last, "text")
                                                            : NULL;
}

const char *turbo_chain_state_pending_tool_name(const json_value_t *state) {
  json_value_t *tool_requests;
  json_value_t *tool_results;
  json_value_t *pending;
  size_t request_index;

  if (!state || turbo_json_type(state) != TURBO_JSON_OBJECT) {
    return NULL;
  }

  tool_requests = turbo_json_object_get(state, "tool_requests");
  tool_results = turbo_json_object_get(state, "tool_results");
  if (!tool_requests || turbo_json_type(tool_requests) != TURBO_JSON_ARRAY || !tool_results ||
      turbo_json_type(tool_results) != TURBO_JSON_ARRAY) {
    return NULL;
  }

  request_index = turbo_json_array_size(tool_results);
  if (request_index >= turbo_json_array_size(tool_requests)) {
    return NULL;
  }

  pending = turbo_json_array_get(tool_requests, request_index);
  return pending && turbo_json_type(pending) == TURBO_JSON_OBJECT
             ? turbo_json_get_string(pending, "name")
             : NULL;
}
