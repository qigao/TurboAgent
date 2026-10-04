#include "turbo_agent_praktor_lowering.h"

#include "turbo_runtime_json.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct lower_buffer_s {
  char *data;
  size_t size;
  size_t capacity;
} lower_buffer_t;

struct turbo_agent_praktor_inline_source_s {
  char source_id[sizeof("turboagent:plan:") + 16u];
  char *yaml;
  size_t yaml_size;
  uint64_t plan_hash;
};

static void lower_diag(
    turbo_agent_praktor_lowering_diagnostic_t *diagnostic,
    turbo_agent_praktor_lowering_status_t status,
    const char *message) {
  if (!diagnostic) return;
  memset(diagnostic, 0, sizeof(*diagnostic));
  diagnostic->status = status;
  if (message) {
    size_t n = strlen(message);
    if (n >= sizeof(diagnostic->message)) {
      n = sizeof(diagnostic->message) - 1u;
    }
    memcpy(diagnostic->message, message, n);
    diagnostic->message[n] = '\0';
  }
}

static turbo_agent_praktor_lowering_status_t lower_fail(
    turbo_agent_praktor_lowering_diagnostic_t *diagnostic,
    turbo_agent_praktor_lowering_status_t status,
    const char *message) {
  lower_diag(diagnostic, status, message);
  return status;
}

static int lower_reserve(lower_buffer_t *buffer, size_t extra) {
  size_t required;
  size_t next;
  char *resized;
  if (!buffer || extra > SIZE_MAX - buffer->size - 1u) return 0;
  required = buffer->size + extra + 1u;
  if (required <= buffer->capacity) return 1;
  next = buffer->capacity ? buffer->capacity : 256u;
  while (next < required) {
    if (next > SIZE_MAX / 2u) {
      next = required;
      break;
    }
    next *= 2u;
  }
  resized = (char *)realloc(buffer->data, next);
  if (!resized) return 0;
  buffer->data = resized;
  buffer->capacity = next;
  return 1;
}

static int lower_append_n(
    lower_buffer_t *buffer, const char *text, size_t size) {
  if (!buffer || (!text && size != 0u) || !lower_reserve(buffer, size)) {
    return 0;
  }
  if (size) memcpy(buffer->data + buffer->size, text, size);
  buffer->size += size;
  buffer->data[buffer->size] = '\0';
  return 1;
}

static int lower_append(lower_buffer_t *buffer, const char *text) {
  return text && lower_append_n(buffer, text, strlen(text));
}

static int lower_key_compare(const void *lhs, const void *rhs) {
  const char *const *a = (const char *const *)lhs;
  const char *const *b = (const char *const *)rhs;
  return strcmp(*a, *b);
}

static json_value_t *lower_canonical_json(const json_value_t *value) {
  json_value_t *result = NULL;
  size_t i;
  if (!value) return NULL;

  if (json_type(value) == JSON_ARRAY) {
    result = json_create_array();
    if (!result) return NULL;
    for (i = 0; i < json_array_size(value); ++i) {
      json_value_t *entry = lower_canonical_json(json_array_get(value, i));
      if (!entry ||
          turbo_runtime_json_array_append(result, entry) !=
              TURBO_RUNTIME_JSON_OK) {
        turbo_runtime_json_destroy(entry);
        turbo_runtime_json_destroy(result);
        return NULL;
      }
    }
    return result;
  }

  if (json_type(value) == JSON_OBJECT) {
    const size_t count = json_object_size(value);
    const char **keys = NULL;
    result = json_create_object();
    if (!result) return NULL;
    if (count == 0u) return result;

    keys = (const char **)calloc(count, sizeof(*keys));
    if (!keys) {
      turbo_runtime_json_destroy(result);
      return NULL;
    }
    for (i = 0; i < count; ++i) {
      keys[i] = json_object_key(value, i);
      if (!keys[i]) {
        free(keys);
        turbo_runtime_json_destroy(result);
        return NULL;
      }
    }
    qsort(keys, count, sizeof(*keys), lower_key_compare);
    for (i = 0; i < count; ++i) {
      json_value_t *entry =
          lower_canonical_json(json_object_get(value, keys[i]));
      if (!entry ||
          turbo_runtime_json_object_set(result, keys[i], entry) !=
              TURBO_RUNTIME_JSON_OK) {
        turbo_runtime_json_destroy(entry);
        free(keys);
        turbo_runtime_json_destroy(result);
        return NULL;
      }
    }
    free(keys);
    return result;
  }

  return json_clone(value);
}

