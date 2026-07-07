#include "turbo_runnable.h"

#include "turbo_agent_app.h"
#include "turbo_agent_session.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
  const turbo_runnable_t *first;
  const turbo_runnable_t *second;
} turbo_runnable_pipe_t;

typedef struct {
  const turbo_runnable_t *inner;
  turbo_runnable_wrap_config_t config;
} turbo_runnable_wrap_adapter_t;

typedef struct {
  turbo_chain_t *chain;
} turbo_runnable_chain_adapter_t;

typedef struct {
  turbo_graph_t *graph;
  turbo_graph_run_options_t options;
  turbo_graph_run_result_t *result_sink;
} turbo_runnable_graph_adapter_t;

typedef struct {
  turbo_state_graph_t *graph;
  char *thread_id;
  turbo_state_graph_run_options_t options;
  turbo_state_graph_run_result_t *result_sink;
} turbo_runnable_state_graph_adapter_t;

typedef struct {
  turbo_agent_session_t *session;
  turbo_graph_run_options_t options;
} turbo_runnable_agent_session_adapter_t;

typedef struct {
  turbo_agent_app_t *app;
  turbo_graph_run_options_t options;
} turbo_runnable_agent_app_adapter_t;

struct turbo_runnable_s {
  turbo_runnable_invoke_bind_fn invoke_bind;
  turbo_runnable_invoke_bind_stream_fn invoke_bind_stream;
  turbo_runnable_batch_bind_fn batch_bind;
  void *user_data;
  turbo_runnable_user_data_free_fn user_data_free;
};

static char *turbo_runnable_strdup(const char *text) {
  char *copy;
  size_t length;

  if (!text) {
    return NULL;
  }

  length = strlen(text);
  copy = (char *)malloc(length + 1);
  if (!copy) {
    return NULL;
  }
  memcpy(copy, text, length + 1);
  return copy;
}

static int turbo_runnable_state_graph_status_ok(turbo_state_graph_status_t status) {
  return status == TURBO_STATE_GRAPH_OK || status == TURBO_STATE_GRAPH_STOP ||
         status == TURBO_STATE_GRAPH_INTERRUPTED;
}

static void turbo_runnable_state_graph_adapter_free(void *user_data) {
  turbo_runnable_state_graph_adapter_t *adapter =
      (turbo_runnable_state_graph_adapter_t *)user_data;

  if (!adapter) {
    return;
  }

  free(adapter->thread_id);
  free(adapter);
}

static void turbo_runnable_wrap_adapter_free(void *user_data) {
  turbo_runnable_wrap_adapter_t *adapter = (turbo_runnable_wrap_adapter_t *)user_data;

  if (!adapter) {
    return;
  }
  if (adapter->config.user_data_free) {
    adapter->config.user_data_free(adapter->config.user_data);
  }
  free(adapter);
}

static const char *turbo_runnable_agent_text_input(
    const turbo_runtime_data_bind_value_t *input) {
  const turbo_runtime_data_bind_value_t *field;
  const char *text;

  text = turbo_runtime_data_bind_value_as_string(input);
  if (text) {
    return text;
  }
  if (turbo_runtime_data_bind_value_kind(input) != TURBO_RUNTIME_DATA_BIND_VALUE_OBJECT) {
    return NULL;
  }

  field = turbo_runtime_data_bind_object_get(input, "input");
  text = turbo_runtime_data_bind_value_as_string(field);
  if (text) {
    return text;
  }

  field = turbo_runtime_data_bind_object_get(input, "text");
  text = turbo_runtime_data_bind_value_as_string(field);
  if (text) {
    return text;
  }

  field = turbo_runtime_data_bind_object_get(input, "user_text");
  return turbo_runtime_data_bind_value_as_string(field);
}

static const turbo_runtime_data_bind_value_t *turbo_runnable_agent_messages_input(
    const turbo_runtime_data_bind_value_t *input) {
  const turbo_runtime_data_bind_value_t *messages;

  if (turbo_runtime_data_bind_value_kind(input) != TURBO_RUNTIME_DATA_BIND_VALUE_OBJECT) {
    return NULL;
  }
  messages = turbo_runtime_data_bind_object_get(input, "messages");
  if (turbo_runtime_data_bind_value_kind(messages) != TURBO_RUNTIME_DATA_BIND_VALUE_ARRAY) {
    return NULL;
  }
  return messages;
}

