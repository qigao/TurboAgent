#include "turbo_agent_resilience_internal.h"

#include "turbo_agent_core_internal.h"
#include "turbo_agent_runtime_internal.h"
#include "turbo_agent_util_internal.h"

#include <turbo_thread.h>

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define TURBO_AGENT_RETRY_DEFAULT_MAX_ATTEMPTS 1u
#define TURBO_AGENT_RETRY_DEFAULT_BASE_DELAY_MS 250u
#define TURBO_AGENT_RETRY_DEFAULT_MAX_DELAY_MS 8000u
#define TURBO_AGENT_RETRY_DEFAULT_MAX_ELAPSED_MS 30000u
#define TURBO_AGENT_RETRY_DEFAULT_REQUEST_TIMEOUT_MS 30000u
#define TURBO_AGENT_RETRY_DEFAULT_CONNECT_TIMEOUT_MS 10000u
#define TURBO_AGENT_USAGE_MAX_EXACT_INTEGER UINT64_C(9007199254740991)

void turbo_agent_retry_policy_init(turbo_agent_retry_policy_t *policy) {
  if (!policy) return;
  memset(policy, 0, sizeof(*policy));
  policy->struct_size = sizeof(*policy);
  policy->abi_version = TURBO_AGENT_RETRY_POLICY_ABI_VERSION;
  policy->max_attempts = TURBO_AGENT_RETRY_DEFAULT_MAX_ATTEMPTS;
  policy->base_delay_ms = TURBO_AGENT_RETRY_DEFAULT_BASE_DELAY_MS;
  policy->max_delay_ms = TURBO_AGENT_RETRY_DEFAULT_MAX_DELAY_MS;
  policy->max_elapsed_ms = TURBO_AGENT_RETRY_DEFAULT_MAX_ELAPSED_MS;
  policy->request_timeout_ms = TURBO_AGENT_RETRY_DEFAULT_REQUEST_TIMEOUT_MS;
  policy->connect_timeout_ms = TURBO_AGENT_RETRY_DEFAULT_CONNECT_TIMEOUT_MS;
}

static int turbo_agent_retry_policy_valid(const turbo_agent_retry_policy_t *policy) {
  return policy && policy->struct_size >= sizeof(*policy) &&
         policy->abi_version == TURBO_AGENT_RETRY_POLICY_ABI_VERSION && policy->max_attempts > 0 &&
         policy->base_delay_ms > 0 && policy->max_delay_ms >= policy->base_delay_ms &&
         policy->max_elapsed_ms > 0 && policy->request_timeout_ms > 0 &&
         policy->connect_timeout_ms > 0 && policy->jitter_percent <= 100;
}

int turbo_agent_retry_configure(turbo_agent_t *agent, const turbo_agent_retry_policy_t *policy) {
  if (!agent || !turbo_agent_retry_policy_valid(policy)) return SALTS_EINVAL;
  agent->retry_policy = *policy;
  return SALTS_OK;
}

int turbo_agent_transport_v2_set(turbo_agent_t *agent, turbo_agent_transport_v2_fn transport,
                                 void *user_data,
                                 turbo_agent_transport_v2_user_data_free_fn user_data_free) {
  if (!agent || !transport) return SALTS_EINVAL;
  if (agent->transport_v2_user_data_free)
    agent->transport_v2_user_data_free(agent->transport_v2_user_data);
  agent->transport_v2 = transport;
  agent->transport_v2_user_data = user_data;
  agent->transport_v2_user_data_free = user_data_free;
  return SALTS_OK;
}

static unsigned int turbo_agent_retry_delay(const turbo_agent_retry_policy_t *policy,
                                            unsigned int completed_attempts,
                                            unsigned int retry_after_ms) {
  uint64_t delay = policy->base_delay_ms;
  unsigned int shift = completed_attempts > 1 ? completed_attempts - 1 : 0;
  while (shift-- > 0 && delay < policy->max_delay_ms) {
    delay = delay > UINT64_MAX / 2 ? UINT64_MAX : delay * 2;
  }
  if (delay > policy->max_delay_ms) delay = policy->max_delay_ms;
  if (policy->honor_retry_after && retry_after_ms > delay && retry_after_ms <= policy->max_delay_ms)
    delay = retry_after_ms;
  if (policy->jitter_percent > 0) {
    uint64_t span = delay * policy->jitter_percent / 100;
    uint64_t seed = (uint64_t)completed_attempts * UINT64_C(1103515245) + UINT64_C(12345);
    uint64_t width = span > (UINT64_MAX - 1) / 2 ? UINT64_MAX : span * 2 + 1;
    int64_t offset = width ? (int64_t)(seed % width) - (int64_t)span : 0;
    delay = offset < 0 && (uint64_t)(-offset) > delay ? 0 : (uint64_t)((int64_t)delay + offset);
  }
  return delay > UINT_MAX ? UINT_MAX : (unsigned int)delay;
}

