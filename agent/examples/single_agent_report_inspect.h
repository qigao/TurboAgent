#ifndef SINGLE_AGENT_REPORT_INSPECT_EXAMPLE_H
#define SINGLE_AGENT_REPORT_INSPECT_EXAMPLE_H

#include "single_agent_report.h"
#include <limits.h>

/* Read-only example Host reconciliation. Bindings come from the application,
 * never from persisted paths or model data. Endpoint ids are scoped by router:
 * model-only (0) versus the reviewed tool-loop router (1). */
typedef struct single_agent_report_binding_s {
  const char *endpoint;
  turbo_agent_runtime_t *runtime;
  int tool_loop;
} single_agent_report_binding_t;

static const char *const report_inspect_suffixes[] = {"worker-a", "worker-b", "fan-in",
    "aggregate", "aggregate-final", "execute", "pending", "review", "execute-final", "ready",
    "stop", "stop-worker-a", "stop-worker-b", "stop-aggregate", "stop-execute", "gate-failure"};
enum { REPORT_INSPECT_COUNT = 16, REPORT_INSPECT_MAX_BYTES = 384 * 1024 };

static int64_t report_inspect_integer(const json_value_t *v, const char *key) {
  return turbo_runtime_json_value_as_int64(json_object_get(v, key), INT64_MIN);
}
static int report_inspect_text(const json_value_t *v, const char *key, size_t max) {
  const json_value_t *field = json_object_get(v, key); const char *s = json_string(field);
  return s && s[0] && strlen(s) == json_string_len(field) && strlen(s) <= max;
}
static int report_inspect_equal(const json_value_t *a, const char *ak, const json_value_t *b, const char *bk) {
  const char *x = json_get_string(a, ak), *y = json_get_string(b, bk);
  return x && y && strlen(x) == json_string_len(json_object_get(a, ak)) &&
      strlen(y) == json_string_len(json_object_get(b, bk)) && !strcmp(x, y);
}
static int report_inspect_is(const json_value_t *v, const char *key, const char *expected) {
  const char *s = json_get_string(v, key);
  return s && expected && strlen(s) == json_string_len(json_object_get(v, key)) && !strcmp(s, expected);
}
static int report_inspect_endpoint_valid(const char *id) {
  size_t n = id ? strlen(id) : 0;
  if (!n || n > 64) return 0;
  for (size_t i = 0; i < n; ++i) {
    unsigned char c = (unsigned char)id[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return 0;
  }
  return 1;
}
static int report_inspect_false(json_value_t *v, const char *key) {
  json_value_t *b = json_create_bool(0);
  if (!b) return SALTS_ENOMEM;
  if (turbo_runtime_json_object_set(v, key, b)) { json_free(b); return SALTS_ENOMEM; }
  return SALTS_OK;
}
static int report_inspect_size(const json_value_t *v, size_t max) {
  char *s = json_serialize(v, NULL); int rc = !s ? SALTS_ENOMEM : strlen(s) > max ? SALTS_ENOBUFS : SALTS_OK;
  json_serialize_free(s); return rc;
}
static int report_inspect_shape(const json_value_t *v, const char *const *keys, size_t n) {
  uint64_t seen = 0;
  if (json_type(v) != JSON_OBJECT || json_object_size(v) != n || n > 63) return 0;
  for (size_t i = 0; i < n; ++i) {
    const char *key = json_object_key(v, i); size_t j;
    if (!key || strlen(key) != json_object_key_len(v, i)) return 0;
    for (j = 0; j < n; ++j) if (!strcmp(key, keys[j])) break;
    if (j == n || (seen & (UINT64_C(1) << j))) return 0;
    seen |= UINT64_C(1) << j;
  }
  return 1;
}
static int report_inspect_key(const char *report, const char *suffix, char key[64]) {
  single_agent_report_config_t c = {0}; c.audit_id = report;
  return single_agent_report_key(&c, suffix, key);
}

static int report_inspect_manifest(const json_value_t *m, const char *id, const char *parent) {
  static const char *const keys[] = {"schema_version", "report_id", "semantics", "clock_domain",
      "parent_run_id", "started_mono_ms", "deadline_mono_ms", "phase_ids"};
  const json_value_t *phases = json_object_get(m, "phase_ids"); char key[64];
  int64_t start = report_inspect_integer(m, "started_mono_ms"), deadline = report_inspect_integer(m, "deadline_mono_ms");
  size_t count = json_object_size(phases);
  if (!report_inspect_shape(m, keys, sizeof(keys)/sizeof(keys[0])) ||
      report_inspect_integer(m, "schema_version") != 1 || !report_inspect_text(m, "report_id", 32) ||
      !report_inspect_is(m, "report_id", id) || !report_inspect_text(m, "parent_run_id", 128) ||
      !report_inspect_is(m, "parent_run_id", parent) ||
      !report_inspect_is(m, "semantics", "pre_report_dispatch_manifest") ||
      !report_inspect_is(m, "clock_domain", "origin_process_monotonic") || start < 0 ||
      deadline <= start || deadline > INT64_C(9007199254740991) || deadline - start > 86400000 ||
      (count != 10 && count != 15 && count != REPORT_INSPECT_COUNT) ||
      !report_inspect_shape(phases, report_inspect_suffixes, count)) return SALTS_EPROTO;
  for (size_t i = 0; i < count; ++i) {
    if (report_inspect_key(id, report_inspect_suffixes[i], key) ||
        !report_inspect_text(phases, report_inspect_suffixes[i], 64) ||
        !report_inspect_is(phases, report_inspect_suffixes[i], key)) return SALTS_EPROTO;
  }
  return report_inspect_size(m, 16384);
}

static turbo_agent_runtime_t *report_inspect_binding(const single_agent_report_binding_t *bindings,
    size_t count, const char *endpoint, int loop) {
  for (size_t i = 0; i < count; ++i)
    if (bindings[i].tool_loop == loop && !strcmp(bindings[i].endpoint, endpoint)) return bindings[i].runtime;
  return NULL;
}

/* Only exact original anchors are read. In particular there is no thread-head,
 * list scan, state payload replay, lease renewal, callback or authority grant. */
static int report_inspect_anchors(const json_value_t *o, turbo_agent_runtime_t *runtime) {
  const char *run_id = json_get_string(o, "runtime_run_id"), *checkpoint_id = json_get_string(o, "runtime_checkpoint_id");
  json_value_t *run = NULL, *checkpoint = NULL; int rc = SALTS_OK;
  if (!run_id) return rc;
  rc = turbo_agent_runtime_store_get_json_status(runtime, "runs", run_id, &run);
  if (rc) goto done;
  if (!report_inspect_text(run, "id", 128) || !report_inspect_is(run, "id", run_id) ||
      !report_inspect_text(run, "thread_id", 64) || !report_inspect_equal(run, "thread_id", o, "thread_id") ||
      !report_inspect_text(run, "parent_agent_run_id", 128) || !report_inspect_equal(run, "parent_agent_run_id", o, "parent_run_id") ||
      !report_inspect_text(run, "status", 128)) {
    rc = SALTS_EPROTO; goto done;
  }
  if (!checkpoint_id) goto done;
  rc = turbo_agent_runtime_store_get_json_status(runtime, "checkpoints", checkpoint_id, &checkpoint);
  if (!rc && (!report_inspect_text(checkpoint, "id", 128) || !report_inspect_is(checkpoint, "id", checkpoint_id) ||
      !report_inspect_text(checkpoint, "run_id", 128) || !report_inspect_is(checkpoint, "run_id", run_id) ||
      !report_inspect_text(checkpoint, "thread_id", 64) || !report_inspect_equal(checkpoint, "thread_id", o, "thread_id") ||
      !report_inspect_text(checkpoint, "parent_agent_run_id", 128) ||
      !report_inspect_equal(checkpoint, "parent_agent_run_id", o, "parent_run_id") ||
      /* Segment checkpoints are resumable snapshots, always interrupted;
       * cancelled/timed_out are Run/summary outcomes, not checkpoint states. */
      !report_inspect_is(checkpoint, "status", "interrupted") ||
      (!report_inspect_is(o, "runtime_status", "interrupted") &&
       !report_inspect_is(o, "runtime_status", "cancelled") &&
       !report_inspect_is(o, "runtime_status", "timed_out")))) rc = SALTS_EPROTO;
done:
  json_free(run); json_free(checkpoint); return rc;
}

static int report_inspect_route(const json_value_t *o, const char *report, const char *parent,
    const char *role, const single_agent_report_binding_t *bindings, size_t count) {
  char key[64]; int loop = !strcmp(role, "execute");
  const char *endpoint = json_get_string(o, "endpoint");
  if (!endpoint || report_inspect_key(report, role, key) || !report_inspect_is(o, "thread_id", key) ||
      !report_inspect_is(o, "parent_run_id", parent) ||
      !report_inspect_is(o, "role", !strcmp(role, "aggregate") ? "plan" : "execute") ||
      report_inspect_integer(o, "max_model_turns") != (loop ? 2 : 0) ||
      !report_inspect_binding(bindings, count, endpoint, loop)) return SALTS_EPROTO;
  return SALTS_OK;
}

static int report_inspect_observation(const json_value_t *o, const json_value_t *intent,
    const char *report, const char *parent, const char *role,
    const single_agent_report_binding_t *bindings, size_t count) {
  static const char *const identities[] = {"task_id", "endpoint", "model", "provider", "role", "thread_id", "parent_run_id"};
  int rc;
  if (!intent) return SALTS_EPROTO;
  rc = report_inspect_route(o, report, parent, role, bindings, count);
  if (rc) return rc;
  for (size_t i = 0; i < sizeof(identities)/sizeof(identities[0]); ++i)
    if (!report_inspect_equal(o, identities[i], intent, identities[i])) return SALTS_EPROTO;
  return report_inspect_anchors(o, report_inspect_binding(bindings, count,
      json_get_string(o, "endpoint"), !strcmp(role, "execute")));
}

static int report_inspect_stop(const json_value_t *stop, json_value_t *const p[],
    const json_value_t *manifest, const char *id, const char *parent) {
  static const char *const keys[] = {"schema_version", "semantics", "report_id", "parent_run_id",
      "operation_phase", "operation_code", "observed_mono_ms", "clock_domain", "tasks", "acknowledged_gates"};
  static const char *const fields[] = {"role", "observation_id", "observation_boundary", "task_id", "execution_id", "thread_id",
      "execution_status", "runtime_result_code", "unresponsive", "result_taken", "approval_pending"};
  static const char *const roles[] = {"worker-a", "worker-b", "aggregate", "execute"};
  static const char *const phases[] = {"workers", "aggregation", "execution", "approval", "delivery", "delivery-audit"};
  static const size_t min_normal[] = {0, 3, 5, 7, 8, 10}, max_normal[] = {3, 5, 6, 9, 9, 10};
  const json_value_t *tasks = json_object_get(stop, "tasks"), *gates = json_object_get(stop, "acknowledged_gates");
  int64_t code = report_inspect_integer(stop, "operation_code"), observed = report_inspect_integer(stop, "observed_mono_ms");
  char key[64]; size_t normal = 0, phase; unsigned seen = 0;
  for (phase = 0; phase < sizeof(phases)/sizeof(phases[0]); ++phase) if (report_inspect_is(stop, "operation_phase", phases[phase])) break;
  if (!report_inspect_shape(stop, keys, sizeof(keys)/sizeof(keys[0])) || report_inspect_integer(stop, "schema_version") != 1 ||
      !report_inspect_text(stop, "report_id", 32) || !report_inspect_is(stop, "report_id", id) ||
      !report_inspect_text(stop, "parent_run_id", 128) || !report_inspect_is(stop, "parent_run_id", parent) ||
      !report_inspect_is(stop, "semantics", "post_drain_report_stop_observation") ||
      !report_inspect_is(stop, "clock_domain", "origin_process_monotonic") ||
      phase == sizeof(phases)/sizeof(phases[0]) || !report_inspect_text(stop, "operation_phase", 32) ||
      code < INT_MIN || code > INT_MAX || !code || observed < report_inspect_integer(manifest, "started_mono_ms") ||
      observed > INT64_C(9007199254740991) || json_type(tasks) != JSON_ARRAY || json_array_size(tasks) > 2 ||
      json_type(gates) != JSON_ARRAY) return SALTS_EPROTO;
  while (normal < 10 && p[normal]) ++normal;
  if (normal < min_normal[phase] || normal > max_normal[phase]) return SALTS_EPROTO;
  if (json_array_size(gates) != normal + 1) return SALTS_EPROTO;
  for (size_t i = 0; i <= normal; ++i) {
    const json_value_t *v = json_array_get(gates, i); const char *s = json_string(v);
    if (i && report_inspect_key(id, report_inspect_suffixes[i-1], key)) return SALTS_EPROTO;
    if (!s || strlen(s) != json_string_len(v) || strcmp(s, i ? key : id)) return SALTS_EPROTO;
  }
  for (size_t i = 0; i < json_array_size(tasks); ++i) {
    const json_value_t *entry = json_array_get(tasks, i), *observation; size_t role; int taken, index;
    if (!report_inspect_shape(entry, fields, sizeof(fields)/sizeof(fields[0]))) return SALTS_EPROTO;
    for (role = 0; role < 4; ++role) if (report_inspect_is(entry, "role", roles[role])) break;
    if (role == 4 || (seen & (1u << role))) return SALTS_EPROTO;
    seen |= 1u << role;
    if (json_type(json_object_get(entry, "result_taken")) != JSON_BOOL ||
        json_type(json_object_get(entry, "approval_pending")) != JSON_BOOL ||
        json_type(json_object_get(entry, "unresponsive")) != JSON_BOOL) return SALTS_EPROTO;
    taken = json_get_bool(entry, "result_taken", 0);
    if (taken && role < 2) return SALTS_EPROTO;
    if ((phase == 0 && role >= 2) || (phase == 1 && role != 2) || (phase >= 2 && role != 3)) return SALTS_EPROTO;
    if (!taken) index = 11 + (int)role;
    else if (role == 2) index = 4;
    else index = report_inspect_is(entry, "observation_id", json_get_string(json_object_get(manifest, "phase_ids"), "pending")) ? 6 : 8;
    observation = p[index];
    if (!observation || !report_inspect_text(entry, "role", 16) ||
        !report_inspect_text(entry, "observation_boundary", 32) ||
        !report_inspect_is(entry, "observation_boundary", taken ? "before_result_transfer" : "after_drain") ||
        !report_inspect_text(entry, "observation_id", 64) || report_inspect_key(id, report_inspect_suffixes[index], key) ||
        !report_inspect_is(entry, "observation_id", key)) return SALTS_EPROTO;
    for (size_t j = 3; j <= 5; ++j) if (!report_inspect_text(entry, fields[j], 64) || !report_inspect_equal(entry, fields[j], observation, fields[j])) return SALTS_EPROTO;
    if (report_inspect_integer(entry, "execution_status") < TURBO_AGENT_EXECUTION_COMPLETED ||
        report_inspect_integer(entry, "execution_status") != report_inspect_integer(observation, "execution_status") ||
        report_inspect_integer(entry, "runtime_result_code") != report_inspect_integer(observation, "runtime_result_code") ||
        json_get_bool(entry, "unresponsive", 0) != json_get_bool(observation, "unresponsive", 0) ||
        json_get_bool(entry, "approval_pending", 0) || (!taken && json_get_bool(observation, "approval_pending", 0))) return SALTS_EPROTO;
  }
  return report_inspect_size(stop, 16384);
}

static int report_inspect_cleanup_failure(const json_value_t *failure, json_value_t *const p[],
    const json_value_t *manifest, const char *id, const char *parent) {
  static const char *const keys[] = {"schema_version", "semantics", "report_id", "parent_run_id",
      "operation_phase", "operation_code", "gate_id", "gate_return_code", "gate_outcome", "observed_mono_ms",
      "clock_domain", "acknowledged_gates", "acknowledged_cleanup"};
  static const char *const phases[] = {"workers", "aggregation", "execution", "approval", "delivery", "delivery-audit"};
  static const size_t min_normal[] = {0, 3, 5, 7, 8, 10}, max_normal[] = {3, 5, 6, 9, 9, 10};
  const json_value_t *gates = json_object_get(failure, "acknowledged_gates"), *cleaned = json_object_get(failure, "acknowledged_cleanup");
  int64_t code = report_inspect_integer(failure, "gate_return_code"), operation = report_inspect_integer(failure, "operation_code");
  int64_t observed = report_inspect_integer(failure, "observed_mono_ms");
  char key[64]; size_t phase, normal = 0, expected[2], count = 0, acknowledged, gate = 5;
  for (phase = 0; phase < 6; ++phase) if (report_inspect_is(failure, "operation_phase", phases[phase])) break;
  if (!report_inspect_shape(failure, keys, sizeof(keys)/sizeof(keys[0])) ||
      report_inspect_integer(failure, "schema_version") != 2 ||
      !report_inspect_is(failure, "semantics", "post_drain_report_cleanup_failure_observation") ||
      !report_inspect_is(failure, "report_id", id) || !report_inspect_is(failure, "parent_run_id", parent) ||
      !report_inspect_is(failure, "clock_domain", "origin_process_monotonic") || phase == 6 ||
      code < INT_MIN || code > INT_MAX || !code || operation < INT_MIN || operation > INT_MAX || !operation ||
      observed < report_inspect_integer(manifest, "started_mono_ms") || observed > INT64_C(9007199254740991) ||
      json_type(gates) != JSON_ARRAY || json_type(cleaned) != JSON_ARRAY) return SALTS_EPROTO;
  while (normal < 10 && p[normal]) ++normal;
  if (normal < min_normal[phase] || normal > max_normal[phase] || json_array_size(gates) != normal + 1) return SALTS_EPROTO;
  if (phase == 3 && normal == 9 && !report_inspect_is(p[7], "decision", "reject")) return SALTS_EPROTO;
  for (size_t i = 0; i <= normal; ++i) {
    const json_value_t *v = json_array_get(gates, i); const char *s = json_string(v);
    if (i && report_inspect_key(id, single_agent_report_gates[i], key)) return SALTS_EPROTO;
    if (!s || strlen(s) != json_string_len(v) || strcmp(s, i ? key : id)) return SALTS_EPROTO;
  }
  /* Normal gates acknowledged all preceding work. The retained segment and
   * result-transfer boundaries determine which cleanup writes can occur. */
  if (phase == 0) { if (normal) expected[count++] = 0; if (normal > 1) expected[count++] = 1; }
  else if (phase == 1 && normal == 4) expected[count++] = 2;
  else if ((phase == 2 && normal == 6) || (phase == 3 && normal >= 8) || (phase == 4 && normal == 8)) expected[count++] = 3;
  acknowledged = json_array_size(cleaned);
  if (acknowledged > count) return SALTS_EPROTO;
  for (size_t i = 0; i < acknowledged; ++i) {
    const json_value_t *v = json_array_get(cleaned, i); const char *s = json_string(v);
    if (report_inspect_key(id, single_agent_report_cleanup_gates[expected[i]], key) || !s ||
        strlen(s) != json_string_len(v) || strcmp(s, key) || !p[11 + expected[i]]) return SALTS_EPROTO;
  }
  int attempted = json_type(json_object_get(failure, "gate_id")) != JSON_NULL;
  if (attempted) {
    for (gate = 0; gate < 5; ++gate) {
      if (report_inspect_key(id, single_agent_report_cleanup_gates[gate], key)) return SALTS_EPROTO;
      if (report_inspect_is(failure, "gate_id", key)) break;
    }
    if (gate == 5 || !report_inspect_is(failure, "gate_outcome", "unresolved") ||
        (gate == 4 ? acknowledged != count : acknowledged == count || gate != expected[acknowledged])) return SALTS_EPROTO;
  } else if (!report_inspect_is(failure, "gate_outcome", "not_attempted")) return SALTS_EPROTO;
  for (size_t role = 0; role < 4; ++role) if (p[11 + role]) {
    int permitted = attempted && gate == role;
    for (size_t i = 0; i < acknowledged; ++i) if (expected[i] == role) permitted = 1;
    if (!permitted) return SALTS_EPROTO;
  }
  if (p[10] && (!attempted || gate != 4 ||
      !report_inspect_equal(p[10], "operation_phase", failure, "operation_phase") ||
      report_inspect_integer(p[10], "operation_code") != operation ||
      report_inspect_integer(p[10], "observed_mono_ms") > observed)) return SALTS_EPROTO;
  return report_inspect_size(failure, 16384);
}

static int report_inspect_gate_failure(const json_value_t *failure, json_value_t *const p[],
    const json_value_t *manifest, const char *id, const char *parent) {
  if (report_inspect_integer(failure, "schema_version") == 2)
    return report_inspect_cleanup_failure(failure, p, manifest, id, parent);
  static const char *const keys[] = {"schema_version", "semantics", "report_id", "parent_run_id",
      "operation_phase", "gate_id", "gate_return_code", "gate_outcome", "observed_mono_ms",
      "clock_domain", "acknowledged_gates"};
  static const char *const phases[] = {"manifest", "workers", "workers", "workers", "aggregation",
      "aggregation", "execution", "approval", "approval", "delivery", "delivery-audit"};
  const json_value_t *gates = json_object_get(failure, "acknowledged_gates");
  int64_t code = report_inspect_integer(failure, "gate_return_code"), observed = report_inspect_integer(failure, "observed_mono_ms");
  char key[64]; size_t gate, normal = 0;
  if (!report_inspect_shape(failure, keys, sizeof(keys)/sizeof(keys[0])) ||
      report_inspect_integer(failure, "schema_version") != 1 ||
      !report_inspect_is(failure, "semantics", "post_drain_report_gate_failure_observation") ||
      !report_inspect_is(failure, "report_id", id) || !report_inspect_is(failure, "parent_run_id", parent) ||
      !report_inspect_is(failure, "clock_domain", "origin_process_monotonic") ||
      !report_inspect_is(failure, "gate_outcome", "unresolved") || code < INT_MIN || code > INT_MAX || !code ||
      observed < report_inspect_integer(manifest, "started_mono_ms") || observed > INT64_C(9007199254740991) ||
      json_type(gates) != JSON_ARRAY) return SALTS_EPROTO;
  for (gate = 1; gate < 11; ++gate) {
    if (report_inspect_key(id, single_agent_report_gates[gate], key)) return SALTS_EPROTO;
    if (report_inspect_is(failure, "gate_id", key)) break;
  }
  if (gate == 11 || (!report_inspect_is(failure, "operation_phase", phases[gate]) &&
      !(gate == 9 && report_inspect_is(failure, "operation_phase", "approval")))) return SALTS_EPROTO;
  while (normal < 10 && p[normal]) ++normal;
  /* Only the failed gate may or may not have committed. Its presence does not
   * resolve acceptance/handoff, and no later gate or operational stop is valid. */
  if ((normal != gate - 1 && normal != gate) || json_array_size(gates) != gate) return SALTS_EPROTO;
  for (size_t i = 10; i < 15; ++i) if (p[i]) return SALTS_EPROTO;
  for (size_t i = 0; i < gate; ++i) {
    const json_value_t *v = json_array_get(gates, i); const char *s = json_string(v);
    if (i && report_inspect_key(id, single_agent_report_gates[i], key)) return SALTS_EPROTO;
    if (!s || strlen(s) != json_string_len(v) || strcmp(s, i ? key : id)) return SALTS_EPROTO;
  }
  return report_inspect_size(failure, 16384);
}

/* All stores/owners are trusted and quiescent throughout this bounded read.
 * ENOENT for an optional phase means missing evidence, not work/effect absence.
 * Any other error, missing referenced anchor or identity mismatch withholds the
 * whole owned output. Reads/allocations precede projection limits; no snapshot
 * transaction across stores is promised. No installed API is changed. */
static int single_agent_report_inspect(turbo_agent_runtime_t *host,
    const single_agent_report_binding_t *bindings, size_t count, const char *id,
    const char *parent, json_value_t **out) {
  static const char kinds[] = "iigioioro"; /* ready/stop/cleanup handled separately */
  static const int intents[] = {0, 1, 3, 5};
  static const char *const roles[] = {"worker-a", "worker-b", "aggregate", "execute"};
  static const char *const ready_keys[] = {"schema_version", "report_id", "semantics", "task_id", "execution_id",
      "runtime_run_id", "terminal_observation_id", "verification"};
  json_value_t *m = NULL, *p[REPORT_INSPECT_COUNT] = {0}, *result = NULL, *records = NULL;
  char key[64]; int rc = SALTS_EINVAL;
  if (!out) return rc;
  *out = NULL;
  if (!host || !bindings || !count || count > 32 || !single_agent_report_id_valid(id) || !parent || !parent[0] || strlen(parent) > 128) return rc;
  for (size_t i = 0; i < count; ++i) {
    if (!report_inspect_endpoint_valid(bindings[i].endpoint)) return rc;
    if (!bindings[i].runtime || bindings[i].runtime == host || (bindings[i].tool_loop != 0 && bindings[i].tool_loop != 1)) return rc;
    for (size_t j = 0; j < i; ++j) if (bindings[j].tool_loop == bindings[i].tool_loop && !strcmp(bindings[j].endpoint, bindings[i].endpoint)) return rc;
  }
  rc = turbo_agent_runtime_store_get_json_status(host, SINGLE_AGENT_REPORT_MANIFESTS, id, &m);
  if (!rc) rc = report_inspect_manifest(m, id, parent);
  if (rc) goto done;
  for (size_t i = 0; i < REPORT_INSPECT_COUNT; ++i) {
    rc = report_inspect_key(id, report_inspect_suffixes[i], key);
    if (rc) goto done;
    if (i == 15) rc = turbo_agent_runtime_store_get_json_status(host, SINGLE_AGENT_REPORT_GATE_FAILURES, key, &p[i]);
    else if (i == 10) rc = turbo_agent_runtime_store_get_json_status(host, SINGLE_AGENT_REPORT_STOPS, key, &p[i]);
    else if (i == 9) rc = turbo_agent_runtime_store_get_json_status(host, SINGLE_AGENT_REPORT_READY, key, &p[i]);
    else if (i >= 11 || kinds[i] == 'o') rc = turbo_agent_router_load_observation(host, key, &p[i]);
    else if (kinds[i] == 'i') rc = turbo_agent_router_load_intent(host, key, &p[i]);
    else if (kinds[i] == 'g') rc = turbo_agent_router_load_group_observation(host, key, &p[i]);
    else rc = turbo_agent_router_load_review_intent(host, key, &p[i]);
    if (rc == SALTS_ENOENT) { rc = SALTS_OK; continue; }
    if (rc) goto done;
    size_t phases = json_object_size(json_object_get(m, "phase_ids"));
    if ((i && i < 10 && !p[i-1]) || (i >= 10 && phases < 15) || (i == 15 && phases < 16)) { rc = SALTS_EPROTO; goto done; }
  }
  for (size_t i = 0; i < 4; ++i) {
    const json_value_t *intent = p[intents[i]];
    if (intent && (rc = report_inspect_route(intent, id, parent, roles[i], bindings, count))) goto done;
    if (p[11+i] && (rc = report_inspect_observation(p[11+i], intent, id, parent, roles[i], bindings, count))) goto done;
    if (p[11+i] && report_inspect_integer(p[11+i], "execution_status") < TURBO_AGENT_EXECUTION_COMPLETED) { rc = SALTS_EPROTO; goto done; }
  }
  if (p[2]) {
    const json_value_t *members = json_object_get(p[2], "members");
    report_inspect_key(id, "analysis", key);
    if (!report_inspect_is(p[2], "group_id", key) || !report_inspect_is(p[2], "parent_run_id", parent) ||
        !report_inspect_is(p[2], "handoff_kind", "aggregation_input") || json_array_size(members) != 2) { rc = SALTS_EPROTO; goto done; }
    for (size_t i = 0; i < 2; ++i) if ((rc = report_inspect_observation(json_array_get(members, i), p[i], id, parent, roles[i], bindings, count))) goto done;
  }
  if (p[4] && (rc = report_inspect_observation(p[4], p[3], id, parent, "aggregate", bindings, count))) goto done;
  if (p[4] && (report_inspect_integer(p[4], "execution_status") != TURBO_AGENT_EXECUTION_COMPLETED ||
      report_inspect_integer(p[4], "runtime_result_code"))) { rc = SALTS_EPROTO; goto done; }
  if (p[6] && ((rc = report_inspect_observation(p[6], p[5], id, parent, "execute", bindings, count)) || !json_get_bool(p[6], "approval_pending", 0))) { if (!rc) rc = SALTS_EPROTO; goto done; }
  if (p[7] && (!report_inspect_equal(p[7], "task_id", p[6], "task_id") ||
      !report_inspect_equal(p[7], "previous_execution_id", p[6], "execution_id") ||
      !report_inspect_equal(p[7], "source_run_id", p[6], "runtime_run_id") ||
      !report_inspect_equal(p[7], "source_checkpoint_id", p[6], "runtime_checkpoint_id") ||
      !report_inspect_equal(p[7], "admission_id", p[5], "admission_id") ||
      !report_inspect_equal(p[7], "endpoint", p[5], "endpoint") || !report_inspect_equal(p[7], "model", p[5], "model") ||
      !report_inspect_equal(p[7], "provider", p[5], "provider") || !report_inspect_equal(p[7], "thread_id", p[5], "thread_id") ||
      !report_inspect_equal(p[7], "parent_run_id", p[5], "parent_run_id") ||
      report_inspect_integer(p[7], "max_model_turns") != 2 ||
      report_inspect_integer(p[7], "review_segment") != 1)) { rc = SALTS_EPROTO; goto done; }
  if (p[6] && report_inspect_integer(p[6], "review_resumes") != 0) { rc = SALTS_EPROTO; goto done; }
  if (p[8] && ((rc = report_inspect_observation(p[8], p[5], id, parent, "execute", bindings, count)) ||
      !report_inspect_equal(p[8], "execution_id", p[7], "execution_id") ||
      report_inspect_integer(p[8], "review_resumes") != 1 || json_get_bool(p[8], "approval_pending", 0))) { if (!rc) rc = SALTS_EPROTO; goto done; }
  if (p[8] && (report_inspect_integer(p[8], "runtime_result_code") || report_inspect_integer(p[8], "execution_status") !=
      (report_inspect_is(p[7], "decision", "approve") ? TURBO_AGENT_EXECUTION_COMPLETED : TURBO_AGENT_EXECUTION_INTERRUPTED))) { rc = SALTS_EPROTO; goto done; }
  if (p[14]) {
    int64_t resumes = report_inspect_integer(p[14], "review_resumes");
    if (resumes > 1 || (resumes && (!p[7] || !report_inspect_equal(p[14], "execution_id", p[7], "execution_id")))) { rc = SALTS_EPROTO; goto done; }
  }
  if (p[9]) {
    report_inspect_key(id, "execute-final", key);
    if (!report_inspect_shape(p[9], ready_keys, sizeof(ready_keys)/sizeof(ready_keys[0])) ||
        report_inspect_integer(p[9], "schema_version") != 1 || !report_inspect_text(p[9], "report_id", 32) ||
        !report_inspect_is(p[9], "report_id", id) || !report_inspect_is(p[9], "semantics", "pre_report_delivery_observation") ||
        !report_inspect_is(p[9], "verification", "reference_identity_only") ||
        !report_inspect_is(p[7], "decision", "approve") ||
        report_inspect_integer(p[8], "execution_status") != TURBO_AGENT_EXECUTION_COMPLETED ||
        !report_inspect_is(p[8], "runtime_status", "completed") ||
        !report_inspect_is(p[9], "terminal_observation_id", key) ||
        !report_inspect_equal(p[9], "task_id", p[8], "task_id") || !report_inspect_equal(p[9], "execution_id", p[8], "execution_id") ||
        !report_inspect_equal(p[9], "runtime_run_id", p[8], "runtime_run_id")) { rc = SALTS_EPROTO; goto done; }
    if ((rc = report_inspect_size(p[9], 16384))) goto done;
  }
  if (p[10] && (rc = report_inspect_stop(p[10], p, m, id, parent))) goto done;
  if (p[15] && (rc = report_inspect_gate_failure(p[15], p, m, id, parent))) goto done;
  result = json_create_object(); records = json_create_object(); rc = SALTS_ENOMEM;
  if (!result || !records || turbo_runtime_json_object_set_int64(result, "schema_version", 1) ||
      turbo_runtime_json_object_set_string(result, "semantics", "historical_report_trace_inspection") ||
      turbo_runtime_json_object_set_clone(result, "manifest", m) || report_inspect_false(result, "delivery_proven") ||
      report_inspect_false(result, "recovery_authorized")) goto done;
  for (size_t i = 0; i < REPORT_INSPECT_COUNT; ++i) {
    json_value_t *v = p[i] ? json_clone(p[i]) : json_create_null();
    if (!v) goto done;
    if (turbo_runtime_json_object_set(records, report_inspect_suffixes[i], v)) { json_free(v); goto done; }
  }
  if (turbo_runtime_json_object_set(result, "records", records)) goto done;
  records = NULL;
  rc = report_inspect_size(result, REPORT_INSPECT_MAX_BYTES);
  if (!rc) { *out = result; result = NULL; }
done:
  json_free(records); json_free(result); json_free(m);
  for (size_t i = 0; i < REPORT_INSPECT_COUNT; ++i) json_free(p[i]);
  return rc;
}

#endif
