#include "turbo_agent_context_internal.h"

#include "turbo_agent_event_internal.h"
#include "turbo_agent_request_common_internal.h"
#include "turbo_agent_runtime_internal.h"
#include "turbo_agent_runtime_v1_internal.h"
#include "turbo_agent_util_internal.h"

#include <openssl/sha.h>
#include <salts_uuid.h>

#include <stdlib.h>
#include <string.h>

#define TURBO_AGENT_COMPACTIONS_COLLECTION "agent_compactions"
#define TURBO_AGENT_CONTEXT_HEADS_COLLECTION "agent_context_heads"
#define TURBO_AGENT_CONTEXT_SCHEMA_VERSION 1U
#define TURBO_AGENT_CONTEXT_MAX_EXACT_JSON_INTEGER 9007199254740991ULL

struct turbo_agent_context_s {
  turbo_agent_runtime_t *runtime;
  turbo_agent_t *agent;
  char *thread_id;
  turbo_agent_context_policy_t policy;
  json_value_t *summary;
  json_value_t *head;
  size_t event_start;
};

static char *turbo_agent_context_source_hash(const json_value_t *source);

static int turbo_agent_context_policy_valid(const turbo_agent_context_policy_t *policy) {
  uint64_t input_capacity;

  if (!policy || policy->struct_size < sizeof(*policy) ||
      policy->abi_version != TURBO_AGENT_CONTEXT_ABI_VERSION ||
      policy->context_window_tokens == 0 ||
      policy->reserve_output_tokens >= policy->context_window_tokens ||
      policy->compact_trigger_tokens == 0 || policy->retain_recent_tokens == 0 ||
      policy->max_summary_input_tokens == 0 || policy->max_summary_tokens == 0 ||
      policy->max_compactions_per_turn == 0 || !policy->estimate_tokens || !policy->summarize) {
    return 0;
  }
  input_capacity = policy->context_window_tokens - policy->reserve_output_tokens;
  return policy->compact_trigger_tokens <= input_capacity &&
         policy->retain_recent_tokens < policy->compact_trigger_tokens &&
         policy->max_summary_tokens < input_capacity;
}

static int turbo_agent_context_exact_size(const json_value_t *object, const char *key,
                                          size_t *out_value) {
  double value;

  if (!object || !key || !out_value) return SALTS_EINVAL;
  value = json_get_double(object, key, -1.0);
  if (value < 0.0 || value > (double)TURBO_AGENT_CONTEXT_MAX_EXACT_JSON_INTEGER ||
      value != (double)(size_t)value) {
    return SALTS_EPROTO;
  }
  *out_value = (size_t)value;
  return SALTS_OK;
}

static int turbo_agent_context_summary_valid(const json_value_t *summary) {
  double version;

  if (!summary || json_type(summary) != JSON_OBJECT) return 0;
  version = json_get_double(summary, "schema_version", -1.0);
  return version == (double)TURBO_AGENT_CONTEXT_SCHEMA_VERSION;
}

static void turbo_agent_context_clear_projection(turbo_agent_context_t *context) {
  if (!context || !context->agent) return;
  turbo_runtime_json_destroy(context->agent->context_summary);
  context->agent->context_summary = NULL;
  context->agent->context_event_start = 0;
  context->agent->context_projection_active = 0;
}

static int turbo_agent_context_apply_projection(turbo_agent_context_t *context) {
  json_value_t *summary;

  if (!context || !context->agent) return SALTS_EINVAL;
  if (!context->summary) {
    turbo_agent_context_clear_projection(context);
    return SALTS_OK;
  }
  summary = json_clone(context->summary);
  if (!summary) return SALTS_ENOMEM;
  turbo_runtime_json_destroy(context->agent->context_summary);
  context->agent->context_summary = summary;
  context->agent->context_event_start = context->event_start;
  context->agent->context_projection_active = 1;
  return SALTS_OK;
}