static int turbo_runnable_agent_result_bind(
    json_value_t **summary_json, turbo_runtime_data_bind_value_t **state,
    char *output_text, turbo_runtime_data_bind_value_t **out_output) {
  turbo_runtime_data_bind_value_t *result;
  turbo_runtime_data_bind_value_t *summary = NULL;
  turbo_runtime_data_bind_value_t *text = NULL;

  if (!state || !*state || !out_output) {
    return -1;
  }

  result = turbo_runtime_data_bind_value_create_object();
  if (!result) {
    return -1;
  }

  if (summary_json && *summary_json) {
    summary = turbo_runtime_data_bind_value_from_json(*summary_json);
    turbo_free_json(summary_json);
  } else {
    summary = turbo_runtime_data_bind_value_create_null();
  }
  if (!summary ||
      turbo_runtime_data_bind_object_set(result, "summary", summary) !=
          TURBO_RUNTIME_DATA_BIND_OK) {
    turbo_runtime_data_bind_value_destroy(summary);
    turbo_runtime_data_bind_value_destroy(result);
    return -1;
  }
  summary = NULL;

  if (turbo_runtime_data_bind_object_set(result, "state", *state) !=
      TURBO_RUNTIME_DATA_BIND_OK) {
    turbo_runtime_data_bind_value_destroy(result);
    return -1;
  }
  *state = NULL;

  text = output_text ? turbo_runtime_data_bind_value_create_string(output_text)
                     : turbo_runtime_data_bind_value_create_null();
  if (!text ||
      turbo_runtime_data_bind_object_set(result, "output_text", text) !=
          TURBO_RUNTIME_DATA_BIND_OK) {
    turbo_runtime_data_bind_value_destroy(text);
    turbo_runtime_data_bind_value_destroy(result);
    return -1;
  }

  *out_output = result;
  return 0;
}

static int turbo_runnable_pipe_invoke_bind(const turbo_runtime_data_bind_value_t *input,
                                           turbo_runtime_data_bind_value_t **out_output,
                                           void *user_data) {
  turbo_runnable_pipe_t *pipe = (turbo_runnable_pipe_t *)user_data;
  turbo_runtime_data_bind_value_t *middle = NULL;
  int rc;

  if (!pipe || !pipe->first || !pipe->second || !out_output) {
    return -1;
  }

  rc = turbo_runnable_invoke_bind(pipe->first, input, &middle);
  if (rc != 0) {
    return rc;
  }

  rc = turbo_runnable_invoke_bind(pipe->second, middle, out_output);
  turbo_runtime_data_bind_value_destroy(middle);
  return rc;
}

static int turbo_runnable_pipe_invoke_bind_stream(
    const turbo_runtime_data_bind_value_t *input, turbo_event_sink_bind_fn event_sink,
    void *event_sink_user_data, turbo_runtime_data_bind_value_t **out_output, void *user_data) {
  turbo_runnable_pipe_t *pipe = (turbo_runnable_pipe_t *)user_data;
  turbo_runtime_data_bind_value_t *middle = NULL;
  int rc;

  if (!pipe || !pipe->first || !pipe->second || !out_output) {
    return -1;
  }

  rc = turbo_runnable_invoke_bind_stream(pipe->first, input, event_sink, event_sink_user_data,
                                         &middle);
  if (rc != 0) {
    return rc;
  }

  rc = turbo_runnable_invoke_bind_stream(pipe->second, middle, event_sink, event_sink_user_data,
                                         out_output);
  turbo_runtime_data_bind_value_destroy(middle);
  return rc;
}

static int turbo_runnable_pipe_batch_bind(
    const turbo_runtime_data_bind_value_t *inputs,
    turbo_runtime_data_bind_value_t **out_outputs, void *user_data) {
  turbo_runnable_pipe_t *pipe = (turbo_runnable_pipe_t *)user_data;
  turbo_runtime_data_bind_value_t *middle = NULL;
  int rc;

  if (!pipe || !pipe->first || !pipe->second || !out_outputs) {
    return -1;
  }

  rc = turbo_runnable_batch_bind(pipe->first, inputs, &middle);
  if (rc != 0) {
    return rc;
  }

  rc = turbo_runnable_batch_bind(pipe->second, middle, out_outputs);
  turbo_runtime_data_bind_value_destroy(middle);
  return rc;
}

