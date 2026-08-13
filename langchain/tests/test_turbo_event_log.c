#include "tinytest.h"
#include "turbo_chain.h"
#include "turbo_event_log.h"

#include <string.h>

typedef struct {
  int call_count;
} fake_model_t;

typedef struct {
  int count;
  int model_count;
  int tool_result_count;
} replay_capture_t;

typedef struct {
  int tool_result_count;
  const char *parent_agent_run_id;
  const char *parent_tool_call_id;
  const char *parent_tool_name;
} parent_lineage_replay_t;

static int fake_sum_tool_json_value(const json_value_t *arguments,
                              json_value_t **out_result, void *user_data) {
  const json_value_t *a;
  const json_value_t *b;
  json_value_t *result;

  (void)user_data;
  check_not_null(arguments);
  check_not_null(out_result);

  a = turbo_json_object_get(arguments, "a");
  b = turbo_json_object_get(arguments, "b");
  result = turbo_json_create_int64(
      turbo_runtime_json_value_as_int64(a, 0) +
      turbo_runtime_json_value_as_int64(b, 0));
  check_not_null(result);

  *out_result = result;
  return TURBO_TOOL_OK;
}

static int fake_parent_lineage_tool_json_value(const json_value_t *arguments,
                                         json_value_t **out_result,
                                         void *user_data) {
  json_value_t *result;

  (void)arguments;
  (void)user_data;
  check_not_null(out_result);

  result = turbo_json_create_object();
  check_not_null(result);
  check_int_eq(
      turbo_runtime_json_object_set(result, "ok",
                                         turbo_json_create_bool(1)),
      TURBO_RUNTIME_JSON_OK);
  check_int_eq(
      turbo_runtime_json_object_set(result, "summary",
                                         turbo_json_create_string("ok")),
      TURBO_RUNTIME_JSON_OK);
  check_int_eq(
      turbo_runtime_json_object_set(result, "stdout",
                                         turbo_json_create_string("")),
      TURBO_RUNTIME_JSON_OK);
  check_int_eq(
      turbo_runtime_json_object_set(result, "stderr",
                                         turbo_json_create_string("")),
      TURBO_RUNTIME_JSON_OK);
  check_int_eq(
      turbo_runtime_json_object_set(
          result, "parent_agent_run_id",
          turbo_json_create_string("run_parent")),
      TURBO_RUNTIME_JSON_OK);
  check_int_eq(
      turbo_runtime_json_object_set(
          result, "parent_tool_call_id",
          turbo_json_create_string("call_parent")),
      TURBO_RUNTIME_JSON_OK);
  check_int_eq(
      turbo_runtime_json_object_set(
          result, "parent_tool_name",
          turbo_json_create_string("delegate")),
      TURBO_RUNTIME_JSON_OK);

  *out_result = result;
  return TURBO_TOOL_OK;
}

static int fake_parent_lineage_model_invoke_json_value(
    void *user_data, const json_value_t *messages,
    const turbo_tool_registry_t *tools, turbo_model_json_value_result_t *out_result) {
  fake_model_t *model = (fake_model_t *)user_data;
  json_value_t *args;

  check_not_null(model);
  check_not_null(messages);
  check_not_null(tools);
  check_not_null(out_result);

  model->call_count++;
  if (model->call_count == 1) {
    args = turbo_json_create_object();
    check_not_null(args);
    check_int_eq(
        turbo_runtime_json_object_set(args, "input",
                                           turbo_json_create_string(
                                               "delegate")),
        TURBO_RUNTIME_JSON_OK);
    out_result->output_text = "Calling delegate";
    out_result->tool_name = "delegate";
    out_result->tool_arguments = args;
    return 0;
  }

  out_result->output_text = "done";
  out_result->tool_name = NULL;
  out_result->tool_arguments = NULL;
  return 0;
}

static int fake_model_invoke_json_value(void *user_data, const json_value_t *messages,
                                  const turbo_tool_registry_t *tools,
                                  turbo_model_json_value_result_t *out_result) {
  fake_model_t *model = (fake_model_t *)user_data;
  json_value_t *args;
  json_value_t *a;
  json_value_t *b;

  check_not_null(model);
  check_not_null(messages);
  check_not_null(tools);
  check_not_null(out_result);

  model->call_count++;
  if (model->call_count == 1) {
    args = turbo_json_create_object();
    a = turbo_json_create_int64(41);
    b = turbo_json_create_int64(1);
    check_not_null(args);
    check_not_null(a);
    check_not_null(b);
    check_int_eq(turbo_runtime_json_object_set(args, "a", a), TURBO_RUNTIME_JSON_OK);
    check_int_eq(turbo_runtime_json_object_set(args, "b", b), TURBO_RUNTIME_JSON_OK);
    out_result->output_text = "Calling sum";
    out_result->tool_name = "sum";
    out_result->tool_arguments = args;
    return 0;
  }

  out_result->output_text = "42";
  out_result->tool_name = NULL;
  out_result->tool_arguments = NULL;
  return 0;
}