static int turbo_agent_context_load_head(turbo_agent_context_t *context) {
  json_value_t *heads = NULL;
  json_value_t *compaction = NULL;
  const json_value_t *head;
  const json_value_t *summary;
  const json_value_t *source;
  const char *compaction_id;
  const char *head_hash;
  const char *compaction_hash;
  const char *thread_id;
  size_t event_start;
  size_t compaction_end;
  char *computed_hash = NULL;
  int rc = SALTS_OK;

  if (turbo_agent_runtime_store_list_json(context->runtime, TURBO_AGENT_CONTEXT_HEADS_COLLECTION,
                                          "thread_id", context->thread_id, &heads) != 0) {
    return SALTS_EIO;
  }
  if (json_array_size(heads) == 0) goto cleanup;
  if (json_array_size(heads) != 1) {
    rc = SALTS_EPROTO;
    goto cleanup;
  }
  head = json_array_get(heads, 0);
  compaction_id = json_get_string(head, "compaction_id");
  head_hash = json_get_string(head, "source_hash");
  thread_id = json_get_string(head, "thread_id");
  if (!compaction_id || !head_hash || !thread_id || strcmp(thread_id, context->thread_id) != 0 ||
      strcmp(json_get_string(head, "status") ? json_get_string(head, "status") : "",
             "committed") != 0 ||
      turbo_agent_context_exact_size(head, "source_event_end", &event_start) != SALTS_OK) {
    rc = SALTS_EPROTO;
    goto cleanup;
  }
  if (turbo_agent_runtime_store_get_json(context->runtime, TURBO_AGENT_COMPACTIONS_COLLECTION,
                                         compaction_id, &compaction) != 0) {
    rc = SALTS_EIO;
    goto cleanup;
  }
  compaction_hash = json_get_string(compaction, "source_hash");
  thread_id = json_get_string(compaction, "thread_id");
  summary = json_object_get(compaction, "summary");
  source = json_object_get(compaction, "source");
  computed_hash = source ? turbo_agent_context_source_hash(source) : NULL;
  if (!compaction_hash || strcmp(compaction_hash, head_hash) != 0 || !thread_id ||
      strcmp(thread_id, context->thread_id) != 0 ||
      strcmp(json_get_string(compaction, "status")
                 ? json_get_string(compaction, "status")
                 : "",
             "prepared") != 0 ||
      turbo_agent_context_exact_size(compaction, "source_event_end", &compaction_end) != SALTS_OK ||
      compaction_end != event_start || !computed_hash ||
      strcmp(computed_hash, compaction_hash) != 0 || !turbo_agent_context_summary_valid(summary)) {
    rc = SALTS_EPROTO;
    goto cleanup;
  }
  context->summary = json_clone(summary);
  context->head = json_clone(head);
  if (!context->summary || !context->head) {
    rc = SALTS_ENOMEM;
    goto cleanup;
  }
  context->event_start = event_start;
  rc = turbo_agent_context_apply_projection(context);

cleanup:
  free(computed_hash);
  turbo_runtime_json_destroy(compaction);
  turbo_runtime_json_destroy(heads);
  if (rc != SALTS_OK) {
    turbo_runtime_json_destroy(context->summary);
    turbo_runtime_json_destroy(context->head);
    context->summary = NULL;
    context->head = NULL;
    context->event_start = 0;
    turbo_agent_context_clear_projection(context);
  }
  return rc;
}

int turbo_agent_context_create(turbo_agent_runtime_t *runtime, turbo_agent_t *agent,
                               const char *thread_id, const turbo_agent_context_policy_t *policy,
                               turbo_agent_context_t **out_context) {
  turbo_agent_context_t *context;
  int rc;

  if (out_context) *out_context = NULL;
  if (!runtime || !agent || !thread_id || thread_id[0] == '\0' ||
      !turbo_agent_context_policy_valid(policy) || !out_context) {
    return SALTS_EINVAL;
  }
  context = (turbo_agent_context_t *)calloc(1, sizeof(*context));
  if (!context) return SALTS_ENOMEM;
  context->runtime = runtime;
  context->agent = agent;
  context->thread_id = turbo_agent_util_strdup(thread_id);
  context->policy = *policy;
  if (!context->thread_id) {
    turbo_agent_context_destroy(context);
    return SALTS_ENOMEM;
  }
  rc = turbo_agent_context_load_head(context);
  if (rc != SALTS_OK) {
    turbo_agent_context_destroy(context);
    return rc;
  }
  *out_context = context;
  return SALTS_OK;
}

