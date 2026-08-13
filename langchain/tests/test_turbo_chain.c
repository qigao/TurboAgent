#include "tinytest.h"
#include "turbo_chain.h"
#include "turbo_event_log.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
  int call_count;
} fake_model_t;

typedef struct {
  int count;
  int model_count;
  int tool_result_count;
  char *last_kind;
  char *last_output_text;
  char *last_tool_name;
} chain_event_capture_t;

static void count_model_user_data_free(void *user_data) {
  int *count = (int *)user_data;

  check_not_null(count);
  (*count)++;
}

static int custom_json_value_append_message(turbo_chain_exec_ctx_t *ctx, void *user_data) {
  (void)user_data;

  check_not_null(ctx);
  check_not_null(ctx->json_value_state);
  return turbo_prompt_messages_append_json_value(
             (json_value_t *)turbo_json_object_get(ctx->json_value_state,
                                                                                   "messages"),
             "assistant", "custom bind") == TURBO_PROMPT_OK
             ? TURBO_CHAIN_OK
             : TURBO_CHAIN_OUT_OF_MEMORY;
}

static char *test_strdup(const char *text) {
  size_t len;
  char *copy;

  if (!text) {
    return NULL;
  }

  len = strlen(text) + 1;
  copy = (char *)malloc(len);
  if (!copy) {
    return NULL;
  }

  memcpy(copy, text, len);
  return copy;
}

static void capture_chain_event(const json_value_t *event, void *user_data) {
  chain_event_capture_t *capture = (chain_event_capture_t *)user_data;
  const char *kind;

  check_not_null(event);
  check_not_null(capture);
  kind = turbo_runtime_json_value_as_string(turbo_json_object_get(event, "kind"));
  capture->count++;
  free(capture->last_kind);
  free(capture->last_output_text);
  free(capture->last_tool_name);
  capture->last_kind = test_strdup(kind);
  capture->last_output_text = NULL;
  capture->last_tool_name = NULL;

  if (kind && strcmp(kind, "model") == 0) {
    capture->model_count++;
    capture->last_output_text = test_strdup(turbo_runtime_json_value_as_string(
        turbo_json_object_get(event, "output_text")));
  } else if (kind && strcmp(kind, "tool_result") == 0) {
    capture->tool_result_count++;
    capture->last_tool_name = test_strdup(turbo_runtime_json_value_as_string(
        turbo_json_object_get(event, "name")));
    capture->last_output_text = test_strdup(turbo_runtime_json_value_as_string(
        turbo_json_object_get(event, "output")));
  }
}

static int fake_sum_tool(const char *arguments_json, char **out_output, void *user_data) {
  (void)user_data;
  (void)arguments_json;

  if (!out_output) {
    return TURBO_TOOL_INVALID_ARGUMENT;
  }

  *out_output = test_strdup("42");
  return *out_output ? TURBO_TOOL_OK : TURBO_TOOL_OUT_OF_MEMORY;
}

static int fake_sum_tool_json_value(const json_value_t *arguments,
                              json_value_t **out_result, void *user_data) {
  const json_value_t *a;
  const json_value_t *b;
  json_value_t *result;

  (void)user_data;
  if (!arguments || !out_result) {
    return TURBO_TOOL_INVALID_ARGUMENT;
  }

  a = turbo_json_object_get(arguments, "a");
  b = turbo_json_object_get(arguments, "b");
  result = turbo_json_create_int64(
      turbo_runtime_json_value_as_int64(a, 0) +
      turbo_runtime_json_value_as_int64(b, 0));
  if (!result) {
    return TURBO_TOOL_OUT_OF_MEMORY;
  }

  *out_result = result;
  return TURBO_TOOL_OK;
}