static int turbo_agent_retry_wait(const turbo_cancel_token_t *token, unsigned int delay_ms) {
  if (!token) {
    turbo_sleep_ms(delay_ms);
    return SALTS_OK;
  }
  if (turbo_cancel_token_wait(token, delay_ms) == TURBO_CANCEL_WAIT_SIGNALED)
    return turbo_cancel_token_check(token);
  return SALTS_OK;
}

static int turbo_agent_attempt_append(json_value_t *state, unsigned int attempt,
                                      const turbo_agent_transport_response_t *response,
                                      const char *outcome, unsigned int delay_ms,
                                      uint64_t started_ms, uint64_t finished_ms) {
  json_value_t *attempts;
  json_value_t *record;
  if (!state) return SALTS_OK;
  attempts = turbo_json_object_get(state, "provider_attempts");
  if (!attempts) {
    attempts = turbo_json_create_array();
    if (!attempts) return SALTS_ENOMEM;
    turbo_json_object_add(state, "provider_attempts", attempts);
  }
  if (turbo_json_type(attempts) != TURBO_JSON_ARRAY) return SALTS_EPROTO;
  record = turbo_json_create_object();
  if (!record) return SALTS_ENOMEM;
  turbo_json_object_set_number(record, "schema_version", 1);
  turbo_json_object_set_number(record, "attempt", attempt);
  turbo_json_object_set_number(record, "transport_status", response->transport_status);
  turbo_json_object_set_number(record, "http_status", response->http_status);
  turbo_json_object_set_bool(record, "retryable", response->retryable);
  turbo_json_object_set_number(record, "delay_ms", delay_ms);
  turbo_json_object_set_number(record, "started_mono_ms", (double)started_ms);
  turbo_json_object_set_number(record, "finished_mono_ms", (double)finished_ms);
  turbo_json_object_set_string(record, "outcome", outcome);
  if (response->provider_request_id)
    turbo_json_object_set_string(record, "provider_request_id", response->provider_request_id);
  turbo_json_array_add(attempts, record);
  return SALTS_OK;
}

int turbo_agent_resilient_transport(turbo_agent_t *agent, json_value_t *state,
                                    const char *request_json, char **out_response_json) {
  turbo_agent_execution_context_t context = {0};
  uint64_t execution_start;
  unsigned int attempt;
  if (!agent || !request_json || !out_response_json) return SALTS_EINVAL;
  *out_response_json = NULL;
  tstr_free(agent->last_provider_request_id);
  agent->last_provider_request_id = NULL;
  if (!agent->transport_v2) {
    return agent->transport_fn(request_json, out_response_json, agent->transport_user_data);
  }
  turbo_agent_execution_context_get(&context);
  execution_start = turbo_monotonic_ms();
  for (attempt = 1; attempt <= agent->retry_policy.max_attempts; ++attempt) {
    turbo_agent_transport_response_t response = {sizeof(turbo_agent_transport_response_t),
                                                 TURBO_AGENT_TRANSPORT_V2_ABI_VERSION};
    uint64_t started_ms = turbo_monotonic_ms();
    uint64_t finished_ms;
    unsigned int delay_ms = 0;
    int invoke_rc;
    if (context.cancel_token && turbo_cancel_token_check(context.cancel_token) != SALTS_OK)
      return turbo_cancel_token_check(context.cancel_token);
    invoke_rc = agent->transport_v2(
        request_json, context.cancel_token, agent->retry_policy.request_timeout_ms,
        agent->retry_policy.connect_timeout_ms, &response, agent->transport_v2_user_data);
    finished_ms = turbo_monotonic_ms();
    if (response.struct_size < sizeof(response) ||
        response.abi_version != TURBO_AGENT_TRANSPORT_V2_ABI_VERSION) {
      free(response.body);
      return SALTS_EPROTO;
    }
    if (invoke_rc == 0 && response.transport_status == 0 && response.body) {
      if (turbo_agent_attempt_append(state, attempt, &response, "succeeded", 0, started_ms,
                                     finished_ms) != SALTS_OK) {
        free(response.body);
        return SALTS_EIO;
      }
      *out_response_json = response.body;
      if (response.provider_request_id) {
        agent->last_provider_request_id = turbo_agent_util_strdup(response.provider_request_id);
        if (!agent->last_provider_request_id) {
          free(*out_response_json);
          *out_response_json = NULL;
          return SALTS_ENOMEM;
        }
      }
      return SALTS_OK;
    }
    if (!response.retryable || attempt == agent->retry_policy.max_attempts) {
      (void)turbo_agent_attempt_append(state, attempt, &response, "failed", 0, started_ms,
                                       finished_ms);
      *out_response_json = response.body;
      return response.transport_status != 0 ? response.transport_status
                                            : (invoke_rc != 0 ? invoke_rc : SALTS_EIO);
    }
    delay_ms = turbo_agent_retry_delay(&agent->retry_policy, attempt, response.retry_after_ms);
    if (finished_ms - execution_start > agent->retry_policy.max_elapsed_ms ||
        delay_ms > agent->retry_policy.max_elapsed_ms - (finished_ms - execution_start)) {
      (void)turbo_agent_attempt_append(state, attempt, &response, "elapsed_limit", 0, started_ms,
                                       finished_ms);
      *out_response_json = response.body;
      return SALTS_ETIMEDOUT;
    }
    if (turbo_agent_attempt_append(state, attempt, &response, "retry_scheduled", delay_ms,
                                   started_ms, finished_ms) != SALTS_OK) {
      free(response.body);
      return SALTS_EIO;
    }
    free(response.body);
    if (turbo_agent_retry_wait(context.cancel_token, delay_ms) != SALTS_OK)
      return context.cancel_token ? turbo_cancel_token_check(context.cancel_token)
                                  : SALTS_ECANCELED;
  }
  return SALTS_EIO;
}