void turbo_agent_context_destroy(turbo_agent_context_t *context) {
  if (!context) return;
  turbo_agent_context_clear_projection(context);
  turbo_runtime_json_destroy(context->summary);
  turbo_runtime_json_destroy(context->head);
  tstr_free(context->thread_id);
  free(context);
}

static int turbo_agent_context_estimate(turbo_agent_context_t *context, const json_value_t *value,
                                        uint64_t *out_tokens) {
  uint64_t tokens = 0;

  if (!context || !value || !out_tokens ||
      context->policy.estimate_tokens(value, &tokens, context->policy.user_data) != 0) {
    return SALTS_EIO;
  }
  *out_tokens = tokens;
  return SALTS_OK;
}

static int turbo_agent_context_estimate_request(turbo_agent_context_t *context, json_value_t *state,
                                                uint64_t *out_tokens) {
  char *request_json = NULL;
  json_value_t *request = NULL;
  int rc;

  if (turbo_agent_build_turn_request(context->agent, state, &request_json) != 0 || !request_json) {
    free(request_json);
    return SALTS_EIO;
  }
  request = json_parse(request_json, strlen(request_json));
  if (!request) {
    free(request_json);
    turbo_runtime_json_destroy(request);
    return SALTS_EPROTO;
  }
  free(request_json);
  rc = turbo_agent_context_estimate(context, request, out_tokens);
  turbo_runtime_json_destroy(request);
  return rc;
}

static int turbo_agent_context_boundary_valid(const json_value_t *events, size_t boundary) {
  const json_value_t *previous;

  if (!events || boundary == 0 || boundary > json_array_size(events)) return 0;
  previous = json_array_get(events, boundary - 1);
  if (turbo_agent_event_kind_is(previous, "tool_results")) return 1;
  return turbo_agent_event_kind_is(previous, "model") &&
         turbo_agent_model_event_tool_call_count(previous) == 0;
}

static size_t turbo_agent_context_previous_boundary(const json_value_t *events, size_t start,
                                                    size_t before) {
  size_t boundary;

  for (boundary = before; boundary > start + 1; --boundary) {
    size_t candidate = boundary - 1;
    if (turbo_agent_context_boundary_valid(events, candidate)) return candidate;
  }
  return start;
}

static size_t turbo_agent_context_next_boundary(const json_value_t *events, size_t after,
                                                size_t limit) {
  size_t boundary;

  for (boundary = after + 1; boundary <= limit; ++boundary) {
    if (turbo_agent_context_boundary_valid(events, boundary)) return boundary;
  }
  return limit + 1;
}

static json_value_t *turbo_agent_context_clone_event_range(const json_value_t *events, size_t start,
                                                           size_t end) {
  json_value_t *range = json_create_array();
  size_t index;

  if (!range || !events || start > end || end > json_array_size(events)) {
    turbo_runtime_json_destroy(range);
    return NULL;
  }
  for (index = start; index < end; ++index) {
    json_value_t *clone = json_clone(json_array_get(events, index));
    if (!clone || turbo_runtime_json_array_append(range, clone) != TURBO_RUNTIME_JSON_OK) {
      turbo_runtime_json_destroy(clone);
      turbo_runtime_json_destroy(range);
      return NULL;
    }
  }
  return range;
}

static json_value_t *turbo_agent_context_build_source(turbo_agent_context_t *context,
                                                      const json_value_t *events, size_t start,
                                                      size_t end) {
  json_value_t *source = json_create_object();
  json_value_t *previous =
      context->summary ? json_clone(context->summary) : json_create_null();
  json_value_t *range = turbo_agent_context_clone_event_range(events, start, end);

  if (!source || !previous || !range) {
    turbo_runtime_json_destroy(previous);
    turbo_runtime_json_destroy(range);
    turbo_runtime_json_destroy(source);
    return NULL;
  }
  if (turbo_runtime_json_object_set(source, "previous_summary", previous) !=
      TURBO_RUNTIME_JSON_OK) {
    turbo_runtime_json_destroy(previous);
    turbo_runtime_json_destroy(range);
    turbo_runtime_json_destroy(source);
    return NULL;
  }
  previous = NULL;
  if (turbo_runtime_json_object_set(source, "events", range) != TURBO_RUNTIME_JSON_OK) {
    turbo_runtime_json_destroy(range);
    turbo_runtime_json_destroy(source);
    return NULL;
  }
  return source;
}

