#include <turbo_runtime_json.h>
#include "turbo_mcp_tool_pack.h"

#include "turbo_mcp_protocol_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <turbo_str.h>
#include <turbo_vec.h>

static const char *const turbo_mcp_tool_required_capabilities[] = {"runtime_tools", "network"};

enum {
  TURBO_MCP_DEFAULT_MAX_TOOLS = 128,
  TURBO_MCP_DEFAULT_MAX_PAGES = 16,
  TURBO_MCP_DEFAULT_MAX_REQUEST_BYTES = 1024 * 1024,
  TURBO_MCP_DEFAULT_MAX_RESPONSE_BYTES = 4 * 1024 * 1024,
  TURBO_MCP_DEFAULT_MAX_HEADERS = 64,
  TURBO_MCP_DEFAULT_MAX_HEADER_BYTES = 16 * 1024,
  TURBO_MCP_DEFAULT_TIMEOUT_MS = 10000,
  TURBO_MCP_MAX_SCHEMA_DEPTH = 32,
  TURBO_MCP_MAX_TOOL_NAME_BYTES = 128,
  TURBO_MCP_MAX_SERVER_ID_BYTES = 48
};

#define TURBO_MCP_SAFE_INTEGER_MAX 9007199254740991.0

typedef enum turbo_mcp_header_value_type_e {
  TURBO_MCP_HEADER_STRING = 0,
  TURBO_MCP_HEADER_INTEGER = 1,
  TURBO_MCP_HEADER_BOOLEAN = 2
} turbo_mcp_header_value_type_t;

typedef struct turbo_mcp_header_binding_s {
  tstr_t name;
  turbo_vec_t path;
  turbo_mcp_header_value_type_t type;
} turbo_mcp_header_binding_t;

typedef struct turbo_mcp_tool_binding_s {
  turbo_mcp_tool_pack_t *pack;
  tstr_t remote_name;
  turbo_vec_t headers;
} turbo_mcp_tool_binding_t;

struct turbo_mcp_tool_pack_s {
  turbo_mcp_client_t *client;
  turbo_tool_registry_t *registry;
  tstr_t server_id;
  size_t max_tools;
  size_t max_pages;
  size_t rejected_tool_count;
  turbo_tool_execution_policy_t execution_policy;
};

static void turbo_mcp_header_binding_destroy(turbo_mcp_header_binding_t *binding) {
  size_t index;
  if (!binding) return;
  for (index = 0; index < turbo_vec_size(&binding->path); ++index) {
    tstr_t *segment = (tstr_t *)turbo_vec_at(&binding->path, index);
    if (segment) tstr_free(*segment);
  }
  turbo_vec_destroy(&binding->path);
  tstr_free(binding->name);
  memset(binding, 0, sizeof(*binding));
}

static void turbo_mcp_tool_binding_destroy(void *user_data) {
  turbo_mcp_tool_binding_t *binding = (turbo_mcp_tool_binding_t *)user_data;
  size_t index;
  if (!binding) return;
  for (index = 0; index < turbo_vec_size(&binding->headers); ++index)
    turbo_mcp_header_binding_destroy(
        (turbo_mcp_header_binding_t *)turbo_vec_at(&binding->headers, index));
  turbo_vec_destroy(&binding->headers);
  tstr_free(binding->remote_name);
  free(binding);
}

static int turbo_mcp_execution_policy_valid(const turbo_tool_execution_policy_t *policy) {
  if (!policy || policy->mode < TURBO_TOOL_EXECUTION_SEQUENTIAL ||
      policy->mode > TURBO_TOOL_EXECUTION_EXCLUSIVE ||
      policy->mode == TURBO_TOOL_EXECUTION_PARALLEL_SAFE) {
    return 0;
  }
  return policy->idempotency >= TURBO_TOOL_IDEMPOTENCY_NONE &&
         policy->idempotency <= TURBO_TOOL_IDEMPOTENCY_READ_ONLY;
}

