#include "tinytest.h"
#include "turbo_event_log.h"
#include "turbo_runnable.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
  int count;
} runnable_counter_t;

typedef struct {
  int before_count;
  int after_count;
} runnable_wrap_counter_t;

typedef struct {
  int count;
  const char *last_detail;
  const char *last_name;
  int saw_graph_node;
} runnable_event_capture_t;

static char *test_strdup_local(const char *value) {
  char *copy;
  size_t length;

  if (!value) {
    return NULL;
  }

  length = strlen(value);
  copy = (char *)malloc(length + 1);
  check_not_null(copy);
  memcpy(copy, value, length + 1);
  return copy;
}

static int increment_json_value(const json_value_t *input,
                          json_value_t **out_output, void *user_data) {
  runnable_counter_t *counter = (runnable_counter_t *)user_data;
  json_value_t *output;

  check_not_null(input);
  check_not_null(out_output);
  check_not_null(counter);

  counter->count++;
  output = turbo_json_clone(input);
  check_not_null(output);
  check_int_eq(
      turbo_runtime_json_object_set(output, "count",
                                         turbo_json_create_int64(counter->count)),
      TURBO_RUNTIME_JSON_OK);
  *out_output = output;
  return 0;
}

static int wrap_before_json_value(const json_value_t *input,
                            json_value_t **out_input,
                            void *user_data) {
  runnable_wrap_counter_t *counter = (runnable_wrap_counter_t *)user_data;
  json_value_t *replacement;

  check_not_null(input);
  check_not_null(out_input);
  check_not_null(counter);

  counter->before_count++;
  replacement = turbo_json_clone(input);
  check_not_null(replacement);
  check_int_eq(turbo_runtime_json_object_set(
                   replacement, "value",
                   turbo_json_create_string("wrapped")),
               TURBO_RUNTIME_JSON_OK);
  *out_input = replacement;
  return 0;
}

static int wrap_after_json_value(const json_value_t *input,
                           const json_value_t *output,
                           json_value_t **out_output,
                           void *user_data) {
  runnable_wrap_counter_t *counter = (runnable_wrap_counter_t *)user_data;
  json_value_t *replacement;

  check_not_null(input);
  check_not_null(output);
  check_not_null(out_output);
  check_not_null(counter);
  check_str_eq(turbo_runtime_json_value_as_string(
                   turbo_json_object_get(input, "value")),
               "wrapped");

  counter->after_count++;
  replacement = turbo_json_clone(output);
  check_not_null(replacement);
  check_int_eq(turbo_runtime_json_object_set(
                   replacement, "after", turbo_json_create_bool(1)),
               TURBO_RUNTIME_JSON_OK);
  *out_output = replacement;
  return 0;
}

static int graph_mark_json_value(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  (void)user_data;
  return turbo_runtime_json_object_set(
             ctx->json_value_state, "graph_done", turbo_json_create_bool(1)) ==
                 TURBO_RUNTIME_JSON_OK
             ? 0
             : -1;
}

static int state_graph_mark_json_value(turbo_state_graph_exec_ctx_t *ctx, void *user_data) {
  (void)user_data;
  return turbo_runtime_json_object_set(
             ctx->update, "state_done", turbo_json_create_bool(1)) ==
                 TURBO_RUNTIME_JSON_OK
             ? 0
             : -1;
}

static void capture_runnable_event(const json_value_t *event, void *user_data) {
  runnable_event_capture_t *capture = (runnable_event_capture_t *)user_data;

  check_not_null(event);
  check_not_null(capture);
  capture->count++;
  free((void *)capture->last_detail);
  free((void *)capture->last_name);
  capture->last_name = test_strdup_local(
      turbo_runtime_json_value_as_string(turbo_json_object_get(event, "name")));
  capture->last_detail = test_strdup_local(
      turbo_runtime_json_value_as_string(turbo_json_object_get(event, "detail")));
  if (capture->last_name && strcmp(capture->last_name, "graph.node") == 0) {
    capture->saw_graph_node = 1;
  }
}