static int turbo_agent_context_choose_cut(turbo_agent_context_t *context,
                                          const json_value_t *events, size_t *out_cut) {
  size_t count = json_array_size(events);
  size_t cursor = count;
  size_t cut = count;
  uint64_t retained = 0;
  int retained_segment = 0;

  if (context->event_start >= count) return SALTS_ENOENT;
  while (cursor > context->event_start) {
    size_t previous = turbo_agent_context_previous_boundary(events, context->event_start, cursor);
    json_value_t *segment;
    uint64_t segment_tokens;

    segment = turbo_agent_context_clone_event_range(events, previous, cursor);
    if (!segment) return SALTS_ENOMEM;
    if (turbo_agent_context_estimate(context, segment, &segment_tokens) != SALTS_OK) {
      turbo_runtime_json_destroy(segment);
      return SALTS_EIO;
    }
    turbo_runtime_json_destroy(segment);
    if (segment_tokens > UINT64_MAX - retained) return SALTS_ERANGE;
    if (retained_segment && retained + segment_tokens > context->policy.retain_recent_tokens) {
      break;
    }
    retained_segment = 1;
    retained += segment_tokens;
    cut = previous;
    cursor = previous;
    if (previous == context->event_start) break;
  }
  if (cut <= context->event_start) return SALTS_ENOENT;
  *out_cut = cut;
  return SALTS_OK;
}

static int turbo_agent_context_limit_source(turbo_agent_context_t *context,
                                            const json_value_t *events, size_t desired_cut,
                                            size_t *out_cut, json_value_t **out_source,
                                            uint64_t *out_source_tokens) {
  size_t candidate = turbo_agent_context_next_boundary(events, context->event_start, desired_cut);
  size_t selected = 0;
  json_value_t *selected_source = NULL;
  uint64_t selected_tokens = 0;

  *out_source = NULL;
  while (candidate <= desired_cut) {
    json_value_t *source =
        turbo_agent_context_build_source(context, events, context->event_start, candidate);
    uint64_t source_tokens;
    int rc;

    if (!source) {
      turbo_runtime_json_destroy(selected_source);
      return SALTS_ENOMEM;
    }
    rc = turbo_agent_context_estimate(context, source, &source_tokens);
    if (rc != SALTS_OK) {
      turbo_runtime_json_destroy(source);
      turbo_runtime_json_destroy(selected_source);
      return rc;
    }
    if (source_tokens > context->policy.max_summary_input_tokens) {
      turbo_runtime_json_destroy(source);
      break;
    }
    turbo_runtime_json_destroy(selected_source);
    selected_source = source;
    selected = candidate;
    selected_tokens = source_tokens;
    if (candidate == desired_cut) break;
    candidate = turbo_agent_context_next_boundary(events, candidate, desired_cut);
  }
  if (!selected_source) return SALTS_EMSGSIZE;
  *out_cut = selected;
  *out_source = selected_source;
  *out_source_tokens = selected_tokens;
  return SALTS_OK;
}

static char *turbo_agent_context_source_hash(const json_value_t *source) {
  static const char hex[] = "0123456789abcdef";
  unsigned char digest[SHA256_DIGEST_LENGTH];
  char *serialized;
  char *hash_text;
  size_t length = 0;
  size_t index;

  serialized = json_serialize(source, &length);
  if (!serialized) return NULL;
  if (!SHA256((const unsigned char *)serialized, length, digest)) {
    json_serialize_free(serialized);
    return NULL;
  }
  json_serialize_free(serialized);
  hash_text = (char *)malloc(SHA256_DIGEST_LENGTH * 2 + 1);
  if (!hash_text) return NULL;
  for (index = 0; index < SHA256_DIGEST_LENGTH; ++index) {
    hash_text[index * 2] = hex[digest[index] >> 4];
    hash_text[index * 2 + 1] = hex[digest[index] & 0x0f];
  }
  hash_text[SHA256_DIGEST_LENGTH * 2] = '\0';
  return hash_text;
}