static int turbo_mcp_server_id_valid(const char *server_id) {
  size_t index;
  size_t length;
  if (!server_id || !(length = strlen(server_id)) ||
      length > TURBO_MCP_MAX_SERVER_ID_BYTES) {
    return 0;
  }
  for (index = 0; index < length; ++index) {
    const unsigned char ch = (unsigned char)server_id[index];
    if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
          (ch >= '0' && ch <= '9') || ch == '_' || ch == '-')) {
      return 0;
    }
  }
  return 1;
}

void turbo_mcp_tool_pack_config_init(turbo_mcp_tool_pack_config_t *config) {
  if (!config) return;
  memset(config, 0, sizeof(*config));
  config->struct_size = sizeof(*config);
  config->abi_version = TURBO_MCP_TOOL_PACK_ABI_VERSION;
  config->client_name = "TurboAgent";
  config->client_version = "1.0.0";
  config->timeout_ms = TURBO_MCP_DEFAULT_TIMEOUT_MS;
  config->max_tools = TURBO_MCP_DEFAULT_MAX_TOOLS;
  config->max_pages = TURBO_MCP_DEFAULT_MAX_PAGES;
  config->max_request_bytes = TURBO_MCP_DEFAULT_MAX_REQUEST_BYTES;
  config->max_response_bytes = TURBO_MCP_DEFAULT_MAX_RESPONSE_BYTES;
  config->max_headers = TURBO_MCP_DEFAULT_MAX_HEADERS;
  config->max_header_bytes = TURBO_MCP_DEFAULT_MAX_HEADER_BYTES;
  config->execution_policy.mode = TURBO_TOOL_EXECUTION_EXCLUSIVE;
  config->execution_policy.idempotency = TURBO_TOOL_IDEMPOTENCY_NONE;
}

turbo_mcp_tool_pack_t *
turbo_mcp_tool_pack_create(const turbo_mcp_tool_pack_config_t *config) {
  turbo_mcp_tool_pack_t *pack;
  turbo_mcp_client_config_t client_config;

  if (!config || config->struct_size < sizeof(*config) ||
      config->abi_version != TURBO_MCP_TOOL_PACK_ABI_VERSION ||
      !config->endpoint || !config->endpoint[0] ||
      !turbo_mcp_server_id_valid(config->server_id) || !config->client_name ||
      !config->client_name[0] || !config->client_version || !config->client_version[0] ||
      config->timeout_ms <= 0 || !config->max_tools || !config->max_pages ||
      !config->max_request_bytes || !config->max_response_bytes || !config->max_headers ||
      config->max_headers < 5 || !config->max_header_bytes ||
      !turbo_mcp_execution_policy_valid(&config->execution_policy)) {
    return NULL;
  }
  pack = (turbo_mcp_tool_pack_t *)calloc(1, sizeof(*pack));
  if (!pack) return NULL;
  pack->server_id = tstr_dup(config->server_id);
  pack->registry = turbo_tool_registry_create();
  pack->max_tools = config->max_tools;
  pack->max_pages = config->max_pages;
  pack->execution_policy = config->execution_policy;
  memset(&client_config, 0, sizeof(client_config));
  client_config.endpoint = config->endpoint;
  client_config.client_name = config->client_name;
  client_config.client_version = config->client_version;
  client_config.bearer_token = config->bearer_token;
  client_config.timeout_ms = config->timeout_ms;
  client_config.max_request_bytes = config->max_request_bytes;
  client_config.max_response_bytes = config->max_response_bytes;
  client_config.max_headers = config->max_headers;
  client_config.max_header_bytes = config->max_header_bytes;
  client_config.transport_post = config->transport_post;
  client_config.transport_user_data = config->transport_user_data;
  pack->client = turbo_mcp_client_create(&client_config);
  if (!pack->server_id || !pack->registry || !pack->client) {
    turbo_mcp_tool_pack_destroy(pack);
    return NULL;
  }
  return pack;
}