static int turbo_runnable_wrap_invoke_common(
    turbo_runnable_wrap_adapter_t *adapter,
    const turbo_runtime_data_bind_value_t *input,
    turbo_event_sink_bind_fn event_sink, void *event_sink_user_data,
    int use_stream,
    turbo_runtime_data_bind_value_t **out_output) {
  const turbo_runtime_data_bind_value_t *effective_input = input;
  turbo_runtime_data_bind_value_t *replacement_input = NULL;
  turbo_runtime_data_bind_value_t *inner_output = NULL;
  turbo_runtime_data_bind_value_t *replacement_output = NULL;
  int rc;

  if (!adapter || !adapter->inner || !out_output) {
    return -1;
  }

  *out_output = NULL;
  if (adapter->config.before_invoke) {
    rc = adapter->config.before_invoke(input, &replacement_input,
                                       adapter->config.user_data);
    if (rc != 0) {
      if (replacement_input && replacement_input != input) {
        turbo_runtime_data_bind_value_destroy(replacement_input);
      }
      return rc;
    }
    if (replacement_input) {
      effective_input = replacement_input;
    }
  }

  if (use_stream) {
    rc = turbo_runnable_invoke_bind_stream(adapter->inner, effective_input, event_sink,
                                           event_sink_user_data, &inner_output);
  } else {
    rc = turbo_runnable_invoke_bind(adapter->inner, effective_input, &inner_output);
  }
  if (rc != 0 || !inner_output) {
    if (replacement_input && replacement_input != input) {
      turbo_runtime_data_bind_value_destroy(replacement_input);
    }
    turbo_runtime_data_bind_value_destroy(inner_output);
    return rc != 0 ? rc : -1;
  }

  if (adapter->config.after_invoke) {
    rc = adapter->config.after_invoke(effective_input, inner_output, &replacement_output,
                                      adapter->config.user_data);
    if (rc != 0) {
      if (replacement_output && replacement_output != inner_output) {
        turbo_runtime_data_bind_value_destroy(replacement_output);
      }
      turbo_runtime_data_bind_value_destroy(inner_output);
      if (replacement_input && replacement_input != input) {
        turbo_runtime_data_bind_value_destroy(replacement_input);
      }
      return rc;
    }
  }

  if (replacement_output) {
    *out_output = replacement_output;
    if (replacement_output != inner_output) {
      turbo_runtime_data_bind_value_destroy(inner_output);
    }
  } else {
    *out_output = inner_output;
  }

  if (replacement_input && replacement_input != input) {
    turbo_runtime_data_bind_value_destroy(replacement_input);
  }
  return 0;
}

static int turbo_runnable_wrap_invoke_bind(
    const turbo_runtime_data_bind_value_t *input,
    turbo_runtime_data_bind_value_t **out_output, void *user_data) {
  return turbo_runnable_wrap_invoke_common((turbo_runnable_wrap_adapter_t *)user_data,
                                           input, NULL, NULL, 0, out_output);
}

static int turbo_runnable_wrap_invoke_bind_stream(
    const turbo_runtime_data_bind_value_t *input, turbo_event_sink_bind_fn event_sink,
    void *event_sink_user_data, turbo_runtime_data_bind_value_t **out_output,
    void *user_data) {
  return turbo_runnable_wrap_invoke_common((turbo_runnable_wrap_adapter_t *)user_data,
                                           input, event_sink, event_sink_user_data,
                                           1, out_output);
}

static int turbo_runnable_wrap_batch_bind(
    const turbo_runtime_data_bind_value_t *inputs,
    turbo_runtime_data_bind_value_t **out_outputs, void *user_data) {
  turbo_runnable_wrap_adapter_t *adapter = (turbo_runnable_wrap_adapter_t *)user_data;
  turbo_runtime_data_bind_value_t *outputs;
  size_t count;
  size_t i;

  if (!adapter || !inputs || !out_outputs ||
      turbo_runtime_data_bind_value_kind(inputs) != TURBO_RUNTIME_DATA_BIND_VALUE_ARRAY) {
    return -1;
  }

  *out_outputs = NULL;
  outputs = turbo_runtime_data_bind_value_create_array();
  if (!outputs) {
    return -1;
  }

  count = turbo_runtime_data_bind_value_size(inputs);
  for (i = 0; i < count; ++i) {
    const turbo_runtime_data_bind_value_t *input =
        turbo_runtime_data_bind_array_get(inputs, i);
    turbo_runtime_data_bind_value_t *output = NULL;
    int rc = turbo_runnable_wrap_invoke_common(adapter, input, NULL, NULL, 0, &output);

    if (rc != 0 || !output) {
      turbo_runtime_data_bind_value_destroy(output);
      turbo_runtime_data_bind_value_destroy(outputs);
      return rc != 0 ? rc : -1;
    }
    if (turbo_runtime_data_bind_array_append(outputs, output) !=
        TURBO_RUNTIME_DATA_BIND_OK) {
      turbo_runtime_data_bind_value_destroy(output);
      turbo_runtime_data_bind_value_destroy(outputs);
      return -1;
    }
  }

  *out_outputs = outputs;
  return 0;
}

static int turbo_runnable_chain_invoke_bind(const turbo_runtime_data_bind_value_t *input,
                                            turbo_runtime_data_bind_value_t **out_output,
                                            void *user_data) {
  turbo_runnable_chain_adapter_t *adapter = (turbo_runnable_chain_adapter_t *)user_data;

  if (!adapter || !adapter->chain || !out_output) {
    return -1;
  }

  return turbo_chain_run_bind(adapter->chain, input, out_output) == TURBO_CHAIN_OK ? 0 : -1;
}