static int turbo_agent_context_compact_once(turbo_agent_context_t *context, json_value_t *state) {
  const json_value_t *events = json_object_get(state, "events");
  json_value_t *source = NULL;
  json_value_t *summary = NULL;
  json_value_t *prepared = NULL;
  json_value_t *head = NULL;
  json_value_t *context_summary = NULL;
  json_value_t *agent_summary = NULL;
  json_value_t *prepared_summary = NULL;
  json_value_t *prepared_source = NULL;
  char *source_hash = NULL;
  char compaction_id[SALTS_UUID_STRING_SIZE];
  char created_at[64];
  salts_uuid_t uuid;
  uint64_t source_tokens = 0;
  uint64_t summary_tokens = 0;
  size_t desired_cut;
  size_t cut;
  int rc;

  if (!events || json_type(events) != JSON_ARRAY) return SALTS_EPROTO;
  rc = turbo_agent_context_choose_cut(context, events, &desired_cut);
  if (rc != SALTS_OK) return rc;
  rc =
      turbo_agent_context_limit_source(context, events, desired_cut, &cut, &source, &source_tokens);
  if (rc != SALTS_OK) return rc;
  if (context->event_start > TURBO_AGENT_CONTEXT_MAX_EXACT_JSON_INTEGER ||
      cut > TURBO_AGENT_CONTEXT_MAX_EXACT_JSON_INTEGER ||
      source_tokens > TURBO_AGENT_CONTEXT_MAX_EXACT_JSON_INTEGER) {
    rc = SALTS_ERANGE;
    goto cleanup;
  }
  if (context->policy.summarize(source, context->policy.max_summary_tokens, &summary,
                                context->policy.user_data) != 0 ||
      !turbo_agent_context_summary_valid(summary)) {
    rc = SALTS_EPROTO;
    goto cleanup;
  }
  rc = turbo_agent_context_estimate(context, summary, &summary_tokens);
  if (rc != SALTS_OK || summary_tokens > context->policy.max_summary_tokens) {
    rc = rc == SALTS_OK ? SALTS_EMSGSIZE : rc;
    goto cleanup;
  }
  source_hash = turbo_agent_context_source_hash(source);
  if (!source_hash || salts_uuid_v7_generate(&uuid) != SALTS_OK ||
      salts_uuid_format(&uuid, compaction_id, sizeof(compaction_id)) != SALTS_OK ||
      turbo_agent_runtime_make_timestamp(created_at, sizeof(created_at)) != 0) {
    rc = SALTS_EIO;
    goto cleanup;
  }
  prepared = json_create_object();
  head = json_create_object();
  context_summary = json_clone(summary);
  agent_summary = json_clone(summary);
  prepared_summary = json_clone(summary);
  prepared_source = json_clone(source);
  if (!prepared || !head || !context_summary || !agent_summary || !prepared_summary ||
      !prepared_source) {
    rc = SALTS_ENOMEM;
    goto cleanup;
  }
  json_object_set_number(prepared, "schema_version",
                               (double)TURBO_AGENT_CONTEXT_SCHEMA_VERSION);
  json_object_set_string(prepared, "compaction_id", compaction_id);
  json_object_set_string(prepared, "thread_id", context->thread_id);
  json_object_set_string(prepared, "status", "prepared");
  json_object_set_number(prepared, "source_event_start", (double)context->event_start);
  json_object_set_number(prepared, "source_event_end", (double)cut);
  json_object_set_number(prepared, "source_tokens", (double)source_tokens);
  json_object_set_string(prepared, "source_hash", source_hash);
  json_object_set_string(prepared, "created_at", created_at);
  if (turbo_runtime_json_object_set(prepared, "summary", prepared_summary) !=
      TURBO_RUNTIME_JSON_OK) {
    rc = SALTS_ENOMEM;
    goto cleanup;
  }
  prepared_summary = NULL;
  if (turbo_runtime_json_object_set(prepared, "source", prepared_source) != TURBO_RUNTIME_JSON_OK) {
    rc = SALTS_ENOMEM;
    goto cleanup;
  }
  prepared_source = NULL;

  json_object_set_number(head, "schema_version", (double)TURBO_AGENT_CONTEXT_SCHEMA_VERSION);
  json_object_set_string(head, "thread_id", context->thread_id);
  json_object_set_string(head, "status", "committed");
  json_object_set_string(head, "compaction_id", compaction_id);
  json_object_set_string(head, "source_hash", source_hash);
  json_object_set_number(head, "source_event_end", (double)cut);
  json_object_set_string(head, "committed_at", created_at);
  if (turbo_agent_runtime_store_put_json(context->runtime, TURBO_AGENT_COMPACTIONS_COLLECTION,
                                         compaction_id, prepared) != 0) {
    rc = SALTS_EIO;
    goto cleanup;
  }
  if (turbo_agent_runtime_store_put_json(context->runtime, TURBO_AGENT_CONTEXT_HEADS_COLLECTION,
                                         context->thread_id, head) != 0) {
    rc = SALTS_EIO;
    goto cleanup;
  }

  turbo_runtime_json_destroy(context->summary);
  turbo_runtime_json_destroy(context->head);
  turbo_runtime_json_destroy(context->agent->context_summary);
  context->summary = context_summary;
  context_summary = NULL;
  context->head = head;
  head = NULL;
  context->event_start = cut;
  context->agent->context_summary = agent_summary;
  agent_summary = NULL;
  context->agent->context_event_start = cut;
  context->agent->context_projection_active = 1;
  rc = SALTS_OK;

cleanup:
  free(source_hash);
  turbo_runtime_json_destroy(prepared_source);
  turbo_runtime_json_destroy(prepared_summary);
  turbo_runtime_json_destroy(agent_summary);
  turbo_runtime_json_destroy(context_summary);
  turbo_runtime_json_destroy(head);
  turbo_runtime_json_destroy(prepared);
  turbo_runtime_json_destroy(summary);
  turbo_runtime_json_destroy(source);
  return rc;
}