void turbo_mcp_tool_pack_destroy(turbo_mcp_tool_pack_t *pack) {
  if (!pack) return;
  turbo_tool_registry_destroy(pack->registry);
  turbo_mcp_client_destroy(pack->client);
  tstr_free(pack->server_id);
  free(pack);
}

static int turbo_mcp_tchar(unsigned char ch) {
  return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
         (ch >= '0' && ch <= '9') || ch == '!' || ch == '#' || ch == '$' ||
         ch == '%' || ch == '&' || ch == '\'' || ch == '*' || ch == '+' ||
         ch == '-' || ch == '.' || ch == '^' || ch == '_' || ch == '`' ||
         ch == '|' || ch == '~';
}

static int turbo_mcp_header_name_valid(const char *name) {
  size_t index;
  if (!name || !name[0]) return 0;
  for (index = 0; name[index]; ++index)
    if (!turbo_mcp_tchar((unsigned char)name[index])) return 0;
  return 1;
}

static int turbo_mcp_header_name_unique(const turbo_vec_t *headers, const char *name) {
  size_t index;
  for (index = 0; index < turbo_vec_size(headers); ++index) {
    const turbo_mcp_header_binding_t *binding =
        (const turbo_mcp_header_binding_t *)turbo_vec_at_const(headers, index);
    if (binding && tstr_casecmp(binding->name, name) == 0) return 0;
  }
  return 1;
}

static int turbo_mcp_collect_header(turbo_mcp_tool_binding_t *tool,
                                    const turbo_vec_t *path, const json_value_t *schema) {
  const char *name = json_get_string(schema, "x-mcp-header");
  const char *type = json_get_string(schema, "type");
  turbo_mcp_header_binding_t binding;
  size_t index;
  if (!name || !turbo_mcp_header_name_valid(name) ||
      !turbo_mcp_header_name_unique(&tool->headers, name) || !type || !path ||
      turbo_vec_empty(path)) {
    return 1;
  }
  memset(&binding, 0, sizeof(binding));
  if (!strcmp(type, "string"))
    binding.type = TURBO_MCP_HEADER_STRING;
  else if (!strcmp(type, "integer"))
    binding.type = TURBO_MCP_HEADER_INTEGER;
  else if (!strcmp(type, "boolean"))
    binding.type = TURBO_MCP_HEADER_BOOLEAN;
  else
    return 1;
  binding.name = tstr_dup(name);
  if (!binding.name || turbo_vec_init(&binding.path, sizeof(tstr_t)) != TURBO_OK) {
    turbo_mcp_header_binding_destroy(&binding);
    return -1;
  }
  for (index = 0; index < turbo_vec_size(path); ++index) {
    const char *const *segment = (const char *const *)turbo_vec_at_const(path, index);
    tstr_t copy = segment && *segment ? tstr_dup(*segment) : NULL;
    if (!copy || turbo_vec_push(&binding.path, &copy) != TURBO_OK) {
      tstr_free(copy);
      turbo_mcp_header_binding_destroy(&binding);
      return -1;
    }
  }
  if (turbo_vec_push(&tool->headers, &binding) != TURBO_OK) {
    turbo_mcp_header_binding_destroy(&binding);
    return -1;
  }
  return 0;
}