static char *lower_canonical_json_text(
    const json_value_t *value, size_t *out_size) {
  json_value_t *canonical;
  char *text;
  if (out_size) *out_size = 0u;
  canonical = lower_canonical_json(value);
  if (!canonical) return NULL;
  text = json_serialize(canonical, out_size);
  turbo_runtime_json_destroy(canonical);
  return text;
}

static int lower_arguments_are_concrete(const json_value_t *value) {
  size_t i;
  if (!value) return 0;
  switch (json_type(value)) {
    case JSON_STRING: {
      const char *text = json_string(value);
      return text && strstr(text, "{{") == NULL && strstr(text, "}}") == NULL;
    }
    case JSON_ARRAY:
      for (i = 0; i < json_array_size(value); ++i) {
        if (!lower_arguments_are_concrete(json_array_get(value, i))) return 0;
      }
      return 1;
    case JSON_OBJECT:
      for (i = 0; i < json_object_size(value); ++i) {
        const char *key = json_object_key(value, i);
        if (!key || strstr(key, "{{") || strstr(key, "}}") ||
            !lower_arguments_are_concrete(json_object_value(value, i))) {
          return 0;
        }
      }
      return 1;
    default:
      return 1;
  }
}

static int lower_append_json_string(
    lower_buffer_t *buffer, const char *value) {
  json_value_t *string_value;
  char *encoded;
  size_t size = 0u;
  int ok;
  if (!buffer || !value) return 0;
  string_value = json_create_string(value);
  if (!string_value) return 0;
  encoded = json_serialize(string_value, &size);
  turbo_runtime_json_destroy(string_value);
  if (!encoded) return 0;
  ok = lower_append_n(buffer, encoded, size);
  json_serialize_free(encoded);
  return ok;
}

static int lower_expected_step_id(
    turbo_agent_template_kind_t kind,
    size_t index,
    const char **out_id) {
  static const char *const change_ids[] = {"inspect", "change", "verify"};
  static const char *const repair_ids[] = {"diagnose", "change", "verify"};
  if (!out_id || index >= 3u) return 0;
  if (kind == TURBO_AGENT_TEMPLATE_CHANGE) {
    *out_id = change_ids[index];
    return 1;
  }
  if (kind == TURBO_AGENT_TEMPLATE_REPAIR) {
    *out_id = repair_ids[index];
    return 1;
  }
  return 0;
}