static void capture_replayed_event(const json_value_t *event, void *user_data) {
  replay_capture_t *capture = (replay_capture_t *)user_data;
  const char *kind;

  check_not_null(event);
  check_not_null(capture);
  kind = turbo_event_kind_json_value(event);
  check_not_null(kind);

  capture->count++;
  if (strcmp(kind, "model") == 0) {
    capture->model_count++;
  } else if (strcmp(kind, "tool_result") == 0) {
    capture->tool_result_count++;
  }
}

static void capture_parent_lineage_replayed_event(const json_value_t *event,
                                                  void *user_data) {
  parent_lineage_replay_t *capture = (parent_lineage_replay_t *)user_data;
  const char *kind;

  check_not_null(event);
  check_not_null(capture);
  kind = turbo_event_kind_json_value(event);
  check_not_null(kind);
  if (strcmp(kind, "tool_result") != 0) {
    return;
  }

  capture->tool_result_count++;
  capture->parent_agent_run_id = turbo_runtime_json_value_as_string(
      turbo_json_object_get(event, "parent_agent_run_id"));
  capture->parent_tool_call_id = turbo_runtime_json_value_as_string(
      turbo_json_object_get(event, "parent_tool_call_id"));
  capture->parent_tool_name = turbo_runtime_json_value_as_string(
      turbo_json_object_get(event, "parent_tool_name"));
}