static int turbo_mcp_scan_schema(const json_value_t *node, int reachable,
                                 turbo_vec_t *path, turbo_mcp_tool_binding_t *tool,
                                 size_t depth) {
  size_t index;
  json_value_t *annotation;
  json_value_t *properties;
  if (!node || depth > TURBO_MCP_MAX_SCHEMA_DEPTH) return depth ? 1 : -1;
  if (json_type(node) == JSON_ARRAY) {
    for (index = 0; index < json_array_size(node); ++index) {
      int status = turbo_mcp_scan_schema(json_array_get(node, index), 0, path, tool,
                                         depth + 1);
      if (status != 0) return status;
    }
    return 0;
  }
  if (json_type(node) != JSON_OBJECT) return 0;

  annotation = json_object_get(node, "x-mcp-header");
  if (annotation) {
    int status;
    if (!reachable || json_type(annotation) != JSON_STRING) return 1;
    status = turbo_mcp_collect_header(tool, path, node);
    if (status != 0) return status;
  }

  properties = json_object_get(node, "properties");
  if (reachable && properties) {
    if (json_type(properties) != JSON_OBJECT) return 1;
    for (index = 0; index < json_object_size(properties); ++index) {
      const char *key = json_object_key(properties, index);
      json_value_t *child = json_object_value(properties, index);
      const char *popped = NULL;
      int status;
      if (!key || turbo_vec_push(path, &key) != TURBO_OK) return -1;
      status = turbo_mcp_scan_schema(child, 1, path, tool, depth + 1);
      (void)turbo_vec_pop(path, &popped);
      if (status != 0) return status;
    }
  }

  for (index = 0; index < json_object_size(node); ++index) {
    const char *key = json_object_key(node, index);
    json_value_t *child = json_object_value(node, index);
    int status;
    if (key && reachable && !strcmp(key, "properties")) continue;
    status = turbo_mcp_scan_schema(child, 0, path, tool, depth + 1);
    if (status != 0) return status;
  }
  return 0;
}

static tstr_t turbo_mcp_local_tool_name(const turbo_mcp_tool_pack_t *pack,
                                        const char *remote_name) {
  tstr_t local;
  size_t index;
  if (!pack || !remote_name || !remote_name[0] || strlen(remote_name) >
                                                   TURBO_MCP_MAX_TOOL_NAME_BYTES) {
    return NULL;
  }
  local = tstr_dup("mcp_");
  if (local) local = tstr_cat_str(local, pack->server_id);
  if (local) local = tstr_cat(local, "_");
  for (index = 0; local && remote_name[index]; ++index) {
    unsigned char ch = (unsigned char)remote_name[index];
    char normalized = ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                       (ch >= '0' && ch <= '9') || ch == '_' || ch == '-')
                          ? (char)ch
                          : '_';
    local = tstr_cat_len(local, &normalized, 1);
  }
  if (!local || tstr_len(local) > TURBO_MCP_MAX_TOOL_NAME_BYTES) {
    tstr_free(local);
    return NULL;
  }
  return local;
}

static const json_value_t *turbo_mcp_argument_at_path(
    const json_value_t *arguments, const turbo_mcp_header_binding_t *header) {
  const json_value_t *current = arguments;
  size_t index;
  for (index = 0; current && index < turbo_vec_size(&header->path); ++index) {
    const tstr_t *segment = (const tstr_t *)turbo_vec_at_const(&header->path, index);
    if (json_type(current) != JSON_OBJECT || !segment) return NULL;
    current = json_object_get(current, *segment);
  }
  return current;
}

static tstr_t turbo_mcp_parameter_header(const turbo_mcp_header_binding_t *header,
                                         const json_value_t *value) {
  tstr_t full_name = NULL;
  tstr_t result = NULL;
  char number[32];
  const char *text = NULL;
  size_t text_length = 0;

  if (!header || !value || json_is_null(value)) return NULL;
  if (header->type == TURBO_MCP_HEADER_STRING &&
      json_type(value) == JSON_STRING) {
    text = json_string(value);
    text_length = json_string_len(value);
  } else if (header->type == TURBO_MCP_HEADER_BOOLEAN &&
             json_type(value) == JSON_BOOL) {
    text = json_bool(value) ? "true" : "false";
    text_length = strlen(text);
  } else if (header->type == TURBO_MCP_HEADER_INTEGER &&
             json_type(value) == JSON_NUMBER) {
    double numeric = json_number(value);
    if (!isfinite(numeric) || trunc(numeric) != numeric ||
        numeric < -TURBO_MCP_SAFE_INTEGER_MAX || numeric > TURBO_MCP_SAFE_INTEGER_MAX) {
      return NULL;
    }
    snprintf(number, sizeof(number), "%.0f", numeric);
    text = number;
    text_length = strlen(number);
  } else {
    return NULL;
  }
  full_name = tstr_dup("Mcp-Param-");
  if (full_name) full_name = tstr_cat_str(full_name, header->name);
  if (full_name) result = turbo_mcp_format_header(full_name, text, text_length);
  tstr_free(full_name);
  return result;
}

