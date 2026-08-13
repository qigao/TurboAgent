#include "turbo_graph_run_log.h"

#include <stdlib.h>
#include <string.h>

struct turbo_graph_run_log_s {
  turbo_event_log_t *events;
  turbo_graph_checkpoint_t *checkpoint;
  turbo_graph_exec_status_t capture_status;
};

typedef struct {
  turbo_graph_run_log_t *log;
  turbo_graph_checkpoint_cb user_checkpoint_cb;
  void *user_checkpoint_data;
} turbo_graph_run_log_capture_ctx_t;

static turbo_graph_exec_status_t
turbo_graph_run_log_clone_checkpoint(const turbo_graph_checkpoint_t *checkpoint,
                                     turbo_graph_checkpoint_t **out_checkpoint) {
  char *serialized;
  size_t len = 0;
  turbo_graph_exec_status_t status;

  if (!checkpoint || !out_checkpoint) {
    return TURBO_GRAPH_EXEC_INVALID_ARGUMENT;
  }
  *out_checkpoint = NULL;

  serialized = turbo_graph_checkpoint_serialize(checkpoint, &len);
  if (!serialized) {
    return TURBO_GRAPH_EXEC_OUT_OF_MEMORY;
  }

  status = turbo_graph_checkpoint_deserialize(serialized, len, out_checkpoint);
  turbo_json_serialize_free(serialized);
  return status;
}

static turbo_graph_exec_status_t
turbo_graph_run_log_replace_checkpoint(turbo_graph_run_log_t *log,
                                       const turbo_graph_checkpoint_t *checkpoint) {
  turbo_graph_checkpoint_t *copy = NULL;
  turbo_graph_exec_status_t status;

  if (!log || !checkpoint) {
    return TURBO_GRAPH_EXEC_INVALID_ARGUMENT;
  }

  status = turbo_graph_run_log_clone_checkpoint(checkpoint, &copy);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  turbo_graph_checkpoint_destroy(log->checkpoint);
  log->checkpoint = copy;
  return TURBO_GRAPH_EXEC_OK;
}

static void turbo_graph_run_log_capture_checkpoint(const turbo_graph_checkpoint_t *checkpoint,
                                                   void *user_data) {
  turbo_graph_run_log_capture_ctx_t *capture = (turbo_graph_run_log_capture_ctx_t *)user_data;

  if (!capture || !capture->log || capture->log->capture_status != TURBO_GRAPH_EXEC_OK) {
    return;
  }

  capture->log->capture_status =
      turbo_graph_run_log_replace_checkpoint(capture->log, checkpoint);
  if (capture->user_checkpoint_cb) {
    capture->user_checkpoint_cb(checkpoint, capture->user_checkpoint_data);
  }
}

turbo_graph_run_log_t *turbo_graph_run_log_create(void) {
  turbo_graph_run_log_t *log = (turbo_graph_run_log_t *)calloc(1, sizeof(*log));

  if (!log) {
    return NULL;
  }

  log->events = turbo_event_log_create();
  if (!log->events) {
    free(log);
    return NULL;
  }

  log->capture_status = TURBO_GRAPH_EXEC_OK;
  return log;
}

void turbo_graph_run_log_destroy(turbo_graph_run_log_t *log) {
  if (!log) {
    return;
  }

  turbo_event_log_destroy(log->events);
  turbo_graph_checkpoint_destroy(log->checkpoint);
  free(log);
}

turbo_graph_exec_status_t turbo_graph_run_log_reset(turbo_graph_run_log_t *log) {
  turbo_event_log_t *events;

  if (!log) {
    return TURBO_GRAPH_EXEC_INVALID_ARGUMENT;
  }

  events = turbo_event_log_create();
  if (!events) {
    return TURBO_GRAPH_EXEC_OUT_OF_MEMORY;
  }

  turbo_event_log_destroy(log->events);
  log->events = events;
  turbo_graph_checkpoint_destroy(log->checkpoint);
  log->checkpoint = NULL;
  log->capture_status = TURBO_GRAPH_EXEC_OK;
  return TURBO_GRAPH_EXEC_OK;
}