static int turbo_runnable_chain_invoke_bind_stream(
    const turbo_runtime_data_bind_value_t *input, turbo_event_sink_bind_fn event_sink,
    void *event_sink_user_data, turbo_runtime_data_bind_value_t **out_output, void *user_data) {
  turbo_runtime_data_bind_value_t *event;
  int rc;

  if (event_sink) {
    event = turbo_event_trace_create_bind("runnable.chain", "start", "", 0);
    if (event) {
      event_sink(event, event_sink_user_data);
      turbo_runtime_data_bind_value_destroy(event);
    }
  }

  {
    turbo_runnable_chain_adapter_t *adapter = (turbo_runnable_chain_adapter_t *)user_data;
    rc = turbo_chain_run_bind_stream(adapter->chain, input, event_sink, event_sink_user_data,
                                     out_output) == TURBO_CHAIN_OK
             ? 0
             : -1;
  }

  if (event_sink) {
    event = turbo_event_trace_create_bind("runnable.chain", "finish", "", rc);
    if (event) {
      event_sink(event, event_sink_user_data);
      turbo_runtime_data_bind_value_destroy(event);
    }
  }

  return rc;
}

static int turbo_runnable_graph_invoke_bind(const turbo_runtime_data_bind_value_t *input,
                                            turbo_runtime_data_bind_value_t **out_output,
                                            void *user_data) {
  turbo_runnable_graph_adapter_t *adapter = (turbo_runnable_graph_adapter_t *)user_data;
  turbo_graph_run_result_t result = {0};
  turbo_graph_exec_status_t status;

  if (!adapter || !adapter->graph || !out_output) {
    return -1;
  }

  status = turbo_graph_run_bind(adapter->graph, input, &adapter->options, &result, out_output);
  if (adapter->result_sink) {
    *adapter->result_sink = result;
  }

  return (status == TURBO_GRAPH_EXEC_OK || status == TURBO_GRAPH_EXEC_STOP ||
          status == TURBO_GRAPH_EXEC_INTERRUPTED)
             ? 0
             : -1;
}

static int turbo_runnable_graph_invoke_bind_stream(
    const turbo_runtime_data_bind_value_t *input, turbo_event_sink_bind_fn event_sink,
    void *event_sink_user_data, turbo_runtime_data_bind_value_t **out_output, void *user_data) {
  turbo_runtime_data_bind_value_t *event;
  int rc;

  if (event_sink) {
    event = turbo_event_trace_create_bind("runnable.graph", "start", "", 0);
    if (event) {
      event_sink(event, event_sink_user_data);
      turbo_runtime_data_bind_value_destroy(event);
    }
  }

  {
    turbo_runnable_graph_adapter_t *adapter = (turbo_runnable_graph_adapter_t *)user_data;
    turbo_graph_run_result_t result = {0};
    turbo_graph_exec_status_t status =
        turbo_graph_run_bind_stream(adapter->graph, input, &adapter->options, event_sink,
                                    event_sink_user_data, &result, out_output);
    if (adapter->result_sink) {
      *adapter->result_sink = result;
    }
    rc = (status == TURBO_GRAPH_EXEC_OK || status == TURBO_GRAPH_EXEC_STOP ||
          status == TURBO_GRAPH_EXEC_INTERRUPTED)
             ? 0
             : -1;
  }

  if (event_sink) {
    event = turbo_event_trace_create_bind("runnable.graph", "finish", "", rc);
    if (event) {
      event_sink(event, event_sink_user_data);
      turbo_runtime_data_bind_value_destroy(event);
    }
  }

  return rc;
}

static int turbo_runnable_state_graph_invoke_bind(
    const turbo_runtime_data_bind_value_t *input,
    turbo_runtime_data_bind_value_t **out_output, void *user_data) {
  turbo_runnable_state_graph_adapter_t *adapter =
      (turbo_runnable_state_graph_adapter_t *)user_data;
  turbo_state_graph_run_result_t result = {0};
  turbo_state_graph_status_t status;

  if (!adapter || !adapter->graph || !out_output) {
    return -1;
  }

  status = turbo_state_graph_start(adapter->graph, adapter->thread_id, input,
                                   &adapter->options, &result, out_output);
  if (adapter->result_sink) {
    *adapter->result_sink = result;
  }

  return turbo_runnable_state_graph_status_ok(status) ? 0 : -1;
}

