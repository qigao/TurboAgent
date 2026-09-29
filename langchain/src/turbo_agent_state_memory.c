#define TURBO_AGENT_INTERNAL_STATE_IMPL_REMAP 1
#include "turbo_agent_core_internal.h"
#include "turbo_agent_hooks_internal.h"
#include "turbo_agent_state_memory_internal.h"

#include <json_parser.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int turbo_agent_state_set_memory_json_impl(json_value_t *state, const char *key,
                                           const char *value_json) {
  json_value_t *memory;
  json_value_t *value = NULL;

  if (!state || !key || !value_json || key[0] == '\0' ||
      json_type(state) != JSON_OBJECT) {
    return -1;
  }

  memory = turbo_agent_state_get_or_create_object(state, "memory");
  if (!memory) {
    return -1;
  }

  value = json_parse(value_json, strlen(value_json));
  if (!value) {
    return -1;
  }

  json_object_add(memory, key, value);
  return 0;
}

const json_value_t *turbo_agent_state_memory_json_impl(const json_value_t *state,
                                                       const char *key) {
  const json_value_t *memory;
  size_t i;

  if (!state || !key || key[0] == '\0' || json_type(state) != JSON_OBJECT) {
    return NULL;
  }

  memory = turbo_agent_state_get_object_const(state, "memory");
  if (!memory || json_type(memory) != JSON_OBJECT) {
    return NULL;
  }

  for (i = json_object_size(memory); i > 0; --i) {
    const char *memory_key = json_object_key(memory, i - 1);
    if (memory_key && strcmp(memory_key, key) == 0) {
      return json_object_value(memory, i - 1);
    }
  }

  return NULL;
}

int turbo_agent_state_load_memory_impl(turbo_agent_t *agent, json_value_t *state,
                                       const char *key) {
  char *value_json = NULL;
  int rc;

  if (!agent || !state || !key || !agent->has_store || !agent->store.get) {
    return -1;
  }

  rc = agent->store.get(agent->store.user_data, key, &value_json);
  if (rc != 0 || !value_json) {
    free(value_json);
    return -1;
  }

  rc = turbo_agent_state_set_memory_json(state, key, value_json);
  if (rc == 0) {
    turbo_agent_emit_trace(agent, state, TURBO_AGENT_TRACE_MEMORY_LOAD, key, NULL, value_json, 0);
  }
  free(value_json);
  return rc;
}

int turbo_agent_state_save_memory_impl(turbo_agent_t *agent, json_value_t *state,
                                       const char *key) {
  const json_value_t *value;
  char *serialized;
  int rc;

  if (!agent || !state || !key || !agent->has_store || !agent->store.put) {
    return -1;
  }

  value = turbo_agent_state_memory_json(state, key);
  if (!value) {
    return agent->store.remove ? agent->store.remove(agent->store.user_data, key) : -1;
  }

  serialized = json_serialize(value, NULL);
  if (!serialized) {
    return -1;
  }

  rc = agent->store.put(agent->store.user_data, key, serialized);
  if (rc == 0) {
    turbo_agent_emit_trace(agent, state, TURBO_AGENT_TRACE_MEMORY_SAVE, key, NULL, serialized, 0);
  }
  json_serialize_free(serialized);
  return rc;
}

int turbo_agent_state_add_memory_context_layer_impl(json_value_t *state, const char *scope,
                                                    const char *path, const char *text) {
  json_value_t *memory_context;
  json_value_t *layers;
  json_value_t *layer;

  if (!state || !scope || scope[0] == '\0' || !text || text[0] == '\0' ||
      json_type(state) != JSON_OBJECT) {
    return -1;
  }

  memory_context = turbo_agent_state_get_or_create_object(state, "memory_context");
  if (!memory_context) {
    return -1;
  }

  layers = json_object_get(memory_context, "layers");
  if (!layers) {
    layers = json_create_array();
    if (!layers) {
      return -1;
    }
    json_object_add(memory_context, "layers", layers);
  }
  if (json_type(layers) != JSON_ARRAY) {
    return -1;
  }

  layer = json_create_object();
  if (!layer) {
    return -1;
  }

  json_object_set_string(layer, "scope", scope);
  json_object_set_string(layer, "path", path && path[0] != '\0' ? path : "");
  json_object_set_string(layer, "text", text);
  json_array_add(layers, layer);
  return 0;
}

