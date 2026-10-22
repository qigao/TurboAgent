#ifndef TURBO_DOCUMENT_RENDER_EXAMPLE_H
#define TURBO_DOCUMENT_RENDER_EXAMPLE_H

#include "turbo_tool_registry.h"
#include "turbo_tool_schema.h"

#include <string.h>
#include <tstr.h>

/* Host example only; this header is not installed or part of the public ABI.
 * The catalog and all its strings are borrowed. Keep them immutable and alive
 * until dependent registries, selections, agents and plans are destroyed. */
typedef struct document_render_source_s {
  const char *id;
  const char *revision;
} document_render_source_t;

typedef struct document_render_catalog_s {
  const document_render_source_t *sources;
  size_t count;
} document_render_catalog_t;

enum {
  DOCUMENT_RENDER_MAX_SOURCES = 16,
  DOCUMENT_RENDER_MAX_ID_BYTES = 64,
  DOCUMENT_RENDER_MAX_TITLE_BYTES = 160,
  DOCUMENT_RENDER_MAX_TEXT_BYTES = 2048,
  DOCUMENT_RENDER_MAX_OUTPUT_BYTES = 40000
};

static const char document_render_input_schema[] =
    "{\"type\":\"object\",\"additionalProperties\":false,"
    "\"properties\":{\"title\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":160},"
    "\"passages\":{\"type\":\"array\",\"minItems\":1,\"maxItems\":8,\"items\":{"
    "\"type\":\"object\",\"additionalProperties\":false,\"properties\":{"
    "\"text\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":2048},"
    "\"source_id\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":64}},"
    "\"required\":[\"text\",\"source_id\"]}}},\"required\":[\"title\",\"passages\"]}";

static const char document_render_result_schema[] =
    "{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{"
    "\"markdown\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":40000},"
    "\"verification\":{\"type\":\"string\",\"const\":\"reference_identity_only\"},"
    "\"passage_count\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":8}},"
    "\"required\":[\"markdown\",\"verification\",\"passage_count\"]}";

static int document_render_id_valid(const char *text) {
  size_t i;
  if (!text || !text[0] || strlen(text) > DOCUMENT_RENDER_MAX_ID_BYTES) return 0;
  for (i = 0; text[i]; ++i) {
    unsigned char c = (unsigned char)text[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '-')) return 0;
  }
  return 1;
}

static int document_render_catalog_valid(const document_render_catalog_t *catalog) {
  size_t i, j;
  if (!catalog || !catalog->sources || !catalog->count ||
      catalog->count > DOCUMENT_RENDER_MAX_SOURCES) return 0;
  for (i = 0; i < catalog->count; ++i) {
    if (!document_render_id_valid(catalog->sources[i].id) ||
        !document_render_id_valid(catalog->sources[i].revision)) return 0;
    for (j = 0; j < i; ++j) {
      if (strcmp(catalog->sources[i].id, catalog->sources[j].id) == 0) return 0;
    }
  }
  return 1;
}

static const document_render_source_t *document_render_find_source(
    const document_render_catalog_t *catalog, const char *id) {
  size_t i;
  if (!id) return NULL;
  for (i = 0; i < catalog->count; ++i) {
    if (strcmp(id, catalog->sources[i].id) == 0) return &catalog->sources[i];
  }
  return NULL;
}

static int document_render_text_valid(const json_value_t *value, size_t limit, int multiline) {
  const char *text;
  size_t i;
  if (!value || json_type(value) != JSON_STRING) return 0;
  text = json_string(value);
  if (!text || !text[0] || strlen(text) != json_string_len(value) ||
      strlen(text) > limit) return 0;
  for (i = 0; text[i]; ++i) {
    unsigned char c = (unsigned char)text[i];
    if ((c < 32 && !(multiline && c == '\n')) || c == 127) return 0;
  }
  return 1;
}

static int document_render_append(tstr *text, const char *part, size_t length) {
  tstr grown;
  size_t current = *text ? tstr_len(*text) : 0;
  if (length > DOCUMENT_RENDER_MAX_OUTPUT_BYTES ||
      current > DOCUMENT_RENDER_MAX_OUTPUT_BYTES - length) return -1;
  if (!*text) {
    *text = tstr_new();
    if (!*text) return -1;
  }
  grown = tstr_cat_len(*text, part, length);
  if (!grown) return -1;
  *text = grown;
  return 0;
}

/* Escape draft punctuation; source annotations come from the Host catalog.
 * This does not verify claim support or replace a UI's rendering policy. */
static int document_render_append_text(tstr *output, const char *text) {
  size_t i;
  for (i = 0; text[i]; ++i) {
    unsigned char c = (unsigned char)text[i];
    if (((c >= 33 && c <= 47) || (c >= 58 && c <= 64) ||
         (c >= 91 && c <= 96) || (c >= 123 && c <= 126)) &&
        document_render_append(output, "\\", 1) != 0) return -1;
    if (document_render_append(output, text + i, 1) != 0) return -1;
  }
  return 0;
}