spec("turbo event log runtime") {

  describe("canonical event capture") {

    it("should capture streamed canonical events and replay them") {
      turbo_chain_t *chain = turbo_chain_create("event-log-chain");
      turbo_tool_registry_t *tools = turbo_tool_registry_create();
      json_value_t *state = turbo_chain_state_create_json_value();
      json_value_t *result = NULL;
      turbo_event_log_t *log = turbo_event_log_create();
      json_value_t *events = NULL;
      replay_capture_t replay = {0};
      fake_model_t model_state = {0};
      turbo_model_t model = {
          .name = "fake",
          .invoke = NULL,
          .invoke_json_value = fake_model_invoke_json_value,
          .user_data = &model_state,
          .user_data_free = NULL,
      };
      turbo_tool_definition_t sum_tool = {
          .name = "sum",
          .description = "add two numbers",
          .parameters_json = "{\"type\":\"object\"}",
          .strict = 0,
          .handler = NULL,
          .json_value_handler = fake_sum_tool_json_value,
          .user_data = NULL,
          .user_data_free = NULL,
      };

      check_not_null(chain);
      check_not_null(tools);
      check_not_null(state);
      check_not_null(log);

      check_int_eq(turbo_tool_registry_add(tools, &sum_tool), TURBO_TOOL_OK);
      check_int_eq(turbo_chain_add_prompt_step(chain, "user_prompt", "user", "Add 41 and 1"),
                   TURBO_CHAIN_OK);
      check_int_eq(turbo_chain_add_model_step(chain, "planner", &model, tools), TURBO_CHAIN_OK);
      check_int_eq(turbo_chain_add_tool_step(chain, "tool_exec", tools), TURBO_CHAIN_OK);
      check_int_eq(turbo_chain_add_model_step(chain, "finalizer", &model, tools), TURBO_CHAIN_OK);

      check_int_eq(
          turbo_chain_run_json_value_stream(chain, state, turbo_event_log_capture_json_value, log, &result),
          TURBO_CHAIN_OK);
      check_not_null(result);
      check_int_eq(turbo_event_log_status(log), TURBO_EVENT_LOG_OK);
      check_size_eq(turbo_event_log_size(log), 3);
      check_str_eq(turbo_event_kind_json_value(turbo_event_log_get(log, 0)), "model");
      check_str_eq(turbo_event_kind_json_value(turbo_event_log_get(log, 1)), "tool_result");
      check_str_eq(turbo_event_kind_json_value(turbo_event_log_get(log, 2)), "model");

      events = turbo_event_log_events_json_value(log);
      check_not_null(events);
      check_size_eq(turbo_runtime_json_value_size(events), 3);
      check_str_eq(turbo_event_kind_json_value(turbo_json_array_get(events, 1)),
                   "tool_result");

      check_int_eq(turbo_event_log_replay_json_value(log, capture_replayed_event, &replay),
                   TURBO_EVENT_LOG_OK);
      check_int_eq(replay.count, 3);
      check_int_eq(replay.model_count, 2);
      check_int_eq(replay.tool_result_count, 1);

      turbo_runtime_json_destroy(events);
      turbo_runtime_json_destroy(result);
      turbo_event_log_destroy(log);
      turbo_runtime_json_destroy(state);
      turbo_tool_registry_destroy(tools);
      turbo_chain_destroy(chain);
    }

    it("should reject non-canonical events") {
      turbo_event_log_t *log = turbo_event_log_create();
      json_value_t *event = turbo_json_create_object();

      check_not_null(log);
      check_not_null(event);
      check_int_eq(turbo_event_log_append_json_value(log, event), TURBO_EVENT_LOG_INVALID_EVENT);
      check_int_eq(turbo_event_log_status(log), TURBO_EVENT_LOG_INVALID_EVENT);
      check_size_eq(turbo_event_log_size(log), 0);

      turbo_runtime_json_destroy(event);
      turbo_event_log_destroy(log);
    }

    it("should reset captured canonical events") {
      turbo_event_log_t *log = turbo_event_log_create();
      json_value_t *event =
          turbo_event_trace_create_json_value("trace.test", "finish", "ok", 0);

      check_not_null(log);
      check_not_null(event);
      check_int_eq(turbo_event_log_append_json_value(log, event), TURBO_EVENT_LOG_OK);
      check_size_eq(turbo_event_log_size(log), 1);
      check_int_eq(turbo_event_log_reset(log), TURBO_EVENT_LOG_OK);
      check_int_eq(turbo_event_log_status(log), TURBO_EVENT_LOG_OK);
      check_size_eq(turbo_event_log_size(log), 0);
      check_null(turbo_event_log_get(log, 0));

      turbo_runtime_json_destroy(event);
      turbo_event_log_destroy(log);
    }

    it("should import canonical event arrays") {
      turbo_event_log_t *log = turbo_event_log_create();
      json_value_t *events = turbo_json_create_array();

      check_not_null(log);
      check_not_null(events);
      check_int_eq(
          turbo_runtime_json_array_append(
              events, turbo_event_trace_create_json_value("trace.a", "start", "x", 0)),
          TURBO_RUNTIME_JSON_OK);
      check_int_eq(
          turbo_runtime_json_array_append(
              events, turbo_event_trace_create_json_value("trace.a", "finish", "x", 0)),
          TURBO_RUNTIME_JSON_OK);

      check_int_eq(turbo_event_log_load_events_json_value(log, events), TURBO_EVENT_LOG_OK);
      check_size_eq(turbo_event_log_size(log), 2);
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(turbo_event_log_get(log, 0), "detail")),
                   "start");
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(turbo_event_log_get(log, 1), "detail")),
                   "finish");

      turbo_runtime_json_destroy(events);
      turbo_event_log_destroy(log);
    }

    it("should preserve parent lineage on canonical tool result events during replay") {
      turbo_chain_t *chain = turbo_chain_create("event-log-parent-lineage");
      turbo_tool_registry_t *tools = turbo_tool_registry_create();
      json_value_t *state = turbo_chain_state_create_json_value();
      json_value_t *result = NULL;
      turbo_event_log_t *log = turbo_event_log_create();
      fake_model_t model_state = {0};
      parent_lineage_replay_t replay = {0};
      turbo_model_t model = {
          .name = "fake-parent",
          .invoke = NULL,
          .invoke_json_value = fake_parent_lineage_model_invoke_json_value,
          .user_data = &model_state,
          .user_data_free = NULL,
      };
      turbo_tool_definition_t delegate_tool = {
          .name = "delegate",
          .description = "return parent lineage payload",
          .parameters_json = "{\"type\":\"object\"}",
          .strict = 0,
          .handler = NULL,
          .json_value_handler = fake_parent_lineage_tool_json_value,
          .user_data = NULL,
          .user_data_free = NULL,
      };
      const json_value_t *tool_event;

      check_not_null(chain);
      check_not_null(tools);
      check_not_null(state);
      check_not_null(log);

      check_int_eq(turbo_tool_registry_add(tools, &delegate_tool), TURBO_TOOL_OK);
      check_int_eq(turbo_chain_add_prompt_step(chain, "user_prompt", "user", "delegate"),
                   TURBO_CHAIN_OK);
      check_int_eq(turbo_chain_add_model_step(chain, "planner", &model, tools), TURBO_CHAIN_OK);
      check_int_eq(turbo_chain_add_tool_step(chain, "tool_exec", tools), TURBO_CHAIN_OK);
      check_int_eq(turbo_chain_add_model_step(chain, "finalizer", &model, tools), TURBO_CHAIN_OK);

      check_int_eq(
          turbo_chain_run_json_value_stream(chain, state, turbo_event_log_capture_json_value, log, &result),
          TURBO_CHAIN_OK);
      check_not_null(result);
      check_size_eq(turbo_event_log_size(log), 3);
      tool_event = turbo_event_log_get(log, 1);
      check_str_eq(turbo_event_kind_json_value(tool_event), "tool_result");
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(tool_event, "parent_agent_run_id")),
                   "run_parent");
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(tool_event, "parent_tool_call_id")),
                   "call_parent");
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(tool_event, "parent_tool_name")),
                   "delegate");
      check_int_eq(
          turbo_event_log_replay_json_value(log, capture_parent_lineage_replayed_event, &replay),
          TURBO_EVENT_LOG_OK);
      check_int_eq(replay.tool_result_count, 1);
      check_str_eq(replay.parent_agent_run_id, "run_parent");
      check_str_eq(replay.parent_tool_call_id, "call_parent");
      check_str_eq(replay.parent_tool_name, "delegate");

      turbo_runtime_json_destroy(result);
      turbo_event_log_destroy(log);
      turbo_runtime_json_destroy(state);
      turbo_tool_registry_destroy(tools);
      turbo_chain_destroy(chain);
    }
  }
}