int turbo_agent_context_prepare(turbo_agent_context_t *context, json_value_t *state,
                                int force_compaction) {
  uint64_t request_tokens;
  uint64_t input_capacity;
  uint32_t compacted = 0;
  int rc;

  if (!context || !state) return SALTS_EINVAL;
  if (force_compaction) return turbo_agent_context_compact_once(context, state);
  rc = turbo_agent_context_apply_projection(context);
  if (rc != SALTS_OK) return rc;
  input_capacity = context->policy.context_window_tokens - context->policy.reserve_output_tokens;
  rc = turbo_agent_context_estimate_request(context, state, &request_tokens);
  if (rc != SALTS_OK) return rc;
  while (request_tokens >= context->policy.compact_trigger_tokens &&
         compacted < context->policy.max_compactions_per_turn) {
    rc = turbo_agent_context_compact_once(context, state);
    if (rc == SALTS_ENOENT) break;
    if (rc != SALTS_OK) return rc;
    ++compacted;
    rc = turbo_agent_context_estimate_request(context, state, &request_tokens);
    if (rc != SALTS_OK) return rc;
  }
  return request_tokens <= input_capacity ? SALTS_OK : SALTS_EMSGSIZE;
}

int turbo_agent_context_handle_overflow(turbo_agent_context_t *context, json_value_t *state,
                                        int transport_status, const char *response_json) {
  int detected;
  int rc;

  if (!context || !state || !context->policy.is_context_overflow) return 0;
  detected = context->policy.is_context_overflow(transport_status, response_json,
                                                 context->policy.user_data);
  if (detected < 0) return SALTS_EPROTO;
  if (detected == 0) return 0;
  rc = turbo_agent_context_compact_once(context, state);
  return rc == SALTS_OK ? 1 : rc;
}

int turbo_agent_context_status(turbo_agent_context_t *context, json_value_t **out_status) {
  if (!context || !out_status) return SALTS_EINVAL;
  *out_status = NULL;
  if (!context->head) return SALTS_ENOENT;
  *out_status = json_clone(context->head);
  return *out_status ? SALTS_OK : SALTS_ENOMEM;
}