static int turbo_mcp_tool_invoke(const json_value_t *arguments,
                                 json_value_t **out_result, void *user_data) {
  turbo_mcp_tool_binding_t *binding = (turbo_mcp_tool_binding_t *)user_data;
  json_value_t *params = NULL;
  json_value_t *arguments_copy = NULL;
  json_value_t *name_value = NULL;
  turbo_vec_t headers;
  size_t index;
  turbo_tool_status_t status;

  if (!binding || !binding->pack || !out_result ||
      (arguments && json_type(arguments) != JSON_OBJECT) ||
      turbo_vec_init(&headers, sizeof(tstr_t)) != TURBO_OK) {
    return -1;
  }
  *out_result = NULL;
  for (index = 0; index < turbo_vec_size(&binding->headers); ++index) {
    const turbo_mcp_header_binding_t *header =
        (const turbo_mcp_header_binding_t *)turbo_vec_at_const(&binding->headers, index);
    const json_value_t *value = turbo_mcp_argument_at_path(arguments, header);
    tstr_t formatted;
    if (!value || json_is_null(value)) continue;
    formatted = turbo_mcp_parameter_header(header, value);
    if (!formatted || turbo_vec_push(&headers, &formatted) != TURBO_OK) {
      tstr_free(formatted);
      turbo_mcp_client_set_error(binding->pack->client,
                                 "tool argument cannot be mirrored to its MCP header");
      status = TURBO_TOOL_INVALID_ARGUMENT;
      goto cleanup;
    }
  }
  params = json_create_object();
  arguments_copy = arguments ? json_clone(arguments) : json_create_object();
  name_value = json_create_string(binding->remote_name);
  if (!params || !arguments_copy || !name_value ||
      !json_object_add_checked(params, "name", name_value)) {
    turbo_runtime_json_reset(&name_value);
    turbo_runtime_json_reset(&params);
    turbo_runtime_json_reset(&arguments_copy);
    status = TURBO_TOOL_OUT_OF_MEMORY;
    goto cleanup;
  }
  name_value = NULL;
  if (!json_object_add_checked(params, "arguments", arguments_copy)) {
    turbo_runtime_json_reset(&params);
    turbo_runtime_json_reset(&arguments_copy);
    status = TURBO_TOOL_OUT_OF_MEMORY;
    goto cleanup;
  }
  arguments_copy = NULL;
  status = turbo_mcp_client_request(
      binding->pack->client, "tools/call", binding->remote_name, params,
      (const char *const *)turbo_vec_data(&headers), turbo_vec_size(&headers), out_result);
  params = NULL;

cleanup:
  turbo_runtime_json_reset(&params);
  turbo_runtime_json_reset(&arguments_copy);
  for (index = 0; index < turbo_vec_size(&headers); ++index) {
    tstr_t *header = (tstr_t *)turbo_vec_at(&headers, index);
    if (header) tstr_free(*header);
  }
  turbo_vec_destroy(&headers);
  return status == TURBO_TOOL_OK ? 0 : -1;
}