const turbo_event_log_t *turbo_graph_run_log_events(const turbo_graph_run_log_t *log) {
  return log ? log->events : NULL;
}

const turbo_graph_checkpoint_t *turbo_graph_run_log_checkpoint(const turbo_graph_run_log_t *log) {
  return log ? log->checkpoint : NULL;
}

static turbo_graph_exec_status_t turbo_graph_run_log_finalize(
    turbo_graph_run_log_t *log, turbo_graph_exec_status_t status,
    json_value_t **out_state) {
  if (!log) {
    return TURBO_GRAPH_EXEC_INVALID_ARGUMENT;
  }

  if (log->capture_status != TURBO_GRAPH_EXEC_OK) {
    if (out_state && *out_state) {
      turbo_runtime_json_destroy(*out_state);
      *out_state = NULL;
    }
    return log->capture_status;
  }
  if (turbo_event_log_status(log->events) != TURBO_EVENT_LOG_OK) {
    if (out_state && *out_state) {
      turbo_runtime_json_destroy(*out_state);
      *out_state = NULL;
    }
    return TURBO_GRAPH_EXEC_OUT_OF_MEMORY;
  }
  return status;
}

turbo_graph_exec_status_t turbo_graph_run_json_value_log(
    turbo_graph_t *graph, const json_value_t *state,
    const turbo_graph_run_options_t *options, turbo_graph_run_log_t *log,
    turbo_graph_run_result_t *out_result, json_value_t **out_state) {
  turbo_graph_run_options_t effective_options;
  turbo_graph_run_log_capture_ctx_t capture;
  turbo_graph_exec_status_t status;

  if (!log) {
    return TURBO_GRAPH_EXEC_INVALID_ARGUMENT;
  }
  status = turbo_graph_run_log_reset(log);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  if (options) {
    effective_options = *options;
  } else {
    memset(&effective_options, 0, sizeof(effective_options));
  }
  capture.log = log;
  capture.user_checkpoint_cb = effective_options.checkpoint_cb;
  capture.user_checkpoint_data = effective_options.checkpoint_user_data;
  effective_options.checkpoint_cb = turbo_graph_run_log_capture_checkpoint;
  effective_options.checkpoint_user_data = &capture;

  status = turbo_graph_run_json_value_stream(graph, state, &effective_options, turbo_event_log_capture_json_value,
                                       log->events, out_result, out_state);
  return turbo_graph_run_log_finalize(log, status, out_state);
}

turbo_graph_exec_status_t turbo_graph_run_checkpoint_json_value_log(
    turbo_graph_t *graph, const turbo_graph_checkpoint_t *checkpoint,
    const turbo_graph_run_options_t *options, turbo_graph_run_log_t *log,
    turbo_graph_run_result_t *out_result, json_value_t **out_state) {
  turbo_graph_checkpoint_t *checkpoint_copy = NULL;
  turbo_graph_run_options_t effective_options;
  turbo_graph_run_log_capture_ctx_t capture;
  turbo_graph_exec_status_t status;

  if (!checkpoint || !log) {
    return TURBO_GRAPH_EXEC_INVALID_ARGUMENT;
  }
  status = turbo_graph_run_log_clone_checkpoint(checkpoint, &checkpoint_copy);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }
  status = turbo_graph_run_log_reset(log);
  if (status != TURBO_GRAPH_EXEC_OK) {
    turbo_graph_checkpoint_destroy(checkpoint_copy);
    return status;
  }

  if (options) {
    effective_options = *options;
  } else {
    memset(&effective_options, 0, sizeof(effective_options));
  }
  capture.log = log;
  capture.user_checkpoint_cb = effective_options.checkpoint_cb;
  capture.user_checkpoint_data = effective_options.checkpoint_user_data;
  effective_options.checkpoint_cb = turbo_graph_run_log_capture_checkpoint;
  effective_options.checkpoint_user_data = &capture;

  status = turbo_graph_run_checkpoint_json_value_stream(
      graph, checkpoint_copy, &effective_options, turbo_event_log_capture_json_value, log->events, out_result,
      out_state);
  turbo_graph_checkpoint_destroy(checkpoint_copy);
  return turbo_graph_run_log_finalize(log, status, out_state);
}