static int document_render_handler(const json_value_t *arguments,
                                    json_value_t **out_result, void *user_data) {
  const document_render_catalog_t *catalog = (const document_render_catalog_t *)user_data;
  turbo_tool_definition_t input = {0};
  const json_value_t *passages;
  const char *title;
  json_value_t *result = NULL;
  tstr markdown = NULL;
  size_t i;
  int status = TURBO_TOOL_INVALID_ARGUMENT;
  if (!out_result) return status;
  *out_result = NULL;
  input.parameters_json = document_render_input_schema;
  input.strict = 1;
  if (!document_render_catalog_valid(catalog) ||
      turbo_tool_schema_validate_arguments_json_value(&input, arguments, NULL, 0) !=
          TURBO_TOOL_SCHEMA_VALID) return status;
  title = json_get_string(arguments, "title");
  passages = json_object_get(arguments, "passages");
  if (!document_render_text_valid(json_object_get(arguments, "title"),
                                  DOCUMENT_RENDER_MAX_TITLE_BYTES, 0)) return status;
  for (i = 0; i < json_array_size(passages); ++i) {
    const json_value_t *passage = json_array_get(passages, i);
    if (!document_render_text_valid(json_object_get(passage, "text"),
                                    DOCUMENT_RENDER_MAX_TEXT_BYTES, 1) ||
        !document_render_text_valid(json_object_get(passage, "source_id"),
                                    DOCUMENT_RENDER_MAX_ID_BYTES, 0) ||
        !document_render_find_source(catalog, json_get_string(passage, "source_id"))) return status;
  }
  status = TURBO_TOOL_OUT_OF_MEMORY;
  if (document_render_append(&markdown, "# ", 2) != 0 ||
      document_render_append_text(&markdown, title) != 0 ||
      document_render_append(&markdown, "\n\n", 2) != 0) goto done;
  for (i = 0; i < json_array_size(passages); ++i) {
    const json_value_t *passage = json_array_get(passages, i);
    const document_render_source_t *source =
        document_render_find_source(catalog, json_get_string(passage, "source_id"));
    if (document_render_append_text(&markdown, json_get_string(passage, "text")) != 0 ||
        document_render_append(&markdown, "\n\nSource: ", 10) != 0 ||
        document_render_append_text(&markdown, source->id) != 0 ||
        document_render_append(&markdown, " @ ", 3) != 0 ||
        document_render_append_text(&markdown, source->revision) != 0 ||
        document_render_append(&markdown, "\n\n", 2) != 0) goto done;
  }
  result = json_create_object();
  if (!result ||
      turbo_runtime_json_object_set_string(result, "markdown", markdown) != TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set_string(result, "verification", "reference_identity_only") !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set_int64(result, "passage_count", (int64_t)json_array_size(passages)) !=
          TURBO_RUNTIME_JSON_OK) goto done;
  *out_result = result;
  result = NULL;
  status = TURBO_TOOL_OK;
done:
  json_free(result);
  tstr_free(markdown);
  return status;
}

static turbo_tool_definition_v6_t document_render_definition(document_render_catalog_t *catalog) {
  static const char *const required[] = {"runtime_tools"};
  turbo_tool_definition_v6_t definition = {0};
  definition.struct_size = sizeof(definition);
  definition.abi_version = TURBO_TOOL_DEFINITION_V6_ABI_VERSION;
  definition.definition.name = "document_render";
  definition.definition.description =
      "Render plain text as Markdown with Host-selected source identities. "
      "Title is at most 160 UTF-8 bytes, each passage at most 2048. "
      "Reference identity checks do not verify factual support.";
  definition.definition.parameters_json = document_render_input_schema;
  definition.definition.strict = 1;
  definition.definition.json_value_handler = document_render_handler;
  definition.definition.user_data = catalog;
  definition.execution_policy.mode = TURBO_TOOL_EXECUTION_SEQUENTIAL;
  definition.execution_policy.idempotency = TURBO_TOOL_IDEMPOTENCY_READ_ONLY;
  definition.required_capabilities = required;
  definition.required_capability_count = 1;
  definition.result_schema_json = document_render_result_schema;
  definition.strict_result = 1;
  definition.effect_flags = TURBO_TOOL_EFFECT_READ;
  return definition;
}

static turbo_tool_status_t document_render_register(turbo_tool_registry_t *registry,
                                                    document_render_catalog_t *catalog) {
  turbo_tool_definition_v6_t definition;
  if (!document_render_catalog_valid(catalog)) return TURBO_TOOL_INVALID_ARGUMENT;
  definition = document_render_definition(catalog);
  return turbo_tool_registry_add_v6(registry, &definition);
}

#endif