static turbo_tool_status_t turbo_mcp_register_remote_tool(
    turbo_mcp_tool_pack_t *pack, turbo_tool_registry_t *registry,
    const json_value_t *remote_tool, size_t *rejected_count) {
  const char *remote_name;
  const char *description;
  json_value_t *schema;
  turbo_mcp_tool_binding_t *binding = NULL;
  turbo_tool_definition_v3_t definition;
  turbo_vec_t path;
  tstr_t local_name = NULL;
  int schema_status;
  turbo_tool_status_t status;

  if (!remote_tool || json_type(remote_tool) != JSON_OBJECT ||
      !(remote_name = json_get_string(remote_tool, "name")) || !remote_name[0] ||
      !(schema = json_object_get(remote_tool, "inputSchema")) ||
      json_type(schema) != JSON_OBJECT) {
    return TURBO_TOOL_ERROR;
  }
  description = json_get_string(remote_tool, "description");
  binding = (turbo_mcp_tool_binding_t *)calloc(1, sizeof(*binding));
  if (!binding || turbo_vec_init(&binding->headers, sizeof(turbo_mcp_header_binding_t)) !=
                      TURBO_OK ||
      turbo_vec_init(&path, sizeof(const char *)) != TURBO_OK) {
    turbo_mcp_tool_binding_destroy(binding);
    return TURBO_TOOL_OUT_OF_MEMORY;
  }
  binding->pack = pack;
  binding->remote_name = tstr_dup(remote_name);
  local_name = turbo_mcp_local_tool_name(pack, remote_name);
  if (!binding->remote_name || !local_name) {
    turbo_vec_destroy(&path);
    turbo_mcp_tool_binding_destroy(binding);
    tstr_free(local_name);
    return TURBO_TOOL_ERROR;
  }
  schema_status = turbo_mcp_scan_schema(schema, 1, &path, binding, 0);
  turbo_vec_destroy(&path);
  if (schema_status > 0) {
    ++*rejected_count;
    turbo_mcp_tool_binding_destroy(binding);
    tstr_free(local_name);
    return TURBO_TOOL_OK;
  }
  if (schema_status < 0) {
    turbo_mcp_tool_binding_destroy(binding);
    tstr_free(local_name);
    return TURBO_TOOL_OUT_OF_MEMORY;
  }
  if (turbo_tool_registry_count(registry) >= pack->max_tools) {
    turbo_mcp_client_set_error(pack->client, "MCP tool catalog exceeds configured limit");
    turbo_mcp_tool_binding_destroy(binding);
    tstr_free(local_name);
    return TURBO_TOOL_BACKPRESSURE;
  }

  memset(&definition, 0, sizeof(definition));
  definition.struct_size = sizeof(definition);
  definition.abi_version = TURBO_TOOL_DEFINITION_V3_ABI_VERSION;
  definition.definition.name = local_name;
  definition.definition.description = description && description[0]
                                          ? description
                                          : "Tool discovered from an MCP server";
  definition.definition.parameters_schema = schema;
  definition.definition.json_value_handler = turbo_mcp_tool_invoke;
  definition.definition.user_data = binding;
  definition.definition.user_data_free = turbo_mcp_tool_binding_destroy;
  definition.execution_policy = pack->execution_policy;
  definition.required_capabilities = turbo_mcp_tool_required_capabilities;
  definition.required_capability_count = sizeof(turbo_mcp_tool_required_capabilities) /
                                         sizeof(turbo_mcp_tool_required_capabilities[0]);
  status = turbo_tool_registry_add_v3(registry, &definition);
  if (status != TURBO_TOOL_OK) turbo_mcp_tool_binding_destroy(binding);
  tstr_free(local_name);
  return status;
}