static int turbo_runnable_state_graph_invoke_bind_stream(
    const turbo_runtime_data_bind_value_t *input, turbo_event_sink_bind_fn event_sink,
    void *event_sink_user_data, turbo_runtime_data_bind_value_t **out_output, void *user_data) {
  turbo_runtime_data_bind_value_t *event;
  int rc;

  if (event_sink) {
    event = turbo_event_trace_create_bind("runnable.state_graph", "start", "", 0);
    if (event) {
      event_sink(event, event_sink_user_data);
      turbo_runtime_data_bind_value_destroy(event);
    }
  }

  rc = turbo_runnable_state_graph_invoke_bind(input, out_output, user_data);

  if (event_sink) {
    event = turbo_event_trace_create_bind("runnable.state_graph", "finish", "", rc);
    if (event) {
      event_sink(event, event_sink_user_data);
      turbo_runtime_data_bind_value_destroy(event);
    }
  }

  return rc;
}

static int turbo_runnable_agent_session_invoke_common(
    turbo_agent_session_t *session, const turbo_graph_run_options_t *options,
    const turbo_runtime_data_bind_value_t *input, turbo_event_sink_bind_fn event_sink,
    void *event_sink_user_data, turbo_runtime_data_bind_value_t **out_output) {
  const turbo_runtime_data_bind_value_t *messages;
  const char *text;
  json_value_t *summary = NULL;
  turbo_runtime_data_bind_value_t *state = NULL;
  char *output_text = NULL;
  int rc;

  if (!session || !input || !out_output) {
    return -1;
  }

  messages = turbo_runnable_agent_messages_input(input);
  if (messages) {
    if (event_sink) {
      rc = turbo_agent_session_start_messages_stream(
          session, messages, options, TURBO_EVENT_STREAM_ALL, event_sink,
          event_sink_user_data, &summary, &state);
    } else {
      rc = turbo_agent_session_start_messages(session, messages, options, &summary,
                                              &state);
    }
  } else {
    text = turbo_runnable_agent_text_input(input);
    if (!text) {
      return -1;
    }
    if (event_sink) {
      rc = turbo_agent_session_start_text_stream(
          session, text, options, TURBO_EVENT_STREAM_ALL, event_sink,
          event_sink_user_data, &summary, &state);
    } else {
      rc = turbo_agent_session_start_text(session, text, options, &summary, &state);
    }
  }

  if (rc != 0 || !state) {
    turbo_runtime_data_bind_value_destroy(state);
    turbo_free_json(&summary);
    return rc != 0 ? rc : -1;
  }

  output_text = turbo_agent_session_result_text(state);
  rc = turbo_runnable_agent_result_bind(&summary, &state, output_text, out_output);
  free(output_text);
  turbo_runtime_data_bind_value_destroy(state);
  turbo_free_json(&summary);
  return rc;
}

static int turbo_runnable_agent_session_invoke_bind(
    const turbo_runtime_data_bind_value_t *input,
    turbo_runtime_data_bind_value_t **out_output, void *user_data) {
  turbo_runnable_agent_session_adapter_t *adapter =
      (turbo_runnable_agent_session_adapter_t *)user_data;

  if (!adapter) {
    return -1;
  }
  return turbo_runnable_agent_session_invoke_common(
      adapter->session, &adapter->options, input, NULL, NULL, out_output);
}

static int turbo_runnable_agent_session_invoke_bind_stream(
    const turbo_runtime_data_bind_value_t *input, turbo_event_sink_bind_fn event_sink,
    void *event_sink_user_data, turbo_runtime_data_bind_value_t **out_output,
    void *user_data) {
  turbo_runnable_agent_session_adapter_t *adapter =
      (turbo_runnable_agent_session_adapter_t *)user_data;

  if (!adapter) {
    return -1;
  }
  return turbo_runnable_agent_session_invoke_common(
      adapter->session, &adapter->options, input, event_sink, event_sink_user_data,
      out_output);
}

static int turbo_runnable_agent_app_invoke_common(
    turbo_agent_app_t *app, const turbo_graph_run_options_t *options,
    const turbo_runtime_data_bind_value_t *input, turbo_event_sink_bind_fn event_sink,
    void *event_sink_user_data, turbo_runtime_data_bind_value_t **out_output) {
  const turbo_runtime_data_bind_value_t *messages;
  const char *text;
  json_value_t *summary = NULL;
  turbo_runtime_data_bind_value_t *state = NULL;
  char *output_text = NULL;
  int rc;

  if (!app || !input || !out_output) {
    return -1;
  }

  messages = turbo_runnable_agent_messages_input(input);
  if (messages) {
    if (event_sink) {
      rc = turbo_agent_app_start_messages_stream(
          app, messages, options, TURBO_EVENT_STREAM_ALL, event_sink,
          event_sink_user_data, &summary, &state);
    } else {
      rc = turbo_agent_app_start_messages(app, messages, options, &summary, &state);
    }
  } else {
    text = turbo_runnable_agent_text_input(input);
    if (!text) {
      return -1;
    }
    if (event_sink) {
      rc = turbo_agent_app_start_text_stream(
          app, text, options, TURBO_EVENT_STREAM_ALL, event_sink,
          event_sink_user_data, &summary, &state);
    } else {
      rc = turbo_agent_app_start_text(app, text, options, &summary, &state);
    }
  }

  if (rc != 0 || !state) {
    turbo_runtime_data_bind_value_destroy(state);
    turbo_free_json(&summary);
    return rc != 0 ? rc : -1;
  }

  output_text = turbo_agent_app_result_text(state);
  rc = turbo_runnable_agent_result_bind(&summary, &state, output_text, out_output);
  free(output_text);
  turbo_runtime_data_bind_value_destroy(state);
  turbo_free_json(&summary);
  return rc;
}