spec("turbo runnable runtime") {
  describe("TurboParser JSON-native composition") {
    it("should invoke and pipe TurboParser JSON-native runnables") {
      runnable_counter_t first_counter = {0};
      runnable_counter_t second_counter = {0};
      turbo_runnable_config_t first_config = {
          .invoke_json_value = increment_json_value,
          .invoke_json_value_stream = NULL,
          .user_data = &first_counter,
          .user_data_free = NULL};
      turbo_runnable_config_t second_config = {
          .invoke_json_value = increment_json_value,
          .invoke_json_value_stream = NULL,
          .user_data = &second_counter,
          .user_data_free = NULL};
      turbo_runnable_t *first = turbo_runnable_create(&first_config);
      turbo_runnable_t *second = turbo_runnable_create(&second_config);
      turbo_runnable_t *pipe = turbo_runnable_pipe(first, second);
      json_value_t *input = turbo_json_create_object();
      json_value_t *output = NULL;

      check_not_null(first);
      check_not_null(second);
      check_not_null(pipe);
      check_not_null(input);

      check_int_eq(turbo_runtime_json_object_set(
                       input, "value", turbo_json_create_string("ok")),
                   TURBO_RUNTIME_JSON_OK);
      check_int_eq(turbo_runnable_invoke_json_value(pipe, input, &output), 0);
      check_int_eq((int)turbo_runtime_json_value_as_int64(
                       turbo_json_object_get(output, "count"), 0),
                   1);
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(output, "value")),
                   "ok");

      turbo_runtime_json_destroy(output);
      turbo_runtime_json_destroy(input);
      turbo_runnable_destroy(pipe);
      turbo_runnable_destroy(second);
      turbo_runnable_destroy(first);
    }

    it("should batch invoke TurboParser JSON-native runnables") {
      runnable_counter_t counter = {0};
      turbo_runnable_config_t config = {
          .invoke_json_value = increment_json_value,
          .invoke_json_value_stream = NULL,
          .batch_json_value = NULL,
          .user_data = &counter,
          .user_data_free = NULL};
      turbo_runnable_t *runnable = turbo_runnable_create(&config);
      json_value_t *inputs = turbo_json_create_array();
      json_value_t *first_input =
          turbo_json_create_object();
      json_value_t *second_input =
          turbo_json_create_object();
      json_value_t *outputs = NULL;
      const json_value_t *first_output;
      const json_value_t *second_output;

      check_not_null(runnable);
      check_not_null(inputs);
      check_not_null(first_input);
      check_not_null(second_input);
      check_int_eq(turbo_runtime_json_object_set(
                       first_input, "value",
                       turbo_json_create_string("first")),
                   TURBO_RUNTIME_JSON_OK);
      check_int_eq(turbo_runtime_json_object_set(
                       second_input, "value",
                       turbo_json_create_string("second")),
                   TURBO_RUNTIME_JSON_OK);
      check_int_eq(turbo_runtime_json_array_append(inputs, first_input),
                   TURBO_RUNTIME_JSON_OK);
      check_int_eq(turbo_runtime_json_array_append(inputs, second_input),
                   TURBO_RUNTIME_JSON_OK);

      check_int_eq(turbo_runnable_batch_json_value(runnable, inputs, &outputs), 0);
      check_size_eq(turbo_runtime_json_value_size(outputs), 2);
      first_output = turbo_json_array_get(outputs, 0);
      second_output = turbo_json_array_get(outputs, 1);
      check_int_eq((int)turbo_runtime_json_value_as_int64(
                       turbo_json_object_get(first_output, "count"), 0),
                   1);
      check_int_eq((int)turbo_runtime_json_value_as_int64(
                       turbo_json_object_get(second_output, "count"), 0),
                   2);
      check_int_eq(counter.count, 2);

      turbo_runtime_json_destroy(outputs);
      turbo_runtime_json_destroy(inputs);
      turbo_runnable_destroy(runnable);
    }

    it("should batch through runnable pipes") {
      runnable_counter_t first_counter = {0};
      runnable_counter_t second_counter = {0};
      turbo_runnable_config_t first_config = {
          .invoke_json_value = increment_json_value,
          .invoke_json_value_stream = NULL,
          .batch_json_value = NULL,
          .user_data = &first_counter,
          .user_data_free = NULL};
      turbo_runnable_config_t second_config = {
          .invoke_json_value = increment_json_value,
          .invoke_json_value_stream = NULL,
          .batch_json_value = NULL,
          .user_data = &second_counter,
          .user_data_free = NULL};
      turbo_runnable_t *first = turbo_runnable_create(&first_config);
      turbo_runnable_t *second = turbo_runnable_create(&second_config);
      turbo_runnable_t *pipe = turbo_runnable_pipe(first, second);
      json_value_t *inputs = turbo_json_create_array();
      json_value_t *first_input =
          turbo_json_create_object();
      json_value_t *second_input =
          turbo_json_create_object();
      json_value_t *outputs = NULL;

      check_not_null(first);
      check_not_null(second);
      check_not_null(pipe);
      check_not_null(inputs);
      check_not_null(first_input);
      check_not_null(second_input);
      check_int_eq(turbo_runtime_json_array_append(inputs, first_input),
                   TURBO_RUNTIME_JSON_OK);
      check_int_eq(turbo_runtime_json_array_append(inputs, second_input),
                   TURBO_RUNTIME_JSON_OK);

      check_int_eq(turbo_runnable_batch_json_value(pipe, inputs, &outputs), 0);
      check_size_eq(turbo_runtime_json_value_size(outputs), 2);
      check_int_eq(first_counter.count, 2);
      check_int_eq(second_counter.count, 2);
      check_int_eq((int)turbo_runtime_json_value_as_int64(
                       turbo_json_object_get(
                           turbo_json_array_get(outputs, 0), "count"),
                       0),
                   1);
      check_int_eq((int)turbo_runtime_json_value_as_int64(
                       turbo_json_object_get(
                           turbo_json_array_get(outputs, 1), "count"),
                       0),
                   2);

      turbo_runtime_json_destroy(outputs);
      turbo_runtime_json_destroy(inputs);
      turbo_runnable_destroy(pipe);
      turbo_runnable_destroy(second);
      turbo_runnable_destroy(first);
    }

    it("should wrap runnable invoke input and output") {
      runnable_counter_t inner_counter = {0};
      runnable_wrap_counter_t wrap_counter = {0};
      turbo_runnable_config_t inner_config = {
          .invoke_json_value = increment_json_value,
          .invoke_json_value_stream = NULL,
          .batch_json_value = NULL,
          .user_data = &inner_counter,
          .user_data_free = NULL};
      turbo_runnable_wrap_config_t wrap_config = {
          .before_invoke = wrap_before_json_value,
          .after_invoke = wrap_after_json_value,
          .user_data = &wrap_counter,
          .user_data_free = NULL};
      turbo_runnable_t *inner = turbo_runnable_create(&inner_config);
      turbo_runnable_t *wrapped = turbo_runnable_wrap_json_value(inner, &wrap_config);
      json_value_t *input = turbo_json_create_object();
      json_value_t *output = NULL;

      check_not_null(inner);
      check_not_null(wrapped);
      check_not_null(input);
      check_int_eq(turbo_runtime_json_object_set(
                       input, "value",
                       turbo_json_create_string("original")),
                   TURBO_RUNTIME_JSON_OK);

      check_int_eq(turbo_runnable_invoke_json_value(wrapped, input, &output), 0);
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(output, "value")),
                   "wrapped");
      check_int_eq((int)turbo_runtime_json_value_as_int64(
                       turbo_json_object_get(output, "count"), 0),
                   1);
      check_true(turbo_runtime_json_value_as_bool(
          turbo_json_object_get(output, "after"), 0));
      check_int_eq(inner_counter.count, 1);
      check_int_eq(wrap_counter.before_count, 1);
      check_int_eq(wrap_counter.after_count, 1);

      turbo_runtime_json_destroy(output);
      turbo_runtime_json_destroy(input);
      turbo_runnable_destroy(wrapped);
      turbo_runnable_destroy(inner);
    }

    it("should apply runnable wrapper hooks while streaming") {
      runnable_counter_t inner_counter = {0};
      runnable_wrap_counter_t wrap_counter = {0};
      turbo_runnable_config_t inner_config = {
          .invoke_json_value = increment_json_value,
          .invoke_json_value_stream = NULL,
          .batch_json_value = NULL,
          .user_data = &inner_counter,
          .user_data_free = NULL};
      turbo_runnable_wrap_config_t wrap_config = {
          .before_invoke = wrap_before_json_value,
          .after_invoke = wrap_after_json_value,
          .user_data = &wrap_counter,
          .user_data_free = NULL};
      turbo_runnable_t *inner = turbo_runnable_create(&inner_config);
      turbo_runnable_t *wrapped = turbo_runnable_wrap_json_value(inner, &wrap_config);
      json_value_t *input = turbo_json_create_object();
      json_value_t *output = NULL;
      turbo_event_log_t *log = turbo_event_log_create();

      check_not_null(inner);
      check_not_null(wrapped);
      check_not_null(input);
      check_not_null(log);
      check_int_eq(
          turbo_runnable_invoke_json_value_stream(wrapped, input, turbo_event_log_capture_json_value, log,
                                            &output),
          0);
      check_not_null(output);
      check_int_eq(turbo_event_log_status(log), TURBO_EVENT_LOG_OK);
      check_size_eq(turbo_event_log_size(log), 2);
      check_int_eq(wrap_counter.before_count, 1);
      check_int_eq(wrap_counter.after_count, 1);
      check_true(turbo_runtime_json_value_as_bool(
          turbo_json_object_get(output, "after"), 0));

      turbo_event_log_destroy(log);
      turbo_runtime_json_destroy(output);
      turbo_runtime_json_destroy(input);
      turbo_runnable_destroy(wrapped);
      turbo_runnable_destroy(inner);
    }

    it("should apply runnable wrapper hooks to each batch item") {
      runnable_counter_t inner_counter = {0};
      runnable_wrap_counter_t wrap_counter = {0};
      turbo_runnable_config_t inner_config = {
          .invoke_json_value = increment_json_value,
          .invoke_json_value_stream = NULL,
          .batch_json_value = NULL,
          .user_data = &inner_counter,
          .user_data_free = NULL};
      turbo_runnable_wrap_config_t wrap_config = {
          .before_invoke = wrap_before_json_value,
          .after_invoke = wrap_after_json_value,
          .user_data = &wrap_counter,
          .user_data_free = NULL};
      turbo_runnable_t *inner = turbo_runnable_create(&inner_config);
      turbo_runnable_t *wrapped = turbo_runnable_wrap_json_value(inner, &wrap_config);
      json_value_t *inputs = turbo_json_create_array();
      json_value_t *first_input =
          turbo_json_create_object();
      json_value_t *second_input =
          turbo_json_create_object();
      json_value_t *outputs = NULL;

      check_not_null(inner);
      check_not_null(wrapped);
      check_not_null(inputs);
      check_not_null(first_input);
      check_not_null(second_input);
      check_int_eq(turbo_runtime_json_array_append(inputs, first_input),
                   TURBO_RUNTIME_JSON_OK);
      check_int_eq(turbo_runtime_json_array_append(inputs, second_input),
                   TURBO_RUNTIME_JSON_OK);

      check_int_eq(turbo_runnable_batch_json_value(wrapped, inputs, &outputs), 0);
      check_size_eq(turbo_runtime_json_value_size(outputs), 2);
      check_int_eq(inner_counter.count, 2);
      check_int_eq(wrap_counter.before_count, 2);
      check_int_eq(wrap_counter.after_count, 2);
      check_true(turbo_runtime_json_value_as_bool(
          turbo_json_object_get(
              turbo_json_array_get(outputs, 0), "after"),
          0));
      check_true(turbo_runtime_json_value_as_bool(
          turbo_json_object_get(
              turbo_json_array_get(outputs, 1), "after"),
          0));

      turbo_runtime_json_destroy(outputs);
      turbo_runtime_json_destroy(inputs);
      turbo_runnable_destroy(wrapped);
      turbo_runnable_destroy(inner);
    }

    it("should wrap chains and graphs as runnables") {
      turbo_chain_t *chain = turbo_chain_create("runnable-chain");
      turbo_graph_t *graph = turbo_graph_create("runnable-graph");
      turbo_runnable_t *chain_runnable;
      turbo_runnable_t *graph_runnable;
      turbo_runnable_t *pipeline;
      json_value_t *input = turbo_chain_state_create_json_value();
      json_value_t *result = NULL;
      turbo_graph_run_result_t graph_result = {0};

      check_not_null(chain);
      check_not_null(graph);
      check_not_null(input);

      check_int_eq(turbo_runtime_json_object_set(
                       (json_value_t *)turbo_json_object_get(input,
                                                                                             "input"),
                       "task", turbo_json_create_string("ship")),
                   TURBO_RUNTIME_JSON_OK);
      check_int_eq(turbo_chain_add_prompt_step(chain, "prompt", "user", "Do {{task}} now."),
                   TURBO_CHAIN_OK);

      check_int_eq(turbo_graph_add_json_value_node(graph, "done", graph_mark_json_value, NULL),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_set_entry(graph, "done"), TURBO_GRAPH_EXEC_OK);

      chain_runnable = turbo_runnable_from_chain(chain);
      graph_runnable = turbo_runnable_from_graph(graph, NULL, &graph_result);
      pipeline = turbo_runnable_pipe(chain_runnable, graph_runnable);
      check_not_null(chain_runnable);
      check_not_null(graph_runnable);
      check_not_null(pipeline);

      check_int_eq(turbo_runnable_invoke_json_value(pipeline, input, &result), 0);
      check_true(turbo_runtime_json_value_as_bool(
          turbo_json_object_get(result, "graph_done"), 0));
      check_size_eq(turbo_runtime_json_value_size(
                        turbo_json_object_get(result, "messages")),
                    1);
      check_str_eq(graph_result.last_node, "done");

      turbo_runtime_json_destroy(result);
      turbo_runtime_json_destroy(input);
      turbo_runnable_destroy(pipeline);
      turbo_runnable_destroy(graph_runnable);
      turbo_runnable_destroy(chain_runnable);
      turbo_graph_destroy(graph);
      turbo_chain_destroy(chain);
    }

    it("should emit trace events while invoking a runnable stream") {
      runnable_counter_t counter = {0};
      runnable_event_capture_t capture = {0};
      turbo_runnable_config_t config = {
          .invoke_json_value = increment_json_value,
          .invoke_json_value_stream = NULL,
          .user_data = &counter,
          .user_data_free = NULL};
      turbo_runnable_t *runnable = turbo_runnable_create(&config);
      json_value_t *input = turbo_json_create_object();
      json_value_t *output = NULL;

      check_not_null(runnable);
      check_not_null(input);
      check_int_eq(
          turbo_runnable_invoke_json_value_stream(runnable, input, capture_runnable_event, &capture, &output),
          0);
      check_int_eq(capture.count, 2);
      check_str_eq(capture.last_name, "runnable.invoke");
      check_str_eq(capture.last_detail, "finish");

      free((void *)capture.last_name);
      free((void *)capture.last_detail);
      turbo_runtime_json_destroy(output);
      turbo_runtime_json_destroy(input);
      turbo_runnable_destroy(runnable);
    }

    it("should forward specialized runnable stream events without generic wrapper noise") {
      turbo_chain_t *chain = turbo_chain_create("runnable-stream-chain");
      turbo_runnable_t *runnable;
      json_value_t *input = turbo_chain_state_create_json_value();
      json_value_t *output = NULL;
      runnable_event_capture_t capture = {0};

      check_not_null(chain);
      check_not_null(input);
      check_int_eq(turbo_runtime_json_object_set(
                       (json_value_t *)turbo_json_object_get(input,
                                                                                             "input"),
                       "task", turbo_json_create_string("stream")),
                   TURBO_RUNTIME_JSON_OK);
      check_int_eq(turbo_chain_add_prompt_step(chain, "prompt", "user", "Do {{task}} now."),
                   TURBO_CHAIN_OK);

      runnable = turbo_runnable_from_chain(chain);
      check_not_null(runnable);

      check_int_eq(
          turbo_runnable_invoke_json_value_stream(runnable, input, capture_runnable_event, &capture, &output),
          0);
      check_int_eq(capture.count, 2);
      check_str_eq(capture.last_name, "runnable.chain");
      check_str_eq(capture.last_detail, "finish");

      free((void *)capture.last_name);
      free((void *)capture.last_detail);
      turbo_runtime_json_destroy(output);
      turbo_runtime_json_destroy(input);
      turbo_runnable_destroy(runnable);
      turbo_chain_destroy(chain);
    }

    it("should forward graph stream events through runnable adapters") {
      turbo_graph_t *graph = turbo_graph_create("runnable-stream-graph");
      turbo_runnable_t *runnable;
      json_value_t *input = turbo_json_create_object();
      json_value_t *output = NULL;
      runnable_event_capture_t capture = {0};

      check_not_null(graph);
      check_not_null(input);
      check_int_eq(turbo_graph_add_json_value_node(graph, "done", graph_mark_json_value, NULL),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_set_entry(graph, "done"), TURBO_GRAPH_EXEC_OK);

      runnable = turbo_runnable_from_graph(graph, NULL, NULL);
      check_not_null(runnable);

      check_int_eq(
          turbo_runnable_invoke_json_value_stream(runnable, input, capture_runnable_event, &capture, &output),
          0);
      check_true(capture.count > 2);
      check_true(capture.saw_graph_node);
      check_str_eq(capture.last_name, "runnable.graph");
      check_str_eq(capture.last_detail, "finish");

      free((void *)capture.last_name);
      free((void *)capture.last_detail);
      turbo_runtime_json_destroy(output);
      turbo_runtime_json_destroy(input);
      turbo_runnable_destroy(runnable);
      turbo_graph_destroy(graph);
    }

    it("should wrap state graphs as runnables") {
      turbo_state_graph_t *graph = turbo_state_graph_create("runnable-state-graph");
      turbo_runnable_t *runnable;
      json_value_t *default_value =
          turbo_json_create_bool(0);
      json_value_t *input = turbo_json_create_object();
      json_value_t *output = NULL;
      turbo_state_graph_channel_config_t channel = {
          .reducer = TURBO_STATE_GRAPH_REDUCER_REPLACE,
          .value_kind = TURBO_JSON_BOOL,
          .default_value = default_value,
          .description = NULL};
      turbo_state_graph_run_result_t result = {0};
      turbo_event_log_t *log = turbo_event_log_create();
      const json_value_t *last_event;

      check_not_null(graph);
      check_not_null(default_value);
      check_not_null(input);
      check_not_null(log);
      check_int_eq(turbo_state_graph_add_channel(graph, "state_done", &channel),
                   TURBO_STATE_GRAPH_OK);
      turbo_runtime_json_destroy(default_value);
      check_int_eq(turbo_state_graph_add_json_value_node(graph, "done", state_graph_mark_json_value, NULL),
                   TURBO_STATE_GRAPH_OK);
      check_int_eq(turbo_state_graph_set_entry(graph, "done"), TURBO_STATE_GRAPH_OK);

      runnable = turbo_runnable_from_state_graph(graph, "thread-runnable", NULL, &result);
      check_not_null(runnable);

      check_int_eq(turbo_runnable_invoke_json_value_log(runnable, input, log, &output), 0);
      check_not_null(output);
      check_true(turbo_runtime_json_value_as_bool(
          turbo_json_object_get(output, "state_done"), 0));
      check_str_eq(result.thread_id, "thread-runnable");
      check_str_eq(result.last_node, "done");
      check_size_eq(turbo_event_log_size(log), 2);

      last_event = turbo_event_log_get(log, turbo_event_log_size(log) - 1);
      check_not_null(last_event);
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(last_event, "name")),
                   "runnable.state_graph");
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(last_event, "detail")),
                   "finish");

      turbo_event_log_destroy(log);
      turbo_runtime_json_destroy(output);
      turbo_runtime_json_destroy(input);
      turbo_runnable_destroy(runnable);
      turbo_state_graph_destroy(graph);
    }

    it("should capture runnable adapter streams into an event log") {
      turbo_graph_t *graph = turbo_graph_create("runnable-event-log-graph");
      turbo_runnable_t *runnable;
      json_value_t *input = turbo_json_create_object();
      json_value_t *output = NULL;
      turbo_event_log_t *log = turbo_event_log_create();
      const json_value_t *last_event;
      size_t i;
      int saw_graph_node = 0;

      check_not_null(graph);
      check_not_null(input);
      check_not_null(log);
      check_int_eq(turbo_graph_add_json_value_node(graph, "done", graph_mark_json_value, NULL),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_set_entry(graph, "done"), TURBO_GRAPH_EXEC_OK);

      runnable = turbo_runnable_from_graph(graph, NULL, NULL);
      check_not_null(runnable);

      check_int_eq(
          turbo_runnable_invoke_json_value_stream(runnable, input, turbo_event_log_capture_json_value, log, &output),
          0);
      check_not_null(output);
      check_true(turbo_runtime_json_value_as_bool(
          turbo_json_object_get(output, "graph_done"), 0));
      check_int_eq(turbo_event_log_status(log), TURBO_EVENT_LOG_OK);
      check_true(turbo_event_log_size(log) > 2);

      for (i = 0; i < turbo_event_log_size(log); ++i) {
        const json_value_t *event = turbo_event_log_get(log, i);
        const char *name = turbo_runtime_json_value_as_string(
            turbo_json_object_get(event, "name"));

        if (name && strcmp(name, "graph.node") == 0) {
          saw_graph_node = 1;
          break;
        }
      }
      check_true(saw_graph_node);

      last_event = turbo_event_log_get(log, turbo_event_log_size(log) - 1);
      check_not_null(last_event);
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(last_event, "name")),
                   "runnable.graph");
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(last_event, "detail")),
                   "finish");

      turbo_event_log_destroy(log);
      turbo_runtime_json_destroy(output);
      turbo_runtime_json_destroy(input);
      turbo_runnable_destroy(runnable);
      turbo_graph_destroy(graph);
    }

    it("should capture runnable streams through the log helper") {
      runnable_counter_t counter = {0};
      turbo_runnable_config_t config = {
          .invoke_json_value = increment_json_value,
          .invoke_json_value_stream = NULL,
          .user_data = &counter,
          .user_data_free = NULL};
      turbo_runnable_t *runnable = turbo_runnable_create(&config);
      json_value_t *input = turbo_json_create_object();
      json_value_t *output = NULL;
      turbo_event_log_t *log = turbo_event_log_create();

      check_not_null(runnable);
      check_not_null(input);
      check_not_null(log);
      check_int_eq(turbo_runnable_invoke_json_value_log(runnable, input, log, &output), 0);
      check_not_null(output);
      check_int_eq(turbo_event_log_status(log), TURBO_EVENT_LOG_OK);
      check_size_eq(turbo_event_log_size(log), 2);
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(turbo_event_log_get(log, 0), "name")),
                   "runnable.invoke");
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(turbo_event_log_get(log, 1), "detail")),
                   "finish");

      turbo_event_log_destroy(log);
      turbo_runtime_json_destroy(output);
      turbo_runtime_json_destroy(input);
      turbo_runnable_destroy(runnable);
    }
  }
}