static turbo_agent_praktor_lowering_status_t lower_emit_step(
    lower_buffer_t *buffer,
    turbo_agent_template_kind_t kind,
    const json_value_t *step,
    size_t index,
    turbo_agent_praktor_lowering_diagnostic_t *diagnostic) {
  const json_value_t *arguments;
  const json_value_t *dependencies;
  const char *step_id;
  const char *tool;
  const char *expected_id = NULL;
  int retry_limit;
  char *arguments_text = NULL;
  char *dependencies_text = NULL;
  size_t arguments_size = 0u;
  size_t dependencies_size = 0u;

  if (!buffer || !step || json_type(step) != JSON_OBJECT ||
      !lower_expected_step_id(kind, index, &expected_id)) {
    return lower_fail(
        diagnostic, TURBO_AGENT_PRAKTOR_LOWERING_INVALID_PLAN,
        "template DAG step is invalid");
  }

  step_id = json_get_string(step, "step_id");
  tool = json_get_string(step, "tool");
  arguments = json_object_get(step, "arguments");
  dependencies = json_object_get(step, "depends_on");
  retry_limit = json_get_int(step, "retry_limit", -1);

  if (!step_id || strcmp(step_id, expected_id) != 0 ||
      !tool || !tool[0] ||
      !arguments || json_type(arguments) != JSON_OBJECT ||
      !dependencies || json_type(dependencies) != JSON_ARRAY ||
      retry_limit < 0) {
    return lower_fail(
        diagnostic, TURBO_AGENT_PRAKTOR_LOWERING_INVALID_PLAN,
        "template DAG certificate does not match canonical topology");
  }
  if (!lower_arguments_are_concrete(arguments)) {
    return lower_fail(
        diagnostic, TURBO_AGENT_PRAKTOR_LOWERING_INVALID_PLAN,
        "template arguments contain runtime interpolation delimiters");
  }

  arguments_text =
      lower_canonical_json_text(arguments, &arguments_size);
  dependencies_text =
      lower_canonical_json_text(dependencies, &dependencies_size);
  if (!arguments_text || !dependencies_text) {
    if (arguments_text) json_serialize_free(arguments_text);
    if (dependencies_text) json_serialize_free(dependencies_text);
    return lower_fail(
        diagnostic, TURBO_AGENT_PRAKTOR_LOWERING_OUT_OF_MEMORY,
        "could not serialize deterministic inline workflow values");
  }

  if (!lower_append(buffer, "  - name: ") ||
      !lower_append_json_string(buffer, step_id) ||
      !lower_append(buffer, "\n    tool: ") ||
      !lower_append_json_string(buffer, tool) ||
      !lower_append(buffer, "\n")) {
    goto oom;
  }

  if (json_array_size(dependencies) != 0u) {
    if (!lower_append(buffer, "    depends_on: ") ||
        !lower_append_n(
            buffer, dependencies_text, dependencies_size) ||
        !lower_append(buffer, "\n")) {
      goto oom;
    }
  }

  if (retry_limit != 0) {
    char retry_text[32];
    const int retry_size =
        snprintf(retry_text, sizeof(retry_text), "%d", retry_limit);
    if (retry_size <= 0 || (size_t)retry_size >= sizeof(retry_text) ||
        !lower_append(buffer, "    retries:\n      count: ") ||
        !lower_append_n(buffer, retry_text, (size_t)retry_size) ||
        !lower_append(buffer, "\n")) {
      goto oom;
    }
  }

  if (!lower_append(buffer, "    with: ") ||
      !lower_append_n(buffer, arguments_text, arguments_size) ||
      !lower_append(buffer, "\n")) {
    goto oom;
  }

  json_serialize_free(arguments_text);
  json_serialize_free(dependencies_text);
  return TURBO_AGENT_PRAKTOR_LOWERING_OK;

oom:
  json_serialize_free(arguments_text);
  json_serialize_free(dependencies_text);
  return lower_fail(
      diagnostic, TURBO_AGENT_PRAKTOR_LOWERING_OUT_OF_MEMORY,
      "could not build deterministic inline workflow source");
}

turbo_agent_praktor_lowering_status_t
turbo_agent_template_lower_praktor_inline(
    const turbo_agent_template_plan_t *plan,
    turbo_agent_praktor_inline_source_t **out_source,
    turbo_agent_praktor_lowering_diagnostic_t *diagnostic) {
  turbo_agent_template_kind_t kind;
  const turbo_agent_executable_dag_t *dag;
  json_value_t *certificate = NULL;
  const json_value_t *steps;
  turbo_agent_praktor_inline_source_t *source = NULL;
  lower_buffer_t buffer = {0};
  turbo_agent_praktor_lowering_status_t status;
  uint64_t plan_hash;
  size_t i;

  if (out_source) *out_source = NULL;
  lower_diag(diagnostic, TURBO_AGENT_PRAKTOR_LOWERING_OK, "");

  if (!plan || !out_source) {
    return lower_fail(
        diagnostic, TURBO_AGENT_PRAKTOR_LOWERING_INVALID_ARGUMENT,
        "template plan and output are required");
  }

  kind = turbo_agent_template_plan_kind(plan);
  dag = turbo_agent_template_plan_dag(plan);
  plan_hash = turbo_agent_template_plan_hash(plan);
  if ((kind != TURBO_AGENT_TEMPLATE_CHANGE &&
       kind != TURBO_AGENT_TEMPLATE_REPAIR) ||
      !dag || plan_hash == 0u) {
    return lower_fail(
        diagnostic, TURBO_AGENT_PRAKTOR_LOWERING_INVALID_PLAN,
        "only admitted Change/Repair template plans can be lowered");
  }

  certificate = turbo_agent_executable_dag_certificate_json_value(dag);
  steps = certificate ? json_object_get(certificate, "steps") : NULL;
  if (!certificate || !steps || json_type(steps) != JSON_ARRAY ||
      json_array_size(steps) != 3u) {
    turbo_runtime_json_destroy(certificate);
    return lower_fail(
        diagnostic, TURBO_AGENT_PRAKTOR_LOWERING_INVALID_PLAN,
        "template DAG certificate must contain exactly three steps");
  }

  source = (turbo_agent_praktor_inline_source_t *)calloc(1, sizeof(*source));
  if (!source) {
    turbo_runtime_json_destroy(certificate);
    return lower_fail(
        diagnostic, TURBO_AGENT_PRAKTOR_LOWERING_OUT_OF_MEMORY,
        "could not allocate inline workflow artifact");
  }
  source->plan_hash = plan_hash;
  snprintf(
      source->source_id, sizeof(source->source_id),
      "turboagent:plan:%016llx",
      (unsigned long long)plan_hash);

  if (!lower_append(
          &buffer,
          "input_policy: strict\n"
          "outputs:\n"
          "  verify_result:\n"
          "    type: object\n"
          "    required: true\n"
          "    value: \"{{ tasks.verify.outputs.result }}\"\n"
          "tasks:\n")) {
    status = lower_fail(
        diagnostic, TURBO_AGENT_PRAKTOR_LOWERING_OUT_OF_MEMORY,
        "could not initialize inline workflow source");
    goto fail;
  }

  for (i = 0; i < 3u; ++i) {
    status = lower_emit_step(
        &buffer, kind, json_array_get(steps, i), i, diagnostic);
    if (status != TURBO_AGENT_PRAKTOR_LOWERING_OK) goto fail;
  }

  turbo_runtime_json_destroy(certificate);
  source->yaml = buffer.data;
  source->yaml_size = buffer.size;
  *out_source = source;
  lower_diag(diagnostic, TURBO_AGENT_PRAKTOR_LOWERING_OK, "ok");
  return TURBO_AGENT_PRAKTOR_LOWERING_OK;

fail:
  turbo_runtime_json_destroy(certificate);
  free(buffer.data);
  free(source);
  return status;
}