static int turbo_runnable_agent_app_invoke_bind(
    const turbo_runtime_data_bind_value_t *input,
    turbo_runtime_data_bind_value_t **out_output, void *user_data) {
  turbo_runnable_agent_app_adapter_t *adapter =
      (turbo_runnable_agent_app_adapter_t *)user_data;

  if (!adapter) {
    return -1;
  }
  return turbo_runnable_agent_app_invoke_common(adapter->app, &adapter->options, input,
                                               NULL, NULL, out_output);
}

static int turbo_runnable_agent_app_invoke_bind_stream(
    const turbo_runtime_data_bind_value_t *input, turbo_event_sink_bind_fn event_sink,
    void *event_sink_user_data, turbo_runtime_data_bind_value_t **out_output,
    void *user_data) {
  turbo_runnable_agent_app_adapter_t *adapter =
      (turbo_runnable_agent_app_adapter_t *)user_data;

  if (!adapter) {
    return -1;
  }
  return turbo_runnable_agent_app_invoke_common(
      adapter->app, &adapter->options, input, event_sink, event_sink_user_data,
      out_output);
}

turbo_runnable_t *turbo_runnable_create(const turbo_runnable_config_t *config) {
  turbo_runnable_t *runnable;

  if (!config || !config->invoke_bind) {
    return NULL;
  }

  runnable = (turbo_runnable_t *)calloc(1, sizeof(*runnable));
  if (!runnable) {
    return NULL;
  }

  runnable->invoke_bind = config->invoke_bind;
  runnable->invoke_bind_stream = config->invoke_bind_stream;
  runnable->batch_bind = config->batch_bind;
  runnable->user_data = config->user_data;
  runnable->user_data_free = config->user_data_free;
  return runnable;
}

void turbo_runnable_destroy(turbo_runnable_t *runnable) {
  if (!runnable) {
    return;
  }

  if (runnable->user_data_free) {
    runnable->user_data_free(runnable->user_data);
  }
  free(runnable);
}

int turbo_runnable_invoke_bind(const turbo_runnable_t *runnable,
                               const turbo_runtime_data_bind_value_t *input,
                               turbo_runtime_data_bind_value_t **out_output) {
  if (!runnable || !runnable->invoke_bind || !out_output) {
    return -1;
  }

  *out_output = NULL;
  return runnable->invoke_bind(input, out_output, runnable->user_data);
}

int turbo_runnable_invoke_bind_stream(
    const turbo_runnable_t *runnable, const turbo_runtime_data_bind_value_t *input,
    turbo_event_sink_bind_fn event_sink, void *event_sink_user_data,
    turbo_runtime_data_bind_value_t **out_output) {
  turbo_runtime_data_bind_value_t *event;
  int rc;

  if (!runnable || !out_output) {
    return -1;
  }

  if (runnable->invoke_bind_stream) {
    return runnable->invoke_bind_stream(input, event_sink, event_sink_user_data, out_output,
                                        runnable->user_data);
  }

  if (event_sink) {
    event = turbo_event_trace_create_bind("runnable.invoke", "start", "", 0);
    if (event) {
      event_sink(event, event_sink_user_data);
      turbo_runtime_data_bind_value_destroy(event);
    }
  }

  rc = turbo_runnable_invoke_bind(runnable, input, out_output);

  if (event_sink) {
    event = turbo_event_trace_create_bind("runnable.invoke", "finish", "", rc);
    if (event) {
      event_sink(event, event_sink_user_data);
      turbo_runtime_data_bind_value_destroy(event);
    }
  }

  return rc;
}