static int fake_child_lineage_tool_json_value(const json_value_t *arguments,
                                        json_value_t **out_result,
                                        void *user_data) {
  json_value_t *result;

  (void)arguments;
  (void)user_data;
  if (!out_result) {
    return TURBO_TOOL_INVALID_ARGUMENT;
  }

  result = turbo_json_create_object();
  if (!result) {
    return TURBO_TOOL_OUT_OF_MEMORY;
  }
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
          result, "child_thread_id",
          turbo_json_create_string("thr_child")),
      TURBO_RUNTIME_JSON_OK);
  check_int_eq(
      turbo_runtime_json_object_set(result, "child_run_id",
                                         turbo_json_create_string(
                                             "run_child")),
      TURBO_RUNTIME_JSON_OK);
  check_int_eq(
      turbo_runtime_json_object_set(result, "child_checkpoint_id",
                                         turbo_json_create_null()),
      TURBO_RUNTIME_JSON_OK);
  check_int_eq(
      turbo_runtime_json_object_set(
          result, "child_status",
          turbo_json_create_string("completed")),
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

static int fake_model_invoke(void *user_data, const json_value_t *messages,
                             const turbo_tool_registry_t *tools,
                             turbo_model_result_t *out_result) {
  fake_model_t *model = (fake_model_t *)user_data;
  json_value_t *first;
  json_value_t *last;

  check_not_null(model);
  check_not_null(messages);
  check_not_null(out_result);
  check_not_null(tools);

  model->call_count++;
  first = turbo_json_array_get(messages, 0);
  last = turbo_json_array_get(messages, turbo_json_array_size(messages) - 1);

  if (model->call_count == 1) {
    check_size_eq(turbo_json_array_size(messages), 1);
    check_str_eq(turbo_json_get_string(first, "role"), "user");
    check_str_eq(turbo_json_get_string(first, "content"), "Add 41 and 1");
    out_result->output_text = "Calling sum";
    out_result->tool_name = "sum";
    out_result->tool_arguments_json = "{\"a\":41,\"b\":1}";
    return 0;
  }

  check_size_eq(turbo_json_array_size(messages), 3);
  check_str_eq(turbo_json_get_string(last, "role"), "tool");
  check_str_eq(turbo_json_get_string(last, "content"), "42");
  out_result->output_text = "42";
  out_result->tool_name = NULL;
  out_result->tool_arguments_json = NULL;
  return 0;
}

static int fake_model_invoke_json_value(void *user_data, const json_value_t *messages,
                                  const turbo_tool_registry_t *tools,
                                  turbo_model_json_value_result_t *out_result) {
  fake_model_t *model = (fake_model_t *)user_data;
  const json_value_t *first;
  const json_value_t *last;
  json_value_t *args;
  json_value_t *a;
  json_value_t *b;

  check_not_null(model);
  check_not_null(messages);
  check_not_null(out_result);
  check_not_null(tools);

  model->call_count++;
  first = turbo_json_array_get(messages, 0);
  last = turbo_json_array_get(messages, turbo_runtime_json_value_size(messages) - 1);

  if (model->call_count == 1) {
    check_size_eq(turbo_runtime_json_value_size(messages), 1);
    check_str_eq(turbo_runtime_json_value_as_string(
                     turbo_json_object_get(first, "role")),
                 "user");
    check_str_eq(turbo_runtime_json_value_as_string(
                     turbo_json_object_get(first, "content")),
                 "Add 41 and 1");

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

  check_size_eq(turbo_runtime_json_value_size(messages), 3);
  check_str_eq(turbo_runtime_json_value_as_string(
                   turbo_json_object_get(last, "role")),
               "tool");
  check_str_eq(turbo_runtime_json_value_as_string(
                   turbo_json_object_get(last, "content")),
               "42");
  out_result->output_text = "42";
  out_result->tool_name = NULL;
  out_result->tool_arguments = NULL;
  return 0;
}

static int fake_model_invoke_json_value_child_lineage(
    void *user_data, const json_value_t *messages,
    const turbo_tool_registry_t *tools, turbo_model_json_value_result_t *out_result) {
  fake_model_t *model = (fake_model_t *)user_data;
  json_value_t *args;
  const json_value_t *last;
  const char *last_role;
  const char *last_content;

  check_not_null(model);
  check_not_null(messages);
  check_not_null(out_result);
  check_not_null(tools);

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

  last = turbo_json_array_get(messages, turbo_runtime_json_value_size(messages) - 1);
  check_not_null(last);
  last_role = turbo_runtime_json_value_as_string(
      turbo_json_object_get(last, "role"));
  last_content = turbo_runtime_json_value_as_string(
      turbo_json_object_get(last, "content"));
  check_str_eq(last_role, "tool");
  check_true(last_content != NULL);
  check_true(strstr(last_content, "child_run_id") != NULL);
  out_result->output_text = "done";
  out_result->tool_name = NULL;
  out_result->tool_arguments = NULL;
  return 0;
}

spec("turbo chain runtime") {

  it("should leave model adapter user data owned by the caller") {
    turbo_chain_t *chain = turbo_chain_create("model-ownership");
    int free_count = 0;
    turbo_model_t model = {
        .name = "fake",
        .invoke = fake_model_invoke,
        .invoke_json_value = NULL,
        .user_data = &free_count,
        .user_data_free = count_model_user_data_free,
    };

    check_not_null(chain);
    check_int_eq(turbo_chain_add_model_step(chain, "first", &model, NULL), TURBO_CHAIN_OK);
    check_int_eq(turbo_chain_add_model_step(chain, "second", &model, NULL), TURBO_CHAIN_OK);

    turbo_chain_destroy(chain);
    check_int_eq(free_count, 0);
  }

  describe("prompt rendering") {

    it("should render input variables into messages") {
      turbo_chain_t *chain = turbo_chain_create("prompt");
      json_value_t *state = turbo_chain_state_create();
      json_value_t *messages;
      json_value_t *msg;

      check_not_null(chain);
      check_not_null(state);
      check_int_eq(turbo_chain_state_input_set_string(state, "task", "read README"),
                   TURBO_CHAIN_OK);

      check_int_eq(turbo_chain_add_prompt_step(chain, "user_prompt", "user",
                                               "Please {{task}} now."),
                   TURBO_CHAIN_OK);
      check_int_eq(turbo_chain_run(chain, state), TURBO_CHAIN_OK);

      messages = turbo_chain_state_messages(state);
      check_size_eq(turbo_json_array_size(messages), 1);
      msg = turbo_json_array_get(messages, 0);
      check_str_eq(turbo_json_get_string(msg, "role"), "user");
      check_str_eq(turbo_json_get_string(msg, "content"), "Please read README now.");

      turbo_free_json(&state);
      turbo_chain_destroy(chain);
    }

    it("should run through a TurboParser JSON state boundary") {
      turbo_chain_t *chain = turbo_chain_create("prompt-bind");
      json_value_t *state = turbo_chain_state_create_json_value();
      json_value_t *input = NULL;
      json_value_t *task = NULL;
      json_value_t *result = NULL;
      const json_value_t *messages;
      const json_value_t *message;

      check_not_null(chain);
      check_not_null(state);

      input = turbo_json_object_get(state, "input");
      check_not_null(input);
      task = turbo_json_create_string("read README");
      check_not_null(task);
      check_int_eq(turbo_runtime_json_object_set((json_value_t *)input,
                                                      "task", task),
                   TURBO_RUNTIME_JSON_OK);

      check_int_eq(turbo_chain_add_prompt_step(chain, "user_prompt", "user",
                                               "Please {{task}} now."),
                   TURBO_CHAIN_OK);
      check_int_eq(turbo_chain_run_json_value(chain, state, &result), TURBO_CHAIN_OK);

      messages = turbo_json_object_get(result, "messages");
      check_not_null(messages);
      check_size_eq(turbo_runtime_json_value_size(messages), 1);
      message = turbo_json_array_get(messages, 0);
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(message, "role")),
                   "user");
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(message, "content")),
                   "Please read README now.");

      turbo_runtime_json_destroy(result);
      turbo_runtime_json_destroy(state);
      turbo_chain_destroy(chain);
    }
  }

  describe("model and tool orchestration") {

    it("should execute a linear prompt-model-tool-model chain") {
      turbo_chain_t *chain = turbo_chain_create("reactish");
      turbo_tool_registry_t *tools = turbo_tool_registry_create();
      fake_model_t model_state = {0};
      turbo_model_t model = {
          .name = "fake",
          .invoke = fake_model_invoke,
          .invoke_json_value = NULL,
          .user_data = &model_state,
          .user_data_free = NULL,
      };
      turbo_tool_definition_t sum_tool = {
          .name = "sum",
          .description = "add two numbers",
          .parameters_json = "{\"type\":\"object\"}",
          .strict = 0,
          .handler = fake_sum_tool,
          .json_value_handler = NULL,
          .user_data = NULL,
          .user_data_free = NULL,
      };
      json_value_t *state = turbo_chain_state_create();
      json_value_t *messages;
      json_value_t *tool_results;

      check_not_null(chain);
      check_not_null(tools);
      check_not_null(state);
      check_int_eq(turbo_chain_state_input_set_string(state, "a", "41"), TURBO_CHAIN_OK);
      check_int_eq(turbo_chain_state_input_set_string(state, "b", "1"), TURBO_CHAIN_OK);

      check_int_eq(turbo_tool_registry_add(tools, &sum_tool), TURBO_TOOL_OK);
      check_int_eq(turbo_chain_add_prompt_step(chain, "user_prompt", "user",
                                               "Add {{a}} and {{b}}"),
                   TURBO_CHAIN_OK);
      check_int_eq(turbo_chain_add_model_step(chain, "planner", &model, tools),
                   TURBO_CHAIN_OK);
      check_int_eq(turbo_chain_add_tool_step(chain, "tool_exec", tools), TURBO_CHAIN_OK);
      check_int_eq(turbo_chain_add_model_step(chain, "finalizer", &model, tools),
                   TURBO_CHAIN_OK);

      check_int_eq(turbo_chain_run(chain, state), TURBO_CHAIN_OK);

      check_int_eq(model_state.call_count, 2);
      check_str_eq(turbo_chain_state_last_output_text(state), "42");
      check_ptr_eq(turbo_chain_state_pending_tool_name(state), NULL);

      messages = turbo_chain_state_messages(state);
      check_size_eq(turbo_json_array_size(messages), 4);
      check_str_eq(turbo_json_get_string(turbo_json_array_get(messages, 1), "content"),
                   "Calling sum");
      check_str_eq(turbo_json_get_string(turbo_json_array_get(messages, 2), "content"), "42");
      check_str_eq(turbo_json_get_string(turbo_json_array_get(messages, 3), "content"), "42");

      tool_results = turbo_chain_state_tool_results(state);
      check_not_null(tool_results);
      check_size_eq(turbo_json_array_size(tool_results), 1);
      check_str_eq(turbo_json_get_string(turbo_json_array_get(tool_results, 0), "name"), "sum");

      turbo_free_json(&state);
      turbo_tool_registry_destroy(tools);
      turbo_chain_destroy(chain);
    }

    it("should execute a TurboParser JSON-native model and tool chain") {
      turbo_chain_t *chain = turbo_chain_create("reactish-bind");
      turbo_tool_registry_t *tools = turbo_tool_registry_create();
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
      json_value_t *state = turbo_chain_state_create();
      json_value_t *tool_requests;
      json_value_t *tool_results;
      const json_value_t *arguments;
      const json_value_t *output_value;

      check_not_null(chain);
      check_not_null(tools);
      check_not_null(state);
      check_int_eq(turbo_chain_state_input_set_string(state, "a", "41"), TURBO_CHAIN_OK);
      check_int_eq(turbo_chain_state_input_set_string(state, "b", "1"), TURBO_CHAIN_OK);

      check_int_eq(turbo_tool_registry_add(tools, &sum_tool), TURBO_TOOL_OK);
      check_int_eq(turbo_chain_add_prompt_step(chain, "user_prompt", "user",
                                               "Add {{a}} and {{b}}"),
                   TURBO_CHAIN_OK);
      check_int_eq(turbo_chain_add_model_step(chain, "planner", &model, tools),
                   TURBO_CHAIN_OK);
      check_int_eq(turbo_chain_add_tool_step(chain, "tool_exec", tools), TURBO_CHAIN_OK);
      check_int_eq(turbo_chain_add_model_step(chain, "finalizer", &model, tools),
                   TURBO_CHAIN_OK);

      check_int_eq(turbo_chain_run(chain, state), TURBO_CHAIN_OK);
      check_int_eq(model_state.call_count, 2);

      tool_requests = turbo_chain_state_tool_requests(state);
      check_size_eq(turbo_json_array_size(tool_requests), 1);
      arguments = turbo_json_object_get(turbo_json_array_get(tool_requests, 0), "arguments");
      check_not_null(arguments);
      check_int_eq((int)turbo_json_get_double(arguments, "a", 0), 41);
      check_int_eq((int)turbo_json_get_double(arguments, "b", 0), 1);

      tool_results = turbo_chain_state_tool_results(state);
      check_size_eq(turbo_json_array_size(tool_results), 1);
      output_value = turbo_json_object_get(turbo_json_array_get(tool_results, 0), "output_value");
      check_not_null(output_value);
      check_int_eq((int)turbo_json_number(output_value), 42);
      check_str_eq(turbo_json_get_string(turbo_json_array_get(turbo_chain_state_messages(state), 2),
                                         "content"),
                   "42");

      turbo_free_json(&state);
      turbo_tool_registry_destroy(tools);
      turbo_chain_destroy(chain);
    }

    it("should execute TurboParser JSON-native state without json round-trip state storage") {
      turbo_chain_t *chain = turbo_chain_create("reactish-bind-state");
      turbo_tool_registry_t *tools = turbo_tool_registry_create();
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
      json_value_t *state = turbo_chain_state_create_json_value();
      json_value_t *result = NULL;
      json_value_t *a_value = turbo_json_create_string("41");
      json_value_t *b_value = turbo_json_create_string("1");
      const json_value_t *input;
      const json_value_t *tool_requests;
      const json_value_t *tool_results;
      const json_value_t *messages;
      const json_value_t *tool_request;
      const json_value_t *tool_result;

      check_not_null(chain);
      check_not_null(tools);
      check_not_null(state);
      check_not_null(a_value);
      check_not_null(b_value);

      input = turbo_json_object_get(state, "input");
      check_not_null(input);
      check_int_eq(turbo_runtime_json_object_set((json_value_t *)input, "a",
                                                      a_value),
                   TURBO_RUNTIME_JSON_OK);
      check_int_eq(turbo_runtime_json_object_set((json_value_t *)input, "b",
                                                      b_value),
                   TURBO_RUNTIME_JSON_OK);

      check_int_eq(turbo_tool_registry_add(tools, &sum_tool), TURBO_TOOL_OK);
      check_int_eq(turbo_chain_add_prompt_step(chain, "user_prompt", "user",
                                               "Add {{a}} and {{b}}"),
                   TURBO_CHAIN_OK);
      check_int_eq(turbo_chain_add_model_step(chain, "planner", &model, tools),
                   TURBO_CHAIN_OK);
      check_int_eq(turbo_chain_add_tool_step(chain, "tool_exec", tools), TURBO_CHAIN_OK);
      check_int_eq(turbo_chain_add_model_step(chain, "finalizer", &model, tools),
                   TURBO_CHAIN_OK);

      check_int_eq(turbo_chain_run_json_value(chain, state, &result), TURBO_CHAIN_OK);
      check_int_eq(model_state.call_count, 2);

      tool_requests = turbo_json_object_get(result, "tool_requests");
      check_not_null(tool_requests);
      check_size_eq(turbo_runtime_json_value_size(tool_requests), 1);
      tool_request = turbo_json_array_get(tool_requests, 0);
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(tool_request, "name")),
                   "sum");
      check_int_eq((int)turbo_runtime_json_value_as_int64(
                       turbo_json_object_get(
                           turbo_json_object_get(tool_request, "arguments"), "a"),
                       0),
                   41);

      tool_results = turbo_json_object_get(result, "tool_results");
      check_not_null(tool_results);
      check_size_eq(turbo_runtime_json_value_size(tool_results), 1);
      tool_result = turbo_json_array_get(tool_results, 0);
      check_int_eq((int)turbo_runtime_json_value_as_int64(
                       turbo_json_object_get(tool_result, "output_value"), 0),
                   42);

      messages = turbo_json_object_get(result, "messages");
      check_not_null(messages);
      check_size_eq(turbo_runtime_json_value_size(messages), 4);
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(
                           turbo_json_array_get(messages, 2), "content")),
                   "42");

      turbo_runtime_json_destroy(result);
      turbo_runtime_json_destroy(state);
      turbo_tool_registry_destroy(tools);
      turbo_chain_destroy(chain);
    }

    it("should run a TurboParser JSON-native custom step without json bridge") {
      turbo_chain_t *chain = turbo_chain_create("custom-bind");
      json_value_t *state = turbo_chain_state_create_json_value();
      json_value_t *result = NULL;
      const json_value_t *messages;

      check_not_null(chain);
      check_not_null(state);
      check_int_eq(turbo_chain_add_json_value_step(chain, "custom", custom_json_value_append_message, NULL, NULL),
                   TURBO_CHAIN_OK);

      check_int_eq(turbo_chain_run_json_value(chain, state, &result), TURBO_CHAIN_OK);
      messages = turbo_json_object_get(result, "messages");
      check_not_null(messages);
      check_size_eq(turbo_runtime_json_value_size(messages), 1);
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(
                           turbo_json_array_get(messages, 0), "content")),
                   "custom bind");

      turbo_runtime_json_destroy(result);
      turbo_runtime_json_destroy(state);
      turbo_chain_destroy(chain);
    }

    it("should emit canonical model and tool result events while running a TurboParser JSON-native chain") {
      turbo_chain_t *chain = turbo_chain_create("reactish-bind-stream");
      turbo_tool_registry_t *tools = turbo_tool_registry_create();
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
      json_value_t *state = turbo_chain_state_create_json_value();
      json_value_t *result = NULL;
      const json_value_t *input;
      chain_event_capture_t capture = {0};

      check_not_null(chain);
      check_not_null(tools);
      check_not_null(state);

      input = turbo_json_object_get(state, "input");
      check_not_null(input);
      check_int_eq(turbo_runtime_json_object_set((json_value_t *)input, "a",
                                                      turbo_json_create_string("41")),
                   TURBO_RUNTIME_JSON_OK);
      check_int_eq(turbo_runtime_json_object_set((json_value_t *)input, "b",
                                                      turbo_json_create_string("1")),
                   TURBO_RUNTIME_JSON_OK);

      check_int_eq(turbo_tool_registry_add(tools, &sum_tool), TURBO_TOOL_OK);
      check_int_eq(turbo_chain_add_prompt_step(chain, "user_prompt", "user",
                                               "Add {{a}} and {{b}}"),
                   TURBO_CHAIN_OK);
      check_int_eq(turbo_chain_add_model_step(chain, "planner", &model, tools), TURBO_CHAIN_OK);
      check_int_eq(turbo_chain_add_tool_step(chain, "tool_exec", tools), TURBO_CHAIN_OK);
      check_int_eq(turbo_chain_add_model_step(chain, "finalizer", &model, tools), TURBO_CHAIN_OK);

      check_int_eq(turbo_chain_run_json_value_stream(chain, state, capture_chain_event, &capture, &result),
                   TURBO_CHAIN_OK);
      check_not_null(result);
      check_int_eq(model_state.call_count, 2);
      check_int_eq(capture.count, 3);
      check_int_eq(capture.model_count, 2);
      check_int_eq(capture.tool_result_count, 1);
      check_str_eq(capture.last_kind, "model");
      check_str_eq(capture.last_output_text, "42");

      free(capture.last_tool_name);
      free(capture.last_output_text);
      free(capture.last_kind);
      turbo_runtime_json_destroy(result);
      turbo_runtime_json_destroy(state);
      turbo_tool_registry_destroy(tools);
      turbo_chain_destroy(chain);
    }

    it("should capture canonical events into an event log while running a TurboParser JSON-native chain") {
      turbo_chain_t *chain = turbo_chain_create("reactish-bind-log");
      turbo_tool_registry_t *tools = turbo_tool_registry_create();
      turbo_event_log_t *log = turbo_event_log_create();
      json_value_t *state = turbo_chain_state_create_json_value();
      json_value_t *result = NULL;
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
      check_not_null(log);
      check_not_null(state);
      check_int_eq(turbo_tool_registry_add(tools, &sum_tool), TURBO_TOOL_OK);
      check_int_eq(turbo_chain_add_prompt_step(chain, "user_prompt", "user", "Add 41 and 1"),
                   TURBO_CHAIN_OK);
      check_int_eq(turbo_chain_add_model_step(chain, "planner", &model, tools), TURBO_CHAIN_OK);
      check_int_eq(turbo_chain_add_tool_step(chain, "tool_exec", tools), TURBO_CHAIN_OK);
      check_int_eq(turbo_chain_add_model_step(chain, "finalizer", &model, tools), TURBO_CHAIN_OK);

      check_int_eq(turbo_chain_run_json_value_log(chain, state, log, &result), TURBO_CHAIN_OK);
      check_not_null(result);
      check_int_eq(model_state.call_count, 2);
      check_int_eq(turbo_event_log_status(log), TURBO_EVENT_LOG_OK);
      check_size_eq(turbo_event_log_size(log), 3);
      check_str_eq(turbo_event_kind_json_value(turbo_event_log_get(log, 0)), "model");
      check_str_eq(turbo_event_kind_json_value(turbo_event_log_get(log, 1)), "tool_result");
      check_str_eq(turbo_event_kind_json_value(turbo_event_log_get(log, 2)), "model");

      turbo_runtime_json_destroy(result);
      turbo_runtime_json_destroy(state);
      turbo_event_log_destroy(log);
      turbo_tool_registry_destroy(tools);
      turbo_chain_destroy(chain);
    }

    it("should preserve child lineage on canonical tool result events in the event log") {
      turbo_chain_t *chain = turbo_chain_create("reactish-bind-child-log");
      turbo_tool_registry_t *tools = turbo_tool_registry_create();
      turbo_event_log_t *log = turbo_event_log_create();
      json_value_t *state = turbo_chain_state_create_json_value();
      json_value_t *result = NULL;
      fake_model_t model_state = {0};
      turbo_model_t model = {
          .name = "fake-child",
          .invoke = NULL,
          .invoke_json_value = fake_model_invoke_json_value_child_lineage,
          .user_data = &model_state,
          .user_data_free = NULL,
      };
      turbo_tool_definition_t delegate_tool = {
          .name = "delegate",
          .description = "return child lineage payload",
          .parameters_json = "{\"type\":\"object\"}",
          .strict = 0,
          .handler = NULL,
          .json_value_handler = fake_child_lineage_tool_json_value,
          .user_data = NULL,
          .user_data_free = NULL,
      };
      const json_value_t *tool_event;

      check_not_null(chain);
      check_not_null(tools);
      check_not_null(log);
      check_not_null(state);
      check_int_eq(turbo_tool_registry_add(tools, &delegate_tool), TURBO_TOOL_OK);
      check_int_eq(turbo_chain_add_prompt_step(chain, "user_prompt", "user", "delegate"),
                   TURBO_CHAIN_OK);
      check_int_eq(turbo_chain_add_model_step(chain, "planner", &model, tools), TURBO_CHAIN_OK);
      check_int_eq(turbo_chain_add_tool_step(chain, "tool_exec", tools), TURBO_CHAIN_OK);
      check_int_eq(turbo_chain_add_model_step(chain, "finalizer", &model, tools), TURBO_CHAIN_OK);

      check_int_eq(turbo_chain_run_json_value_log(chain, state, log, &result), TURBO_CHAIN_OK);
      check_not_null(result);
      check_int_eq(model_state.call_count, 2);
      check_size_eq(turbo_event_log_size(log), 3);

      tool_event = turbo_event_log_get(log, 1);
      check_str_eq(turbo_event_kind_json_value(tool_event), "tool_result");
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(tool_event, "child_thread_id")),
                   "thr_child");
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(tool_event, "child_run_id")),
                   "run_child");
      check_int_eq(turbo_json_type(
                       turbo_json_object_get(tool_event, "child_checkpoint_id")),
                   TURBO_JSON_NULL);
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(tool_event, "child_status")),
                   "completed");
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(tool_event, "parent_agent_run_id")),
                   "run_parent");
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(tool_event, "parent_tool_call_id")),
                   "call_parent");
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(tool_event, "parent_tool_name")),
                   "delegate");

      turbo_runtime_json_destroy(result);
      turbo_runtime_json_destroy(state);
      turbo_event_log_destroy(log);
      turbo_tool_registry_destroy(tools);
      turbo_chain_destroy(chain);
    }
  }
}