void turbo_agent_praktor_inline_source_destroy(
    turbo_agent_praktor_inline_source_t *source) {
  if (!source) return;
  free(source->yaml);
  memset(source, 0, sizeof(*source));
  free(source);
}

const char *turbo_agent_praktor_inline_source_id(
    const turbo_agent_praktor_inline_source_t *source) {
  return source ? source->source_id : NULL;
}

const char *turbo_agent_praktor_inline_source_yaml(
    const turbo_agent_praktor_inline_source_t *source) {
  return source ? source->yaml : NULL;
}

size_t turbo_agent_praktor_inline_source_yaml_size(
    const turbo_agent_praktor_inline_source_t *source) {
  return source ? source->yaml_size : 0u;
}

uint64_t turbo_agent_praktor_inline_source_plan_hash(
    const turbo_agent_praktor_inline_source_t *source) {
  return source ? source->plan_hash : 0u;
}

turbo_agent_template_outcome_t turbo_agent_template_finish_praktor_result(
    const turbo_agent_template_plan_t *plan,
    const json_value_t *execution_result) {
  const char *workflow_status;
  const json_value_t *outputs;
  const json_value_t *verify_result;
  const json_value_t *verified;

  if (!plan || !execution_result ||
      json_type(execution_result) != JSON_OBJECT) {
    return TURBO_AGENT_TEMPLATE_OUTCOME_INVALID_ARGUMENT;
  }

  workflow_status = json_get_string(execution_result, "workflow_status");
  if (!workflow_status || strcmp(workflow_status, "success") != 0) {
    return turbo_agent_template_finish_verify(
        plan, TURBO_AGENT_VERIFY_EXECUTION_FAILURE);
  }

  outputs = json_object_get(execution_result, "outputs");
  verify_result =
      outputs && json_type(outputs) == JSON_OBJECT
          ? json_object_get(outputs, "verify_result")
          : NULL;
  verified =
      verify_result && json_type(verify_result) == JSON_OBJECT
          ? json_object_get(verify_result, "verified")
          : NULL;
  if (!verified || json_type(verified) != JSON_BOOL) {
    return TURBO_AGENT_TEMPLATE_OUTCOME_VERIFY_FAILED;
  }

  return turbo_agent_template_finish_verify(
      plan,
      json_bool(verified)
          ? TURBO_AGENT_VERIFY_PASSED
          : TURBO_AGENT_VERIFY_SEMANTIC_FAILURE);
}