int turbo_runnable_invoke_bind_log(const turbo_runnable_t *runnable,
                                   const turbo_runtime_data_bind_value_t *input,
                                   turbo_event_log_t *log,
                                   turbo_runtime_data_bind_value_t **out_output) {
  int rc;

  if (!log) {
    return -1;
  }
  if (turbo_event_log_reset(log) != TURBO_EVENT_LOG_OK) {
    return -1;
  }

  rc = turbo_runnable_invoke_bind_stream(runnable, input, turbo_event_log_capture_bind, log,
                                         out_output);
  if (turbo_event_log_status(log) != TURBO_EVENT_LOG_OK) {
    if (out_output && *out_output) {
      turbo_runtime_data_bind_value_destroy(*out_output);
      *out_output = NULL;
    }
    return -1;
  }
  return rc;
}

int turbo_runnable_batch_bind(const turbo_runnable_t *runnable,
                              const turbo_runtime_data_bind_value_t *inputs,
                              turbo_runtime_data_bind_value_t **out_outputs) {
  turbo_runtime_data_bind_value_t *outputs;
  size_t count;
  size_t i;

  if (!runnable || !inputs || !out_outputs) {
    return -1;
  }

  *out_outputs = NULL;
  if (runnable->batch_bind) {
    return runnable->batch_bind(inputs, out_outputs, runnable->user_data);
  }
  if (turbo_runtime_data_bind_value_kind(inputs) != TURBO_RUNTIME_DATA_BIND_VALUE_ARRAY) {
    return -1;
  }

  outputs = turbo_runtime_data_bind_value_create_array();
  if (!outputs) {
    return -1;
  }

  count = turbo_runtime_data_bind_value_size(inputs);
  for (i = 0; i < count; ++i) {
    const turbo_runtime_data_bind_value_t *input =
        turbo_runtime_data_bind_array_get(inputs, i);
    turbo_runtime_data_bind_value_t *output = NULL;
    int rc = turbo_runnable_invoke_bind(runnable, input, &output);

    if (rc != 0 || !output) {
      turbo_runtime_data_bind_value_destroy(output);
      turbo_runtime_data_bind_value_destroy(outputs);
      return rc != 0 ? rc : -1;
    }
    if (turbo_runtime_data_bind_array_append(outputs, output) !=
        TURBO_RUNTIME_DATA_BIND_OK) {
      turbo_runtime_data_bind_value_destroy(output);
      turbo_runtime_data_bind_value_destroy(outputs);
      return -1;
    }
  }

  *out_outputs = outputs;
  return 0;
}

turbo_runnable_t *turbo_runnable_pipe(const turbo_runnable_t *first,
                                      const turbo_runnable_t *second) {
  turbo_runnable_pipe_t *pipe;
  turbo_runnable_config_t config;

  if (!first || !second) {
    return NULL;
  }

  pipe = (turbo_runnable_pipe_t *)calloc(1, sizeof(*pipe));
  if (!pipe) {
    return NULL;
  }

  pipe->first = first;
  pipe->second = second;
  memset(&config, 0, sizeof(config));
  config.invoke_bind = turbo_runnable_pipe_invoke_bind;
  config.invoke_bind_stream = turbo_runnable_pipe_invoke_bind_stream;
  config.batch_bind = turbo_runnable_pipe_batch_bind;
  config.user_data = pipe;
  config.user_data_free = free;
  return turbo_runnable_create(&config);
}

turbo_runnable_t *turbo_runnable_wrap_bind(
    const turbo_runnable_t *inner, const turbo_runnable_wrap_config_t *wrap_config) {
  turbo_runnable_wrap_adapter_t *adapter;
  turbo_runnable_config_t config;
  turbo_runnable_t *runnable;

  if (!inner || !wrap_config) {
    return NULL;
  }

  adapter = (turbo_runnable_wrap_adapter_t *)calloc(1, sizeof(*adapter));
  if (!adapter) {
    return NULL;
  }
  adapter->inner = inner;
  adapter->config = *wrap_config;

  memset(&config, 0, sizeof(config));
  config.invoke_bind = turbo_runnable_wrap_invoke_bind;
  config.invoke_bind_stream = turbo_runnable_wrap_invoke_bind_stream;
  config.batch_bind = turbo_runnable_wrap_batch_bind;
  config.user_data = adapter;
  config.user_data_free = turbo_runnable_wrap_adapter_free;

  runnable = turbo_runnable_create(&config);
  if (!runnable) {
    turbo_runnable_wrap_adapter_free(adapter);
  }
  return runnable;
}

turbo_runnable_t *turbo_runnable_from_chain(turbo_chain_t *chain) {
  turbo_runnable_chain_adapter_t *adapter;
  turbo_runnable_config_t config;

  if (!chain) {
    return NULL;
  }

  adapter = (turbo_runnable_chain_adapter_t *)calloc(1, sizeof(*adapter));
  if (!adapter) {
    return NULL;
  }

  adapter->chain = chain;
  memset(&config, 0, sizeof(config));
  config.invoke_bind = turbo_runnable_chain_invoke_bind;
  config.invoke_bind_stream = turbo_runnable_chain_invoke_bind_stream;
  config.user_data = adapter;
  config.user_data_free = free;
  return turbo_runnable_create(&config);
}