turbo_tool_status_t turbo_mcp_tool_pack_refresh(turbo_mcp_tool_pack_t *pack) {
  turbo_tool_registry_t *candidate;
  tstr_t cursor = NULL;
  size_t page;
  size_t rejected = 0;
  turbo_tool_status_t status = TURBO_TOOL_ERROR;

  if (!pack) return TURBO_TOOL_INVALID_ARGUMENT;
  candidate = turbo_tool_registry_create();
  if (!candidate) return TURBO_TOOL_OUT_OF_MEMORY;
  for (page = 0; page < pack->max_pages; ++page) {
    json_value_t *params = json_create_object();
    json_value_t *cursor_value = NULL;
    json_value_t *result = NULL;
    json_value_t *tools;
    const char *result_type;
    const char *next_cursor;
    size_t index;
    if (cursor) cursor_value = json_create_string(cursor);
    if (!params || (cursor && (!cursor_value || !json_object_add_checked(
                                                 params, "cursor", cursor_value)))) {
      turbo_runtime_json_reset(&cursor_value);
      turbo_runtime_json_reset(&params);
      status = TURBO_TOOL_OUT_OF_MEMORY;
      goto fail;
    }
    status = turbo_mcp_client_request(pack->client, "tools/list", NULL, params, NULL, 0,
                                      &result);
    if (status != TURBO_TOOL_OK) goto fail;
    if (!result || json_type(result) != JSON_OBJECT ||
        !(result_type = json_get_string(result, "resultType")) ||
        strcmp(result_type, "complete") != 0 ||
        !(tools = json_object_get(result, "tools")) ||
        json_type(tools) != JSON_ARRAY) {
      turbo_runtime_json_reset(&result);
      turbo_mcp_client_set_error(pack->client, "invalid MCP tools/list result");
      status = TURBO_TOOL_ERROR;
      goto fail;
    }
    for (index = 0; index < json_array_size(tools); ++index) {
      status = turbo_mcp_register_remote_tool(pack, candidate,
                                              json_array_get(tools, index), &rejected);
      if (status != TURBO_TOOL_OK) {
        turbo_runtime_json_reset(&result);
        turbo_mcp_client_set_error(pack->client,
                                   status == TURBO_TOOL_DUPLICATE
                                       ? "MCP tool names collide after local namespacing"
                                       : "invalid MCP tool catalog entry");
        goto fail;
      }
    }
    next_cursor = json_get_string(result, "nextCursor");
    if (!next_cursor || !next_cursor[0]) {
      turbo_runtime_json_reset(&result);
      turbo_tool_registry_destroy(pack->registry);
      pack->registry = candidate;
      pack->rejected_tool_count = rejected;
      tstr_free(cursor);
      return TURBO_TOOL_OK;
    }
    if (cursor && strcmp(cursor, next_cursor) == 0) {
      turbo_runtime_json_reset(&result);
      turbo_mcp_client_set_error(pack->client, "MCP tools/list cursor did not advance");
      status = TURBO_TOOL_ERROR;
      goto fail;
    }
    tstr_free(cursor);
    cursor = tstr_dup(next_cursor);
    turbo_runtime_json_reset(&result);
    if (!cursor) {
      status = TURBO_TOOL_OUT_OF_MEMORY;
      goto fail;
    }
  }
  turbo_mcp_client_set_error(pack->client, "MCP tools/list exceeds configured page limit");
  status = TURBO_TOOL_BACKPRESSURE;

fail:
  tstr_free(cursor);
  turbo_tool_registry_destroy(candidate);
  return status;
}

turbo_tool_registry_t *turbo_mcp_tool_pack_registry(turbo_mcp_tool_pack_t *pack) {
  return pack ? pack->registry : NULL;
}

size_t turbo_mcp_tool_pack_tool_count(const turbo_mcp_tool_pack_t *pack) {
  return pack ? turbo_tool_registry_count(pack->registry) : 0;
}

size_t turbo_mcp_tool_pack_rejected_tool_count(const turbo_mcp_tool_pack_t *pack) {
  return pack ? pack->rejected_tool_count : 0;
}

const char *turbo_mcp_tool_pack_last_error(const turbo_mcp_tool_pack_t *pack) {
  return pack ? turbo_mcp_client_last_error(pack->client) : "invalid MCP tool pack";
}