static int turbo_agent_usage_exact_u64(const json_value_t *usage, const char *primary,
                                       const char *alternate, uint64_t *out_value) {
  double value;
  const json_value_t *field = turbo_json_object_get(usage, primary);
  if (!field && alternate) field = turbo_json_object_get(usage, alternate);
  if (!field) {
    *out_value = 0;
    return SALTS_ENOENT;
  }
  value = turbo_runtime_json_value_as_double(field, -1.0);
  if (value < 0 || value > (double)TURBO_AGENT_USAGE_MAX_EXACT_INTEGER ||
      value != (double)(uint64_t)value)
    return SALTS_ERANGE;
  *out_value = (uint64_t)value;
  return SALTS_OK;
}

int turbo_agent_usage_record(turbo_agent_t *agent, json_value_t *state,
                             const json_value_t *response) {
  turbo_agent_execution_context_t context = {0};
  const json_value_t *usage;
  json_value_t *records;
  json_value_t *record;
  json_value_t *cost;
  uint64_t input_tokens = 0;
  uint64_t output_tokens = 0;
  uint64_t total_tokens = 0;
  int input_rc;
  int output_rc;
  int total_rc;
  if (!agent || !state || !response) return SALTS_EINVAL;
  turbo_agent_execution_context_get(&context);
  usage = turbo_json_object_get(response, "usage");
  if (!usage || turbo_json_type(usage) != TURBO_JSON_OBJECT) return SALTS_OK;
  input_rc = turbo_agent_usage_exact_u64(usage, "input_tokens", "prompt_tokens", &input_tokens);
  output_rc =
      turbo_agent_usage_exact_u64(usage, "output_tokens", "completion_tokens", &output_tokens);
  total_rc = turbo_agent_usage_exact_u64(usage, "total_tokens", NULL, &total_tokens);
  if (input_rc == SALTS_ERANGE || output_rc == SALTS_ERANGE || total_rc == SALTS_ERANGE)
    return SALTS_ERANGE;
  if (total_rc == SALTS_ENOENT) {
    if (UINT64_MAX - input_tokens < output_tokens) return SALTS_ERANGE;
    total_tokens = input_tokens + output_tokens;
  }
  if (total_tokens > TURBO_AGENT_USAGE_MAX_EXACT_INTEGER) return SALTS_ERANGE;
  records = turbo_json_object_get(state, "usage_records");
  if (!records) {
    records = turbo_json_create_array();
    if (!records) return SALTS_ENOMEM;
    turbo_json_object_add(state, "usage_records", records);
  }
  if (turbo_json_type(records) != TURBO_JSON_ARRAY) return SALTS_EPROTO;
  record = turbo_json_create_object();
  cost = turbo_json_create_object();
  if (!record || !cost) {
    turbo_runtime_json_destroy(record);
    turbo_runtime_json_destroy(cost);
    return SALTS_ENOMEM;
  }
  turbo_json_object_set_number(record, "schema_version", 1);
  if (context.run_id) turbo_json_object_set_string(record, "run_id", context.run_id);
  else turbo_json_object_set_null(record, "run_id");
  if (context.thread_id) turbo_json_object_set_string(record, "thread_id", context.thread_id);
  else turbo_json_object_set_null(record, "thread_id");
  turbo_json_object_set_null(record, "turn_seq");
  turbo_json_object_set_string(record, "operation", "model");
  turbo_json_object_set_string(record, "provider", turbo_agent_provider_name(agent));
  turbo_json_object_set_string(record, "model", agent->model);
  if (agent->last_provider_request_id)
    turbo_json_object_set_string(record, "provider_request_id", agent->last_provider_request_id);
  turbo_json_object_set_number(record, "input_tokens", (double)input_tokens);
  turbo_json_object_set_null(record, "cached_input_tokens");
  turbo_json_object_set_number(record, "output_tokens", (double)output_tokens);
  turbo_json_object_set_null(record, "reasoning_tokens");
  turbo_json_object_set_number(record, "total_tokens", (double)total_tokens);
  turbo_json_object_set_bool(record, "estimated", 0);
  turbo_json_object_set_string(cost, "status", "unknown");
  turbo_json_object_set_string(cost, "currency", "USD");
  turbo_json_object_set_null(cost, "amount_micros");
  turbo_json_object_set_null(cost, "price_catalog_version");
  turbo_json_object_add(record, "cost", cost);
  turbo_json_array_add(records, record);
  return SALTS_OK;
}