turbo_runnable_t *turbo_runnable_from_agent_session(
    turbo_agent_session_t *session, const turbo_graph_run_options_t *options) {
  turbo_runnable_agent_session_adapter_t *adapter;
  turbo_runnable_config_t config;

  if (!session) {
    return NULL;
  }

  adapter = (turbo_runnable_agent_session_adapter_t *)calloc(1, sizeof(*adapter));
  if (!adapter) {
    return NULL;
  }
  adapter->session = session;
  if (options) {
    adapter->options = *options;
  } else {
    memset(&adapter->options, 0, sizeof(adapter->options));
  }

  memset(&config, 0, sizeof(config));
  config.invoke_bind = turbo_runnable_agent_session_invoke_bind;
  config.invoke_bind_stream = turbo_runnable_agent_session_invoke_bind_stream;
  config.user_data = adapter;
  config.user_data_free = free;
  {
    turbo_runnable_t *runnable = turbo_runnable_create(&config);
    if (!runnable) {
      free(adapter);
    }
    return runnable;
  }
}

turbo_runnable_t *turbo_runnable_from_agent_app(
    turbo_agent_app_t *app, const turbo_graph_run_options_t *options) {
  turbo_runnable_agent_app_adapter_t *adapter;
  turbo_runnable_config_t config;

  if (!app) {
    return NULL;
  }

  adapter = (turbo_runnable_agent_app_adapter_t *)calloc(1, sizeof(*adapter));
  if (!adapter) {
    return NULL;
  }
  adapter->app = app;
  if (options) {
    adapter->options = *options;
  } else {
    memset(&adapter->options, 0, sizeof(adapter->options));
  }

  memset(&config, 0, sizeof(config));
  config.invoke_bind = turbo_runnable_agent_app_invoke_bind;
  config.invoke_bind_stream = turbo_runnable_agent_app_invoke_bind_stream;
  config.user_data = adapter;
  config.user_data_free = free;
  {
    turbo_runnable_t *runnable = turbo_runnable_create(&config);
    if (!runnable) {
      free(adapter);
    }
    return runnable;
  }
}

turbo_runnable_t *turbo_runnable_from_state_graph(
    turbo_state_graph_t *graph, const char *thread_id,
    const turbo_state_graph_run_options_t *options,
    turbo_state_graph_run_result_t *result_sink) {
  turbo_runnable_state_graph_adapter_t *adapter;
  turbo_runnable_config_t config;
  turbo_runnable_t *runnable;

  if (!graph) {
    return NULL;
  }

  adapter = (turbo_runnable_state_graph_adapter_t *)calloc(1, sizeof(*adapter));
  if (!adapter) {
    return NULL;
  }

  adapter->graph = graph;
  if (thread_id && thread_id[0] != '\0') {
    adapter->thread_id = turbo_runnable_strdup(thread_id);
    if (!adapter->thread_id) {
      free(adapter);
      return NULL;
    }
  }
  if (options) {
    adapter->options = *options;
  } else {
    memset(&adapter->options, 0, sizeof(adapter->options));
  }
  adapter->result_sink = result_sink;

  memset(&config, 0, sizeof(config));
  config.invoke_bind = turbo_runnable_state_graph_invoke_bind;
  config.invoke_bind_stream = turbo_runnable_state_graph_invoke_bind_stream;
  config.user_data = adapter;
  config.user_data_free = turbo_runnable_state_graph_adapter_free;

  runnable = turbo_runnable_create(&config);
  if (!runnable) {
    turbo_runnable_state_graph_adapter_free(adapter);
  }
  return runnable;
}

turbo_runnable_t *turbo_runnable_from_graph(turbo_graph_t *graph,
                                            const turbo_graph_run_options_t *options,
                                            turbo_graph_run_result_t *result_sink) {
  turbo_runnable_graph_adapter_t *adapter;
  turbo_runnable_config_t config;

  if (!graph) {
    return NULL;
  }

  adapter = (turbo_runnable_graph_adapter_t *)calloc(1, sizeof(*adapter));
  if (!adapter) {
    return NULL;
  }

  adapter->graph = graph;
  if (options) {
    adapter->options = *options;
  } else {
    memset(&adapter->options, 0, sizeof(adapter->options));
  }
  adapter->result_sink = result_sink;

  memset(&config, 0, sizeof(config));
  config.invoke_bind = turbo_runnable_graph_invoke_bind;
  config.invoke_bind_stream = turbo_runnable_graph_invoke_bind_stream;
  config.user_data = adapter;
  config.user_data_free = free;
  return turbo_runnable_create(&config);
}