const json_value_t *turbo_agent_state_memory_context_impl(const json_value_t *state) {
  return state && json_type(state) == JSON_OBJECT
             ? turbo_agent_state_get_object_const(state, "memory_context")
             : NULL;
}

const json_value_t *turbo_agent_state_memory_layers_impl(const json_value_t *state) {
  const json_value_t *memory_context = turbo_agent_state_memory_context(state);
  const json_value_t *layers;

  if (!memory_context || json_type(memory_context) != JSON_OBJECT) {
    return NULL;
  }

  layers = json_object_get(memory_context, "layers");
  return layers && json_type(layers) == JSON_ARRAY ? layers : NULL;
}

size_t turbo_agent_state_memory_layer_count_impl(const json_value_t *state) {
  const json_value_t *layers = turbo_agent_state_memory_layers(state);
  return layers ? json_array_size(layers) : 0;
}

const json_value_t *turbo_agent_state_memory_layer_at_impl(const json_value_t *state,
                                                           size_t index) {
  const json_value_t *layers = turbo_agent_state_memory_layers(state);

  if (!layers || index >= json_array_size(layers)) {
    return NULL;
  }

  return json_array_get(layers, index);
}

char *turbo_agent_state_memory_context_text_impl(const json_value_t *state) {
  const json_value_t *layers = turbo_agent_state_memory_layers(state);
  size_t i;
  size_t total = 0;
  int found = 0;
  char *buffer;
  size_t offset = 0;
  static const char *prefix = "Persistent memory:\n";

  if (!layers || json_type(layers) != JSON_ARRAY || json_array_size(layers) == 0) {
    return NULL;
  }

  total += strlen(prefix);
  for (i = 0; i < json_array_size(layers); ++i) {
    const json_value_t *layer = json_array_get(layers, i);
    const char *scope;
    const char *path;
    const char *text;

    if (!layer || json_type(layer) != JSON_OBJECT) {
      continue;
    }

    scope = json_get_string(layer, "scope");
    path = json_get_string(layer, "path");
    text = json_get_string(layer, "text");
    if (!scope || !text || text[0] == '\0') {
      continue;
    }

    total += 2 + strlen(scope) + 2;
    if (path && path[0] != '\0') {
      total += 1 + strlen(path);
    }
    total += 1 + strlen(text) + 2;
    found = 1;
  }

  if (!found) {
    return NULL;
  }

  buffer = (char *)malloc(total + 1);
  if (!buffer) {
    return NULL;
  }

  memcpy(buffer + offset, prefix, strlen(prefix));
  offset += strlen(prefix);
  for (i = 0; i < json_array_size(layers); ++i) {
    const json_value_t *layer = json_array_get(layers, i);
    const char *scope;
    const char *path;
    const char *text;
    int written;

    if (!layer || json_type(layer) != JSON_OBJECT) {
      continue;
    }

    scope = json_get_string(layer, "scope");
    path = json_get_string(layer, "path");
    text = json_get_string(layer, "text");
    if (!scope || !text || text[0] == '\0') {
      continue;
    }

    if (path && path[0] != '\0') {
      written =
          snprintf(buffer + offset, total + 1 - offset, "[%s] %s\n%s\n\n", scope, path, text);
    } else {
      written = snprintf(buffer + offset, total + 1 - offset, "[%s]\n%s\n\n", scope, text);
    }
    if (written < 0) {
      free(buffer);
      return NULL;
    }
    offset += (size_t)written;
  }

  buffer[offset] = '\0';
  return buffer;
}
