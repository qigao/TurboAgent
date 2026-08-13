#include "turbo_event_log.h"

#include <stdlib.h>

struct turbo_event_log_s {
  json_value_t **events;
  size_t count;
  size_t capacity;
  turbo_event_log_status_t last_status;
};

static turbo_event_log_status_t turbo_event_log_reserve(turbo_event_log_t *log, size_t count) {
  json_value_t **events;
  size_t capacity;

  if (!log) {
    return TURBO_EVENT_LOG_INVALID_ARGUMENT;
  }
  if (count <= log->capacity) {
    return TURBO_EVENT_LOG_OK;
  }

  capacity = log->capacity ? log->capacity * 2 : 8;
  while (capacity < count) {
    capacity *= 2;
  }

  events = (json_value_t **)realloc(log->events, capacity * sizeof(*events));
  if (!events) {
    return TURBO_EVENT_LOG_OUT_OF_MEMORY;
  }

  log->events = events;
  log->capacity = capacity;
  return TURBO_EVENT_LOG_OK;
}

turbo_event_log_t *turbo_event_log_create(void) {
  turbo_event_log_t *log = (turbo_event_log_t *)calloc(1, sizeof(*log));

  if (!log) {
    return NULL;
  }

  log->last_status = TURBO_EVENT_LOG_OK;
  return log;
}

void turbo_event_log_destroy(turbo_event_log_t *log) {
  size_t i;

  if (!log) {
    return;
  }

  for (i = 0; i < log->count; ++i) {
    turbo_runtime_json_destroy(log->events[i]);
  }
  free(log->events);
  free(log);
}

turbo_event_log_status_t
turbo_event_log_append_json_value(turbo_event_log_t *log, const json_value_t *event) {
  json_value_t *copy;
  turbo_event_log_status_t status;

  if (!log || !event) {
    return TURBO_EVENT_LOG_INVALID_ARGUMENT;
  }
  if (turbo_event_validate_json_value(event) != 0) {
    log->last_status = TURBO_EVENT_LOG_INVALID_EVENT;
    return log->last_status;
  }

  status = turbo_event_log_reserve(log, log->count + 1);
  if (status != TURBO_EVENT_LOG_OK) {
    log->last_status = status;
    return log->last_status;
  }

  copy = turbo_json_clone(event);
  if (!copy) {
    log->last_status = TURBO_EVENT_LOG_OUT_OF_MEMORY;
    return log->last_status;
  }

  log->events[log->count++] = copy;
  log->last_status = TURBO_EVENT_LOG_OK;
  return log->last_status;
}

turbo_event_log_status_t turbo_event_log_append_events_json_value(
    turbo_event_log_t *log, const json_value_t *events) {
  json_value_t **copies = NULL;
  turbo_event_log_status_t status;
  size_t count;
  size_t i;

  if (!log || !events ||
      turbo_json_type(events) != TURBO_JSON_ARRAY) {
    return TURBO_EVENT_LOG_INVALID_ARGUMENT;
  }

  count = turbo_runtime_json_value_size(events);
  for (i = 0; i < count; ++i) {
    const json_value_t *event = turbo_json_array_get(events, i);

    if (!event || turbo_event_validate_json_value(event) != 0) {
      log->last_status = TURBO_EVENT_LOG_INVALID_EVENT;
      return log->last_status;
    }
  }

  status = turbo_event_log_reserve(log, log->count + count);
  if (status != TURBO_EVENT_LOG_OK) {
    log->last_status = status;
    return log->last_status;
  }

  if (count > 0) {
    copies = (json_value_t **)calloc(count, sizeof(*copies));
    if (!copies) {
      log->last_status = TURBO_EVENT_LOG_OUT_OF_MEMORY;
      return log->last_status;
    }
  }

  for (i = 0; i < count; ++i) {
    copies[i] = turbo_json_clone(turbo_json_array_get(events, i));
    if (!copies[i]) {
      size_t j;

      for (j = 0; j < i; ++j) {
        turbo_runtime_json_destroy(copies[j]);
      }
      free(copies);
      log->last_status = TURBO_EVENT_LOG_OUT_OF_MEMORY;
      return log->last_status;
    }
  }

  for (i = 0; i < count; ++i) {
    log->events[log->count++] = copies[i];
  }
  free(copies);

  log->last_status = TURBO_EVENT_LOG_OK;
  return log->last_status;
}

void turbo_event_log_capture_json_value(const json_value_t *event, void *user_data) {
  turbo_event_log_t *log = (turbo_event_log_t *)user_data;

  if (!log) {
    return;
  }

  log->last_status = turbo_event_log_append_json_value(log, event);
}

turbo_event_log_status_t turbo_event_log_status(const turbo_event_log_t *log) {
  if (!log) {
    return TURBO_EVENT_LOG_INVALID_ARGUMENT;
  }
  return log->last_status;
}

turbo_event_log_status_t turbo_event_log_reset(turbo_event_log_t *log) {
  size_t i;

  if (!log) {
    return TURBO_EVENT_LOG_INVALID_ARGUMENT;
  }

  for (i = 0; i < log->count; ++i) {
    turbo_runtime_json_destroy(log->events[i]);
    log->events[i] = NULL;
  }
  log->count = 0;
  log->last_status = TURBO_EVENT_LOG_OK;
  return TURBO_EVENT_LOG_OK;
}

turbo_event_log_status_t turbo_event_log_load_events_json_value(
    turbo_event_log_t *log, const json_value_t *events) {
  turbo_event_log_status_t status;

  status = turbo_event_log_reset(log);
  if (status != TURBO_EVENT_LOG_OK) {
    return status;
  }
  return turbo_event_log_append_events_json_value(log, events);
}

size_t turbo_event_log_size(const turbo_event_log_t *log) {
  if (!log) {
    return 0;
  }
  return log->count;
}

const json_value_t *turbo_event_log_get(const turbo_event_log_t *log,
                                                           size_t index) {
  if (!log || index >= log->count) {
    return NULL;
  }
  return log->events[index];
}

json_value_t *turbo_event_log_events_json_value(const turbo_event_log_t *log) {
  json_value_t *events;
  json_value_t *copy;
  size_t i;

  if (!log) {
    return NULL;
  }

  events = turbo_json_create_array();
  if (!events) {
    return NULL;
  }

  for (i = 0; i < log->count; ++i) {
    copy = turbo_json_clone(log->events[i]);
    if (!copy ||
        turbo_runtime_json_array_append(events, copy) != TURBO_RUNTIME_JSON_OK) {
      turbo_runtime_json_destroy(copy);
      turbo_runtime_json_destroy(events);
      return NULL;
    }
  }

  return events;
}

turbo_event_log_status_t turbo_event_log_replay_json_value(
    const turbo_event_log_t *log, turbo_event_sink_json_value_fn event_sink, void *event_sink_user_data) {
  size_t i;

  if (!log || !event_sink) {
    return TURBO_EVENT_LOG_INVALID_ARGUMENT;
  }

  for (i = 0; i < log->count; ++i) {
    event_sink(log->events[i], event_sink_user_data);
  }

  return TURBO_EVENT_LOG_OK;
}
