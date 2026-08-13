#include "tinytest.h"
#include "turbo_event.h"
#include "turbo_model_provider.h"

typedef struct event_stream_capture_s {
  int count;
  const json_value_t *last_event;
} event_stream_capture_t;

static void event_stream_capture_sink(const json_value_t *event,
                                      void *user_data) {
  event_stream_capture_t *capture = (event_stream_capture_t *)user_data;

  check_not_null(capture);
  capture->count++;
  capture->last_event = event;
}

spec("turbo event runtime") {
  describe("canonical runtime events") {
    it("should build and validate TurboParser JSON-native model events") {
      json_value_t *tool_calls = turbo_json_create_array();
      json_value_t *call = turbo_json_create_object();
      json_value_t *event;

      check_not_null(tool_calls);
      check_not_null(call);
      check_int_eq(turbo_runtime_json_object_set(
                       call, "call_id", turbo_json_create_string("call_1")),
                   TURBO_RUNTIME_JSON_OK);
      check_int_eq(turbo_runtime_json_object_set(
                       call, "name", turbo_json_create_string("sum")),
                   TURBO_RUNTIME_JSON_OK);
      check_int_eq(turbo_runtime_json_object_set(
                       call, "arguments", turbo_json_create_string("{\"a\":1}")),
                   TURBO_RUNTIME_JSON_OK);
      check_int_eq(turbo_runtime_json_array_append(tool_calls, call),
                   TURBO_RUNTIME_JSON_OK);

      event = turbo_event_model_create_json_value("resp_1", "hello", tool_calls);
      check_not_null(event);
      check_str_eq(turbo_event_kind_json_value(event), "model");
      check_int_eq(turbo_event_validate_json_value(event), 0);
      check_int_eq(turbo_event_model_validate_json_value(event), 0);
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(event, "kind")),
                   "model");

      turbo_runtime_json_destroy(event);
      turbo_runtime_json_destroy(tool_calls);
    }

    it("should normalize provider responses into TurboParser JSON-native events") {
      json_value_t *chat_response = turbo_json_create_object();
      json_value_t *choices = turbo_json_create_array();
      json_value_t *choice = turbo_json_create_object();
      json_value_t *message = turbo_json_create_object();
      json_value_t *event;

      check_not_null(chat_response);
      check_not_null(choices);
      check_not_null(choice);
      check_not_null(message);

      turbo_json_object_set_string(chat_response, "id", "chat_1");
      turbo_json_object_set_string(message, "content", "world");
      turbo_json_object_add(choice, "message", message);
      turbo_json_array_add(choices, choice);
      turbo_json_object_add(chat_response, "choices", choices);

      event = turbo_model_provider_response_to_event_json_value(
          turbo_model_provider_openai_chat_completions(), chat_response);
      check_not_null(event);
      check_str_eq(turbo_event_kind_json_value(event), "model");
      check_int_eq(turbo_event_validate_json_value(event), 0);
      check_int_eq(turbo_event_model_validate_json_value(event), 0);
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(event, "output_text")),
                   "world");

      turbo_runtime_json_destroy(event);
      turbo_free_json(&chat_response);
    }

    it("should build and validate TurboParser JSON-native trace events") {
      json_value_t *event =
          turbo_event_trace_create_json_value("tool_dispatch", "begin", "{\"ok\":true}", 0);
      check_not_null(event);
      check_str_eq(turbo_event_kind_json_value(event), "trace");
      check_int_eq(turbo_event_validate_json_value(event), 0);
      check_int_eq(turbo_event_trace_validate_json_value(event), 0);
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(event, "kind")),
                   "trace");
      turbo_runtime_json_destroy(event);
    }

    it("should build and validate TurboParser JSON-native tool result events") {
      json_value_t *output_value =
          turbo_json_create_string("structured");
      json_value_t *event = turbo_event_tool_result_create_json_value(
          "sum", "{\"a\":1,\"b\":2}", "3", output_value, 0);

      check_not_null(output_value);
      check_not_null(event);
      check_str_eq(turbo_event_kind_json_value(event), "tool_result");
      check_int_eq(turbo_event_validate_json_value(event), 0);
      check_int_eq(turbo_event_tool_result_validate_json_value(event), 0);
      check_str_eq(
          turbo_runtime_json_value_as_string(turbo_json_object_get(event, "name")),
          "sum");
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(event, "arguments_json")),
                   "{\"a\":1,\"b\":2}");
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(event, "output")),
                   "3");
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(event, "output_value")),
                   "structured");

      turbo_runtime_json_destroy(event);
      turbo_runtime_json_destroy(output_value);
    }

    it("should carry child lineage in canonical TurboParser JSON-native tool result events") {
      json_value_t *output_value =
          turbo_json_create_object();
      json_value_t *event;

      check_not_null(output_value);
      check_int_eq(
          turbo_runtime_json_object_set(
              output_value, "child_thread_id",
              turbo_json_create_string("thr_child")),
          TURBO_RUNTIME_JSON_OK);
      check_int_eq(
          turbo_runtime_json_object_set(output_value, "child_run_id",
                                             turbo_json_create_string(
                                                 "run_child")),
          TURBO_RUNTIME_JSON_OK);
      check_int_eq(
          turbo_runtime_json_object_set(
              output_value, "child_checkpoint_id",
              turbo_json_create_null()),
          TURBO_RUNTIME_JSON_OK);
      check_int_eq(
          turbo_runtime_json_object_set(
              output_value, "child_status",
              turbo_json_create_string("completed")),
          TURBO_RUNTIME_JSON_OK);

      event = turbo_event_tool_result_create_json_value(
          "delegate", "{\"input\":\"hello\"}",
          "{\"ok\":true,\"summary\":\"ok\",\"stdout\":\"\",\"stderr\":\"\","
          "\"child_thread_id\":\"thr_child\",\"child_run_id\":\"run_child\","
          "\"child_checkpoint_id\":null,\"child_status\":\"completed\"}",
          output_value, 0);

      check_not_null(event);
      check_int_eq(turbo_event_validate_json_value(event), 0);
      check_int_eq(turbo_event_tool_result_validate_json_value(event), 0);
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(event, "child_thread_id")),
                   "thr_child");
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(event, "child_run_id")),
                   "run_child");
      check_int_eq(turbo_json_type(
                       turbo_json_object_get(event, "child_checkpoint_id")),
                   TURBO_JSON_NULL);
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(event, "child_status")),
                   "completed");

      turbo_runtime_json_destroy(event);
      turbo_runtime_json_destroy(output_value);
    }

    it("should carry parent lineage in canonical TurboParser JSON-native tool result events") {
      json_value_t *output_value =
          turbo_json_create_object();
      json_value_t *event;

      check_not_null(output_value);
      check_int_eq(
          turbo_runtime_json_object_set(
              output_value, "parent_agent_run_id",
              turbo_json_create_string("run_parent")),
          TURBO_RUNTIME_JSON_OK);
      check_int_eq(
          turbo_runtime_json_object_set(
              output_value, "parent_tool_call_id",
              turbo_json_create_string("call_parent")),
          TURBO_RUNTIME_JSON_OK);
      check_int_eq(
          turbo_runtime_json_object_set(
              output_value, "parent_tool_name",
              turbo_json_create_string("delegate")),
          TURBO_RUNTIME_JSON_OK);
      check_int_eq(
          turbo_runtime_json_object_set(
              output_value, "parent_graph_run_id",
              turbo_json_create_string("run_graph_parent")),
          TURBO_RUNTIME_JSON_OK);
      check_int_eq(
          turbo_runtime_json_object_set(output_value, "call_frame_id",
                                             turbo_json_create_string(
                                                 "frame_parent")),
          TURBO_RUNTIME_JSON_OK);

      event = turbo_event_tool_result_create_json_value(
          "delegate", "{\"input\":\"hello\"}",
          "{\"ok\":true,\"summary\":\"ok\",\"stdout\":\"\",\"stderr\":\"\","
          "\"parent_agent_run_id\":\"run_parent\","
          "\"parent_tool_call_id\":\"call_parent\","
          "\"parent_tool_name\":\"delegate\","
          "\"parent_graph_run_id\":\"run_graph_parent\","
          "\"call_frame_id\":\"frame_parent\"}",
          output_value, 0);

      check_not_null(event);
      check_int_eq(turbo_event_validate_json_value(event), 0);
      check_int_eq(turbo_event_tool_result_validate_json_value(event), 0);
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(event, "parent_agent_run_id")),
                   "run_parent");
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(event, "parent_tool_call_id")),
                   "call_parent");
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(event, "parent_tool_name")),
                   "delegate");
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(event, "parent_graph_run_id")),
                   "run_graph_parent");
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(event, "call_frame_id")),
                   "frame_parent");

      turbo_runtime_json_destroy(event);
      turbo_runtime_json_destroy(output_value);
    }

    it("should build and validate canonical TurboParser JSON-native handoff events") {
      json_value_t *schema = turbo_event_handoff_schema_json_value();
      json_value_t *properties = NULL;
      json_value_t *required = NULL;
      json_value_t *event = turbo_event_handoff_create_json_value(
          "requested", "planner", "executor", "delegate execution", "planner", 0);

      check_not_null(schema);
      properties = turbo_json_object_get(schema, "properties");
      required = turbo_json_object_get(schema, "required");
      check_not_null(properties);
      check_not_null(required);
      check_not_null(turbo_json_object_get(properties, "phase"));
      check_not_null(turbo_json_object_get(properties, "from_agent"));
      check_not_null(turbo_json_object_get(properties, "target_agent"));
      check_not_null(turbo_json_object_get(properties, "reason"));
      check_not_null(turbo_json_object_get(properties, "active_agent"));
      check_not_null(turbo_json_object_get(properties, "status"));

      check_not_null(event);
      check_str_eq(turbo_event_kind_json_value(event), "handoff");
      check_int_eq(turbo_event_validate_json_value(event), 0);
      check_int_eq(turbo_event_handoff_validate_json_value(event), 0);
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(event, "phase")),
                   "requested");
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(event, "from_agent")),
                   "planner");
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(event, "target_agent")),
                   "executor");
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(event, "reason")),
                   "delegate execution");
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(event, "active_agent")),
                   "planner");

      turbo_runtime_json_destroy(event);
      turbo_runtime_json_destroy(schema);
    }

    it("should preserve nullable handoff fields in canonical TurboParser JSON-native handoff events") {
      json_value_t *event =
          turbo_event_handoff_create_json_value("committed", "planner", NULL, NULL, "executor", 0);

      check_not_null(event);
      check_int_eq(turbo_event_validate_json_value(event), 0);
      check_int_eq(turbo_event_handoff_validate_json_value(event), 0);
      check_int_eq(
          turbo_json_type(turbo_json_object_get(event, "target_agent")),
          TURBO_JSON_NULL);
      check_int_eq(
          turbo_json_type(turbo_json_object_get(event, "reason")),
          TURBO_JSON_NULL);
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(event, "active_agent")),
                   "executor");

      turbo_runtime_json_destroy(event);
    }

    it("should reject malformed canonical TurboParser JSON-native handoff events") {
      json_value_t *event = turbo_json_create_object();

      check_not_null(event);
      check_int_eq(turbo_runtime_json_object_set(
                       event, "kind", turbo_json_create_string("handoff")),
                   TURBO_RUNTIME_JSON_OK);
      check_int_eq(turbo_runtime_json_object_set(
                       event, "phase", turbo_json_create_string("requested")),
                   TURBO_RUNTIME_JSON_OK);
      check_int_eq(turbo_runtime_json_object_set(
                       event, "from_agent", turbo_json_create_string("planner")),
                   TURBO_RUNTIME_JSON_OK);
      check_int_eq(turbo_runtime_json_object_set(
                       event, "target_agent",
                       turbo_json_create_string("executor")),
                   TURBO_RUNTIME_JSON_OK);
      check_int_eq(turbo_runtime_json_object_set(
                       event, "reason", turbo_json_create_string("delegate")),
                   TURBO_RUNTIME_JSON_OK);
      check_int_eq(turbo_runtime_json_object_set(
                       event, "active_agent", turbo_json_create_array()),
                   TURBO_RUNTIME_JSON_OK);
      check_int_eq(turbo_runtime_json_object_set(
                       event, "status", turbo_json_create_int64(0)),
                   TURBO_RUNTIME_JSON_OK);
      check_int_eq(turbo_event_validate_json_value(event), -1);
      check_int_eq(turbo_event_handoff_validate_json_value(event), -1);

      turbo_runtime_json_destroy(event);
    }

    it("should classify canonical events by stream mode") {
      json_value_t *tool_calls = turbo_json_create_array();
      json_value_t *model =
          turbo_event_model_create_json_value("resp_1", "hello", tool_calls);
      json_value_t *trace =
          turbo_event_trace_create_json_value("node_start", "planner", "{}", 0);
      json_value_t *tool =
          turbo_event_tool_result_create_json_value("sum", "{}", "ok", NULL, 0);
      json_value_t *handoff =
          turbo_event_handoff_create_json_value("requested", "planner", "executor",
                                          "delegate", "planner", 0);

      check_not_null(tool_calls);
      check_not_null(model);
      check_not_null(trace);
      check_not_null(tool);
      check_not_null(handoff);

      check_int_eq(turbo_event_stream_mode_accepts_json_value(TURBO_EVENT_STREAM_ALL, model), 1);
      check_int_eq(turbo_event_stream_mode_accepts_json_value(TURBO_EVENT_STREAM_ALL, trace), 1);
      check_int_eq(turbo_event_stream_mode_accepts_json_value(TURBO_EVENT_STREAM_MESSAGES, model), 1);
      check_int_eq(turbo_event_stream_mode_accepts_json_value(TURBO_EVENT_STREAM_MESSAGES, trace), 0);
      check_int_eq(turbo_event_stream_mode_accepts_json_value(TURBO_EVENT_STREAM_UPDATES, model), 0);
      check_int_eq(turbo_event_stream_mode_accepts_json_value(TURBO_EVENT_STREAM_UPDATES, trace), 1);
      check_int_eq(turbo_event_stream_mode_accepts_json_value(TURBO_EVENT_STREAM_UPDATES, tool), 1);
      check_int_eq(turbo_event_stream_mode_accepts_json_value(TURBO_EVENT_STREAM_UPDATES, handoff), 1);
      check_int_eq(turbo_event_stream_mode_accepts_json_value(TURBO_EVENT_STREAM_TOOLS, tool), 1);
      check_int_eq(turbo_event_stream_mode_accepts_json_value(TURBO_EVENT_STREAM_TOOLS, trace), 0);
      check_int_eq(turbo_event_stream_mode_accepts_json_value(TURBO_EVENT_STREAM_DEBUG, trace), 1);
      check_int_eq(turbo_event_stream_mode_accepts_json_value(TURBO_EVENT_STREAM_DEBUG, model), 0);
      check_int_eq(turbo_event_stream_mode_accepts_json_value((turbo_event_stream_mode_t)99, model), 0);
      check_int_eq(turbo_event_stream_mode_accepts_json_value(TURBO_EVENT_STREAM_ALL, NULL), 0);

      turbo_runtime_json_destroy(handoff);
      turbo_runtime_json_destroy(tool);
      turbo_runtime_json_destroy(trace);
      turbo_runtime_json_destroy(model);
      turbo_runtime_json_destroy(tool_calls);
    }

    it("should filter stream sink events by mode") {
      json_value_t *tool_calls = turbo_json_create_array();
      json_value_t *model =
          turbo_event_model_create_json_value("resp_1", "hello", tool_calls);
      json_value_t *trace =
          turbo_event_trace_create_json_value("node_start", "planner", "{}", 0);
      event_stream_capture_t capture = {0};
      turbo_event_stream_filter_t filter = {0};

      check_not_null(tool_calls);
      check_not_null(model);
      check_not_null(trace);

      filter.mode = TURBO_EVENT_STREAM_MESSAGES;
      filter.sink = event_stream_capture_sink;
      filter.sink_user_data = &capture;
      turbo_event_stream_filter_sink_json_value(trace, &filter);
      check_int_eq(capture.count, 0);
      turbo_event_stream_filter_sink_json_value(model, &filter);
      check_int_eq(capture.count, 1);
      check_ptr_eq(capture.last_event, model);

      filter.mode = TURBO_EVENT_STREAM_DEBUG;
      turbo_event_stream_filter_sink_json_value(model, &filter);
      check_int_eq(capture.count, 1);
      turbo_event_stream_filter_sink_json_value(trace, &filter);
      check_int_eq(capture.count, 2);
      check_ptr_eq(capture.last_event, trace);

      filter.sink = NULL;
      turbo_event_stream_filter_sink_json_value(trace, &filter);
      check_int_eq(capture.count, 2);
      turbo_event_stream_filter_sink_json_value(trace, NULL);
      check_int_eq(capture.count, 2);

      turbo_runtime_json_destroy(trace);
      turbo_runtime_json_destroy(model);
      turbo_runtime_json_destroy(tool_calls);
    }
  }
}
