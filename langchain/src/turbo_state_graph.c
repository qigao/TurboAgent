#include "turbo_state_graph.h"

#include "turbo_agent_util_internal.h"
#include "turbo_parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TURBO_STATE_GRAPH_SNAPSHOT_SCHEMA_VERSION 1

typedef struct {
  char *name;
  char *description;
  turbo_state_graph_reducer_kind_t reducer;
  turbo_runtime_data_bind_value_kind_t value_kind;
  turbo_runtime_data_bind_value_t *default_value;
} turbo_state_graph_channel_entry_t;

typedef struct {
  char *name;
  turbo_state_graph_bind_node_fn bind_fn;
  void *user_data;
  void (*user_data_free)(void *user_data);
} turbo_state_graph_node_entry_t;

typedef struct {
  size_t from_index;
  size_t to_index;
  turbo_state_graph_bind_edge_predicate_fn predicate;
  void *user_data;
} turbo_state_graph_edge_entry_t;

typedef struct {
  char *id;
  char *current_run_id;
} turbo_state_graph_thread_entry_t;

typedef struct {
  char *id;
  char *checkpoint_id;
  char *thread_id;
  char *run_id;
  char *source_name;
  char *reason;
  char *last_node;
  char *next_node;
  turbo_state_graph_source_kind_t source_kind;
  size_t step;
  turbo_runtime_data_bind_value_t *update;
  turbo_runtime_data_bind_value_t *state;
} turbo_state_graph_history_entry_t;

typedef struct {
  char *id;
  char *thread_id;
  char *parent_run_id;
  char *forked_from_history_id;
  char *latest_history_entry_id;
  char *current_node;
  size_t steps;
  int interrupted;
  int completed;
  turbo_runtime_data_bind_value_t *state;
} turbo_state_graph_run_entry_t;

struct turbo_state_graph_pending_send_s {
  char *target_node;
  turbo_runtime_data_bind_value_t *update;
};

typedef struct {
  turbo_state_graph_t *child_graph;
  char *child_thread_id;
  char *child_thread_id_channel;
  char *output_channel;
  char **input_channels;
  size_t input_channel_count;
} turbo_state_graph_subgraph_node_data_t;

struct turbo_state_graph_s {
  char *name;
  char *entry_node;
  turbo_state_graph_channel_entry_t *channels;
  size_t channel_count;
  size_t channel_capacity;
  turbo_state_graph_node_entry_t *nodes;
  size_t node_count;
  size_t node_capacity;
  turbo_state_graph_edge_entry_t *edges;
  size_t edge_count;
  size_t edge_capacity;
  turbo_state_graph_thread_entry_t *threads;
  size_t thread_count;
  size_t thread_capacity;
  turbo_state_graph_run_entry_t *runs;
  size_t run_count;
  size_t run_capacity;
  turbo_state_graph_history_entry_t *history_entries;
  size_t history_count;
  size_t history_capacity;
  size_t next_thread_id;
  size_t next_run_id;
  size_t next_history_id;
  size_t next_checkpoint_id;
};

static turbo_state_graph_status_t turbo_state_graph_add_bind_node_owned(
    turbo_state_graph_t *graph, const char *name, turbo_state_graph_bind_node_fn fn,
    void *user_data, void (*user_data_free)(void *user_data));

static void turbo_state_graph_free_subgraph_node_data(void *user_data);
static void turbo_state_graph_free_pending_sends(turbo_state_graph_exec_ctx_t *ctx);
static turbo_state_graph_status_t turbo_state_graph_add_history_record(
    turbo_state_graph_t *graph, turbo_state_graph_run_entry_t *run,
    turbo_state_graph_source_kind_t source_kind, const char *source_name, const char *reason,
    const char *last_node, const char *next_node, size_t step,
    const turbo_runtime_data_bind_value_t *update,
    const turbo_runtime_data_bind_value_t *state,
    turbo_state_graph_history_entry_t **out_history_entry);

static void turbo_state_graph_result_init(turbo_state_graph_run_result_t *result,
                                          turbo_state_graph_status_t status,
                                          const char *thread_id, const char *run_id,
                                          const char *history_entry_id,
                                          const char *checkpoint_id,
                                          const char *last_node, const char *next_node,
                                          size_t steps) {
  if (!result) {
    return;
  }

  result->status = status;
  result->thread_id = thread_id;
  result->run_id = run_id;
  result->history_entry_id = history_entry_id;
  result->checkpoint_id = checkpoint_id;
  result->last_node = last_node;
  result->next_node = next_node;
  result->steps = steps;
}

static char *turbo_state_graph_strdup(const char *text) {
  return text ? turbo_agent_util_strdup(text) : NULL;
}

static void turbo_state_graph_str_free(char *text) {
  tstr_free(text);
}

static char *turbo_state_graph_make_id(const char *prefix, size_t value) {
  char buffer[64];

  if (!prefix) {
    return NULL;
  }

  snprintf(buffer, sizeof(buffer), "%s-%zu", prefix, value);
  return turbo_state_graph_strdup(buffer);
}

static int turbo_state_graph_bind_object_set_nullable_string(
    turbo_runtime_data_bind_value_t *object, const char *key, const char *value) {
  turbo_runtime_data_bind_value_t *field = NULL;

  if (!object || !key) {
    return -1;
  }

  field = value ? turbo_runtime_data_bind_value_create_string(value)
                : turbo_runtime_data_bind_value_create_null();
  if (!field) {
    return -1;
  }
  if (turbo_runtime_data_bind_object_set(object, key, field) !=
      TURBO_RUNTIME_DATA_BIND_OK) {
    turbo_runtime_data_bind_value_destroy(field);
    return -1;
  }
  return 0;
}

static void turbo_state_graph_free_channel(turbo_state_graph_channel_entry_t *channel) {
  if (!channel) {
    return;
  }

  turbo_state_graph_str_free(channel->name);
  turbo_state_graph_str_free(channel->description);
  turbo_runtime_data_bind_value_destroy(channel->default_value);
}

static void turbo_state_graph_free_node(turbo_state_graph_node_entry_t *node) {
  if (!node) {
    return;
  }

  turbo_state_graph_str_free(node->name);
  if (node->user_data_free && node->user_data) {
    node->user_data_free(node->user_data);
  }
}

static void turbo_state_graph_free_thread(turbo_state_graph_thread_entry_t *thread) {
  if (!thread) {
    return;
  }

  turbo_state_graph_str_free(thread->id);
  turbo_state_graph_str_free(thread->current_run_id);
}

static void turbo_state_graph_free_history_entry(
    turbo_state_graph_history_entry_t *history_entry) {
  if (!history_entry) {
    return;
  }

  turbo_state_graph_str_free(history_entry->id);
  turbo_state_graph_str_free(history_entry->checkpoint_id);
  turbo_state_graph_str_free(history_entry->thread_id);
  turbo_state_graph_str_free(history_entry->run_id);
  turbo_state_graph_str_free(history_entry->source_name);
  turbo_state_graph_str_free(history_entry->reason);
  turbo_state_graph_str_free(history_entry->last_node);
  turbo_state_graph_str_free(history_entry->next_node);
  turbo_runtime_data_bind_value_destroy(history_entry->update);
  turbo_runtime_data_bind_value_destroy(history_entry->state);
}

static void turbo_state_graph_free_run(turbo_state_graph_run_entry_t *run) {
  if (!run) {
    return;
  }

  turbo_state_graph_str_free(run->id);
  turbo_state_graph_str_free(run->thread_id);
  turbo_state_graph_str_free(run->parent_run_id);
  turbo_state_graph_str_free(run->forked_from_history_id);
  turbo_state_graph_str_free(run->latest_history_entry_id);
  turbo_state_graph_str_free(run->current_node);
  turbo_runtime_data_bind_value_destroy(run->state);
}

static const char *turbo_state_graph_source_kind_text(
    turbo_state_graph_source_kind_t source_kind) {
  switch (source_kind) {
    case TURBO_STATE_GRAPH_SOURCE_START:
      return "start";
    case TURBO_STATE_GRAPH_SOURCE_NODE:
      return "node";
    case TURBO_STATE_GRAPH_SOURCE_HOST:
      return "host";
    case TURBO_STATE_GRAPH_SOURCE_FORK:
      return "fork";
  }

  return "unknown";
}

static const char *turbo_state_graph_run_status_text(
    const turbo_state_graph_run_entry_t *run) {
  if (!run) {
    return "unknown";
  }
  if (run->completed) {
    return "completed";
  }
  if (run->interrupted) {
    return "interrupted";
  }
  return "running";
}

static const char *turbo_state_graph_status_text(turbo_state_graph_status_t status) {
  switch (status) {
    case TURBO_STATE_GRAPH_OK:
      return "completed";
    case TURBO_STATE_GRAPH_STOP:
      return "stopped";
    case TURBO_STATE_GRAPH_INTERRUPTED:
      return "interrupted";
    case TURBO_STATE_GRAPH_STEP_LIMIT:
      return "step_limit";
    case TURBO_STATE_GRAPH_RUN_COMPLETED:
      return "completed";
    default:
      return "error";
  }
}

static int turbo_state_graph_reserve(void **items, size_t *capacity, size_t count,
                                     size_t item_size) {
  void *grown;
  size_t next_capacity;

  if (!items || !capacity) {
    return 0;
  }
  if (*capacity >= count) {
    return 1;
  }

  next_capacity = *capacity == 0 ? 4 : (*capacity * 2);
  if (next_capacity < count) {
    next_capacity = count;
  }

  grown = realloc(*items, next_capacity * item_size);
  if (!grown) {
    return 0;
  }

  memset(((char *)grown) + (*capacity * item_size), 0,
         (next_capacity - *capacity) * item_size);
  *items = grown;
  *capacity = next_capacity;
  return 1;
}

static size_t turbo_state_graph_find_channel_index(const turbo_state_graph_t *graph,
                                                   const char *name) {
  size_t i;

  if (!graph || !name) {
    return (size_t)-1;
  }

  for (i = 0; i < graph->channel_count; ++i) {
    if (strcmp(graph->channels[i].name, name) == 0) {
      return i;
    }
  }

  return (size_t)-1;
}

static size_t turbo_state_graph_find_node_index(const turbo_state_graph_t *graph,
                                                const char *name) {
  size_t i;

  if (!graph || !name) {
    return (size_t)-1;
  }

  for (i = 0; i < graph->node_count; ++i) {
    if (strcmp(graph->nodes[i].name, name) == 0) {
      return i;
    }
  }

  return (size_t)-1;
}

static turbo_state_graph_thread_entry_t *
turbo_state_graph_find_thread(turbo_state_graph_t *graph, const char *thread_id) {
  size_t i;

  if (!graph || !thread_id) {
    return NULL;
  }

  for (i = 0; i < graph->thread_count; ++i) {
    if (strcmp(graph->threads[i].id, thread_id) == 0) {
      return &graph->threads[i];
    }
  }

  return NULL;
}

static turbo_state_graph_run_entry_t *turbo_state_graph_latest_run_for_thread(
    turbo_state_graph_t *graph, const char *thread_id) {
  turbo_state_graph_run_entry_t *latest = NULL;
  size_t i;

  if (!graph || !thread_id) {
    return NULL;
  }

  for (i = 0; i < graph->run_count; ++i) {
    turbo_state_graph_run_entry_t *candidate = &graph->runs[i];
    if (strcmp(candidate->thread_id, thread_id) != 0) {
      continue;
    }
    latest = candidate;
  }

  return latest;
}

static turbo_state_graph_run_entry_t *turbo_state_graph_pending_run_for_thread(
    turbo_state_graph_t *graph, const char *thread_id) {
  size_t i;

  if (!graph || !thread_id) {
    return NULL;
  }

  for (i = graph->run_count; i > 0; --i) {
    turbo_state_graph_run_entry_t *candidate = &graph->runs[i - 1];
    if (strcmp(candidate->thread_id, thread_id) != 0) {
      continue;
    }
    if (candidate->interrupted) {
      return candidate;
    }
  }

  return NULL;
}

static turbo_state_graph_run_entry_t *
turbo_state_graph_find_run(turbo_state_graph_t *graph, const char *run_id) {
  size_t i;

  if (!graph || !run_id) {
    return NULL;
  }

  for (i = 0; i < graph->run_count; ++i) {
    if (strcmp(graph->runs[i].id, run_id) == 0) {
      return &graph->runs[i];
    }
  }

  return NULL;
}

static turbo_state_graph_history_entry_t *turbo_state_graph_find_history(
    turbo_state_graph_t *graph, const char *history_entry_id) {
  size_t i;

  if (!graph || !history_entry_id) {
    return NULL;
  }

  for (i = 0; i < graph->history_count; ++i) {
    if (strcmp(graph->history_entries[i].id, history_entry_id) == 0) {
      return &graph->history_entries[i];
    }
  }

  return NULL;
}

static turbo_state_graph_history_entry_t *turbo_state_graph_find_checkpoint(
    turbo_state_graph_t *graph, const char *checkpoint_id) {
  size_t i;

  if (!graph || !checkpoint_id) {
    return NULL;
  }

  for (i = 0; i < graph->history_count; ++i) {
    if (strcmp(graph->history_entries[i].checkpoint_id, checkpoint_id) == 0) {
      return &graph->history_entries[i];
    }
  }

  return NULL;
}

static void turbo_state_graph_clear_runtime_state(turbo_state_graph_t *graph) {
  size_t i;

  if (!graph) {
    return;
  }

  for (i = 0; i < graph->thread_count; ++i) {
    turbo_state_graph_free_thread(&graph->threads[i]);
  }
  for (i = 0; i < graph->run_count; ++i) {
    turbo_state_graph_free_run(&graph->runs[i]);
  }
  for (i = 0; i < graph->history_count; ++i) {
    turbo_state_graph_free_history_entry(&graph->history_entries[i]);
  }

  free(graph->threads);
  free(graph->runs);
  free(graph->history_entries);
  graph->threads = NULL;
  graph->runs = NULL;
  graph->history_entries = NULL;
  graph->thread_count = 0;
  graph->run_count = 0;
  graph->history_count = 0;
  graph->thread_capacity = 0;
  graph->run_capacity = 0;
  graph->history_capacity = 0;
  graph->next_thread_id = 0;
  graph->next_run_id = 0;
  graph->next_history_id = 0;
  graph->next_checkpoint_id = 0;
}

static int turbo_state_graph_set_nullable_string_field(
    turbo_runtime_data_bind_value_t *object, const char *key, const char *value) {
  return turbo_state_graph_bind_object_set_nullable_string(object, key, value);
}

static turbo_state_graph_status_t turbo_state_graph_channel_object(
    const turbo_state_graph_channel_entry_t *channel, turbo_runtime_data_bind_value_t **out_object) {
  turbo_runtime_data_bind_value_t *object = NULL;

  if (!channel || !out_object) {
    return TURBO_STATE_GRAPH_INVALID_ARGUMENT;
  }

  object = turbo_runtime_data_bind_value_create_object();
  if (!object) {
    return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
  }

  if (turbo_agent_util_bind_object_set_string(object, "name", channel->name) != 0 ||
      turbo_agent_util_bind_object_set_int64(object, "reducer", (int64_t)channel->reducer) != 0 ||
      turbo_agent_util_bind_object_set_int64(object, "value_kind",
                                             (int64_t)channel->value_kind) != 0 ||
      turbo_state_graph_set_nullable_string_field(object, "description",
                                                  channel->description) != 0 ||
      turbo_agent_util_bind_object_set_clone(object, "default_value",
                                             channel->default_value) != 0) {
    turbo_runtime_data_bind_value_destroy(object);
    return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
  }

  *out_object = object;
  return TURBO_STATE_GRAPH_OK;
}

static turbo_state_graph_status_t turbo_state_graph_topology_snapshot(
    const turbo_state_graph_t *graph, turbo_runtime_data_bind_value_t **out_object) {
  turbo_runtime_data_bind_value_t *root = NULL;
  turbo_runtime_data_bind_value_t *channels = NULL;
  turbo_runtime_data_bind_value_t *nodes = NULL;
  turbo_runtime_data_bind_value_t *edges = NULL;
  size_t i;

  if (!graph || !out_object) {
    return TURBO_STATE_GRAPH_INVALID_ARGUMENT;
  }

  root = turbo_runtime_data_bind_value_create_object();
  channels = turbo_runtime_data_bind_value_create_array();
  nodes = turbo_runtime_data_bind_value_create_array();
  edges = turbo_runtime_data_bind_value_create_array();
  if (!root || !channels || !nodes || !edges) {
    turbo_runtime_data_bind_value_destroy(root);
    turbo_runtime_data_bind_value_destroy(channels);
    turbo_runtime_data_bind_value_destroy(nodes);
    turbo_runtime_data_bind_value_destroy(edges);
    return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
  }

  if (turbo_state_graph_set_nullable_string_field(root, "graph_name", graph->name) != 0 ||
      turbo_state_graph_set_nullable_string_field(root, "entry_node", graph->entry_node) != 0) {
    turbo_runtime_data_bind_value_destroy(root);
    turbo_runtime_data_bind_value_destroy(channels);
    turbo_runtime_data_bind_value_destroy(nodes);
    turbo_runtime_data_bind_value_destroy(edges);
    return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
  }

  for (i = 0; i < graph->channel_count; ++i) {
    turbo_runtime_data_bind_value_t *channel = NULL;
    if (turbo_state_graph_channel_object(&graph->channels[i], &channel) !=
        TURBO_STATE_GRAPH_OK ||
        turbo_runtime_data_bind_array_append(channels, channel) != TURBO_RUNTIME_DATA_BIND_OK) {
      turbo_runtime_data_bind_value_destroy(channel);
      turbo_runtime_data_bind_value_destroy(root);
      turbo_runtime_data_bind_value_destroy(channels);
      turbo_runtime_data_bind_value_destroy(nodes);
      turbo_runtime_data_bind_value_destroy(edges);
      return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
    }
  }

  for (i = 0; i < graph->node_count; ++i) {
    turbo_runtime_data_bind_value_t *node = turbo_runtime_data_bind_value_create_string(
        graph->nodes[i].name);
    if (!node || turbo_runtime_data_bind_array_append(nodes, node) !=
                     TURBO_RUNTIME_DATA_BIND_OK) {
      turbo_runtime_data_bind_value_destroy(node);
      turbo_runtime_data_bind_value_destroy(root);
      turbo_runtime_data_bind_value_destroy(channels);
      turbo_runtime_data_bind_value_destroy(nodes);
      turbo_runtime_data_bind_value_destroy(edges);
      return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
    }
  }

  for (i = 0; i < graph->edge_count; ++i) {
    turbo_runtime_data_bind_value_t *edge = turbo_runtime_data_bind_value_create_object();
    if (!edge ||
        turbo_agent_util_bind_object_set_string(edge, "from",
                                                graph->nodes[graph->edges[i].from_index].name) != 0 ||
        turbo_agent_util_bind_object_set_string(edge, "to",
                                                graph->nodes[graph->edges[i].to_index].name) != 0 ||
        turbo_runtime_data_bind_array_append(edges, edge) != TURBO_RUNTIME_DATA_BIND_OK) {
      turbo_runtime_data_bind_value_destroy(edge);
      turbo_runtime_data_bind_value_destroy(root);
      turbo_runtime_data_bind_value_destroy(channels);
      turbo_runtime_data_bind_value_destroy(nodes);
      turbo_runtime_data_bind_value_destroy(edges);
      return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
    }
  }

  if (turbo_runtime_data_bind_object_set(root, "channels", channels) !=
          TURBO_RUNTIME_DATA_BIND_OK ||
      turbo_runtime_data_bind_object_set(root, "nodes", nodes) != TURBO_RUNTIME_DATA_BIND_OK ||
      turbo_runtime_data_bind_object_set(root, "edges", edges) != TURBO_RUNTIME_DATA_BIND_OK) {
    turbo_runtime_data_bind_value_destroy(channels);
    turbo_runtime_data_bind_value_destroy(nodes);
    turbo_runtime_data_bind_value_destroy(edges);
    turbo_runtime_data_bind_value_destroy(root);
    return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
  }

  *out_object = root;
  return TURBO_STATE_GRAPH_OK;
}

static turbo_state_graph_status_t
turbo_state_graph_validate_channel_value(const turbo_state_graph_channel_entry_t *channel,
                                         const turbo_runtime_data_bind_value_t *value) {
  size_t i;

  if (!channel || !value) {
    return TURBO_STATE_GRAPH_INVALID_ARGUMENT;
  }

  if (turbo_runtime_data_bind_value_kind(value) == TURBO_RUNTIME_DATA_BIND_VALUE_NULL ||
      channel->value_kind == TURBO_RUNTIME_DATA_BIND_VALUE_NULL) {
    return TURBO_STATE_GRAPH_OK;
  }

  if (channel->reducer == TURBO_STATE_GRAPH_REDUCER_APPEND &&
      turbo_runtime_data_bind_value_kind(value) == TURBO_RUNTIME_DATA_BIND_VALUE_ARRAY) {
    for (i = 0; i < turbo_runtime_data_bind_value_size(value); ++i) {
      const turbo_runtime_data_bind_value_t *child =
          turbo_runtime_data_bind_array_get(value, i);
      if (child && turbo_runtime_data_bind_value_kind(child) != channel->value_kind &&
          turbo_runtime_data_bind_value_kind(child) != TURBO_RUNTIME_DATA_BIND_VALUE_NULL) {
        return TURBO_STATE_GRAPH_TYPE_MISMATCH;
      }
    }
    return TURBO_STATE_GRAPH_OK;
  }

  if (turbo_runtime_data_bind_value_kind(value) != channel->value_kind) {
    return TURBO_STATE_GRAPH_TYPE_MISMATCH;
  }

  return TURBO_STATE_GRAPH_OK;
}

static turbo_runtime_data_bind_value_t *
turbo_state_graph_clone_or_null(const turbo_runtime_data_bind_value_t *value) {
  if (!value) {
    return turbo_runtime_data_bind_value_create_null();
  }
  return turbo_runtime_data_bind_value_clone(value);
}

static turbo_runtime_data_bind_value_t *turbo_state_graph_channel_base_value(
    const turbo_state_graph_channel_entry_t *channel,
    const turbo_runtime_data_bind_value_t *current_value) {
  if (current_value) {
    return turbo_runtime_data_bind_value_clone(current_value);
  }
  if (channel && channel->default_value) {
    return turbo_runtime_data_bind_value_clone(channel->default_value);
  }
  return turbo_runtime_data_bind_value_create_null();
}

static turbo_state_graph_status_t turbo_state_graph_merge_object_values(
    const turbo_runtime_data_bind_value_t *current_value,
    const turbo_runtime_data_bind_value_t *update_value,
    turbo_runtime_data_bind_value_t **out_value) {
  turbo_runtime_data_bind_value_t *result = NULL;
  size_t i;

  if (!update_value || !out_value) {
    return TURBO_STATE_GRAPH_INVALID_ARGUMENT;
  }

  if (turbo_runtime_data_bind_value_kind(update_value) !=
      TURBO_RUNTIME_DATA_BIND_VALUE_OBJECT) {
    return TURBO_STATE_GRAPH_TYPE_MISMATCH;
  }

  if (current_value &&
      turbo_runtime_data_bind_value_kind(current_value) != TURBO_RUNTIME_DATA_BIND_VALUE_OBJECT &&
      turbo_runtime_data_bind_value_kind(current_value) != TURBO_RUNTIME_DATA_BIND_VALUE_NULL) {
    return TURBO_STATE_GRAPH_TYPE_MISMATCH;
  }

  if (current_value &&
      turbo_runtime_data_bind_value_kind(current_value) == TURBO_RUNTIME_DATA_BIND_VALUE_OBJECT) {
    result = turbo_runtime_data_bind_value_clone(current_value);
  } else {
    result = turbo_runtime_data_bind_value_create_object();
  }
  if (!result) {
    return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
  }

  for (i = 0; i < turbo_runtime_data_bind_value_size(update_value); ++i) {
    const char *key = turbo_runtime_data_bind_object_key_at(update_value, i);
    const turbo_runtime_data_bind_value_t *update_child =
        turbo_runtime_data_bind_object_get(update_value, key);
    const turbo_runtime_data_bind_value_t *current_child =
        turbo_runtime_data_bind_object_get(result, key);
    turbo_runtime_data_bind_value_t *merged_child = NULL;
    turbo_state_graph_status_t status;

    if (!key || !update_child) {
      turbo_runtime_data_bind_value_destroy(result);
      return TURBO_STATE_GRAPH_INVALID_UPDATE;
    }

    if (current_child &&
        turbo_runtime_data_bind_value_kind(current_child) ==
            TURBO_RUNTIME_DATA_BIND_VALUE_OBJECT &&
        turbo_runtime_data_bind_value_kind(update_child) ==
            TURBO_RUNTIME_DATA_BIND_VALUE_OBJECT) {
      status = turbo_state_graph_merge_object_values(current_child, update_child, &merged_child);
    } else {
      merged_child = turbo_runtime_data_bind_value_clone(update_child);
      status = merged_child ? TURBO_STATE_GRAPH_OK : TURBO_STATE_GRAPH_OUT_OF_MEMORY;
    }

    if (status != TURBO_STATE_GRAPH_OK) {
      turbo_runtime_data_bind_value_destroy(result);
      return status;
    }

    if (turbo_runtime_data_bind_object_set(result, key, merged_child) !=
        TURBO_RUNTIME_DATA_BIND_OK) {
      turbo_runtime_data_bind_value_destroy(merged_child);
      turbo_runtime_data_bind_value_destroy(result);
      return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
    }
  }

  *out_value = result;
  return TURBO_STATE_GRAPH_OK;
}

static turbo_state_graph_status_t turbo_state_graph_reduce_channel(
    const turbo_state_graph_channel_entry_t *channel,
    const turbo_runtime_data_bind_value_t *current_value,
    const turbo_runtime_data_bind_value_t *update_value,
    turbo_runtime_data_bind_value_t **out_value) {
  turbo_runtime_data_bind_value_t *result = NULL;
  turbo_state_graph_status_t status;
  size_t i;

  if (!channel || !update_value || !out_value) {
    return TURBO_STATE_GRAPH_INVALID_ARGUMENT;
  }

  status = turbo_state_graph_validate_channel_value(channel, update_value);
  if (status != TURBO_STATE_GRAPH_OK &&
      channel->reducer != TURBO_STATE_GRAPH_REDUCER_APPEND &&
      channel->reducer != TURBO_STATE_GRAPH_REDUCER_MERGE_OBJECT &&
      channel->reducer != TURBO_STATE_GRAPH_REDUCER_ADD) {
    return status;
  }

  switch (channel->reducer) {
    case TURBO_STATE_GRAPH_REDUCER_REPLACE:
      result = turbo_runtime_data_bind_value_clone(update_value);
      return result ? (*out_value = result, TURBO_STATE_GRAPH_OK)
                    : TURBO_STATE_GRAPH_OUT_OF_MEMORY;
    case TURBO_STATE_GRAPH_REDUCER_LAST_NON_NULL:
      if (turbo_runtime_data_bind_value_kind(update_value) ==
          TURBO_RUNTIME_DATA_BIND_VALUE_NULL) {
        result = turbo_state_graph_channel_base_value(channel, current_value);
      } else {
        result = turbo_runtime_data_bind_value_clone(update_value);
      }
      return result ? (*out_value = result, TURBO_STATE_GRAPH_OK)
                    : TURBO_STATE_GRAPH_OUT_OF_MEMORY;
    case TURBO_STATE_GRAPH_REDUCER_APPEND:
      if (current_value &&
          turbo_runtime_data_bind_value_kind(current_value) != TURBO_RUNTIME_DATA_BIND_VALUE_ARRAY &&
          turbo_runtime_data_bind_value_kind(current_value) != TURBO_RUNTIME_DATA_BIND_VALUE_NULL) {
        return TURBO_STATE_GRAPH_TYPE_MISMATCH;
      }
      result =
          (current_value &&
           turbo_runtime_data_bind_value_kind(current_value) == TURBO_RUNTIME_DATA_BIND_VALUE_ARRAY)
              ? turbo_runtime_data_bind_value_clone(current_value)
              : turbo_runtime_data_bind_value_create_array();
      if (!result) {
        return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
      }
      if (turbo_runtime_data_bind_value_kind(update_value) ==
          TURBO_RUNTIME_DATA_BIND_VALUE_ARRAY) {
        for (i = 0; i < turbo_runtime_data_bind_value_size(update_value); ++i) {
          const turbo_runtime_data_bind_value_t *child =
              turbo_runtime_data_bind_array_get(update_value, i);
          turbo_runtime_data_bind_value_t *child_copy = NULL;

          status = turbo_state_graph_validate_channel_value(channel, child);
          if (status != TURBO_STATE_GRAPH_OK) {
            turbo_runtime_data_bind_value_destroy(result);
            return status;
          }

          child_copy = turbo_runtime_data_bind_value_clone(child);
          if (!child_copy) {
            turbo_runtime_data_bind_value_destroy(result);
            return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
          }
          if (turbo_runtime_data_bind_array_append(result, child_copy) !=
              TURBO_RUNTIME_DATA_BIND_OK) {
            turbo_runtime_data_bind_value_destroy(child_copy);
            turbo_runtime_data_bind_value_destroy(result);
            return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
          }
        }
      } else {
        turbo_runtime_data_bind_value_t *child_copy = NULL;

        status = turbo_state_graph_validate_channel_value(channel, update_value);
        if (status != TURBO_STATE_GRAPH_OK) {
          turbo_runtime_data_bind_value_destroy(result);
          return status;
        }

        child_copy = turbo_runtime_data_bind_value_clone(update_value);
        if (!child_copy) {
          turbo_runtime_data_bind_value_destroy(result);
          return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
        }
        if (turbo_runtime_data_bind_array_append(result, child_copy) !=
            TURBO_RUNTIME_DATA_BIND_OK) {
          turbo_runtime_data_bind_value_destroy(child_copy);
          turbo_runtime_data_bind_value_destroy(result);
          return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
        }
      }
      *out_value = result;
      return TURBO_STATE_GRAPH_OK;
    case TURBO_STATE_GRAPH_REDUCER_MERGE_OBJECT:
      return turbo_state_graph_merge_object_values(current_value, update_value, out_value);
    case TURBO_STATE_GRAPH_REDUCER_ADD: {
      int current_is_double = 0;
      int update_is_double = 0;
      double sum_double;
      int64_t sum_int64;

      if (update_value &&
          turbo_runtime_data_bind_value_kind(update_value) != TURBO_RUNTIME_DATA_BIND_VALUE_INT64 &&
          turbo_runtime_data_bind_value_kind(update_value) != TURBO_RUNTIME_DATA_BIND_VALUE_DOUBLE) {
        return TURBO_STATE_GRAPH_TYPE_MISMATCH;
      }
      if (current_value &&
          turbo_runtime_data_bind_value_kind(current_value) != TURBO_RUNTIME_DATA_BIND_VALUE_INT64 &&
          turbo_runtime_data_bind_value_kind(current_value) != TURBO_RUNTIME_DATA_BIND_VALUE_DOUBLE &&
          turbo_runtime_data_bind_value_kind(current_value) != TURBO_RUNTIME_DATA_BIND_VALUE_NULL) {
        return TURBO_STATE_GRAPH_TYPE_MISMATCH;
      }

      current_is_double =
          current_value &&
          turbo_runtime_data_bind_value_kind(current_value) == TURBO_RUNTIME_DATA_BIND_VALUE_DOUBLE;
      update_is_double =
          turbo_runtime_data_bind_value_kind(update_value) == TURBO_RUNTIME_DATA_BIND_VALUE_DOUBLE;

      if (current_is_double || update_is_double ||
          channel->value_kind == TURBO_RUNTIME_DATA_BIND_VALUE_DOUBLE) {
        sum_double =
            current_is_double ? turbo_runtime_data_bind_value_as_double(current_value, 0.0)
                              : (double)turbo_runtime_data_bind_value_as_int64(current_value, 0);
        sum_double += update_is_double ? turbo_runtime_data_bind_value_as_double(update_value, 0.0)
                                       : (double)turbo_runtime_data_bind_value_as_int64(update_value, 0);
        result = turbo_runtime_data_bind_value_create_double(sum_double);
      } else {
        sum_int64 = turbo_runtime_data_bind_value_as_int64(current_value, 0);
        sum_int64 += turbo_runtime_data_bind_value_as_int64(update_value, 0);
        result = turbo_runtime_data_bind_value_create_int64(sum_int64);
      }
      if (!result) {
        return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
      }
      *out_value = result;
      return TURBO_STATE_GRAPH_OK;
    }
  }

  return TURBO_STATE_GRAPH_INVALID_ARGUMENT;
}

static turbo_state_graph_status_t turbo_state_graph_apply_update(
    turbo_state_graph_t *graph, turbo_runtime_data_bind_value_t *state,
    const turbo_runtime_data_bind_value_t *update) {
  size_t i;

  if (!graph || !state || !update) {
    return TURBO_STATE_GRAPH_INVALID_ARGUMENT;
  }
  if (turbo_runtime_data_bind_value_kind(state) != TURBO_RUNTIME_DATA_BIND_VALUE_OBJECT ||
      turbo_runtime_data_bind_value_kind(update) != TURBO_RUNTIME_DATA_BIND_VALUE_OBJECT) {
    return TURBO_STATE_GRAPH_TYPE_MISMATCH;
  }

  for (i = 0; i < turbo_runtime_data_bind_value_size(update); ++i) {
    const char *key = turbo_runtime_data_bind_object_key_at(update, i);
    const turbo_runtime_data_bind_value_t *update_value =
        turbo_runtime_data_bind_object_get(update, key);
    const turbo_runtime_data_bind_value_t *current_value =
        turbo_runtime_data_bind_object_get(state, key);
    turbo_runtime_data_bind_value_t *merged_value = NULL;
    turbo_state_graph_status_t status;
    size_t channel_index;

    if (!key || !update_value) {
      return TURBO_STATE_GRAPH_INVALID_UPDATE;
    }

    channel_index = turbo_state_graph_find_channel_index(graph, key);
    if (channel_index == (size_t)-1) {
      return TURBO_STATE_GRAPH_CHANNEL_NOT_FOUND;
    }

    status = turbo_state_graph_reduce_channel(&graph->channels[channel_index], current_value,
                                              update_value, &merged_value);
    if (status != TURBO_STATE_GRAPH_OK) {
      return status;
    }

    if (turbo_runtime_data_bind_object_set(state, key, merged_value) !=
        TURBO_RUNTIME_DATA_BIND_OK) {
      turbo_runtime_data_bind_value_destroy(merged_value);
      return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
    }
  }

  return TURBO_STATE_GRAPH_OK;
}

static void turbo_state_graph_free_pending_sends(turbo_state_graph_exec_ctx_t *ctx) {
  size_t i;

  if (!ctx || !ctx->sends) {
    return;
  }
  for (i = 0; i < ctx->send_count; ++i) {
    turbo_state_graph_str_free(ctx->sends[i].target_node);
    turbo_runtime_data_bind_value_destroy(ctx->sends[i].update);
  }
  free(ctx->sends);
  ctx->sends = NULL;
  ctx->send_count = 0;
  ctx->send_capacity = 0;
}

static turbo_state_graph_status_t turbo_state_graph_execute_send_node(
    turbo_state_graph_t *graph, turbo_state_graph_run_entry_t *run,
    const turbo_state_graph_pending_send_t *send, size_t step,
    turbo_state_graph_history_entry_t **latest_history) {
  size_t target_index;
  turbo_runtime_data_bind_value_t *branch_update = NULL;
  turbo_runtime_data_bind_value_t *node_update = NULL;
  turbo_state_graph_exec_ctx_t branch_ctx;
  turbo_state_graph_status_t status;
  int callback_status;

  if (!graph || !run || !send || !send->target_node || !send->update || !latest_history) {
    return TURBO_STATE_GRAPH_INVALID_ARGUMENT;
  }
  if (turbo_runtime_data_bind_value_kind(send->update) != TURBO_RUNTIME_DATA_BIND_VALUE_OBJECT) {
    return TURBO_STATE_GRAPH_TYPE_MISMATCH;
  }
  target_index = turbo_state_graph_find_node_index(graph, send->target_node);
  if (target_index == (size_t)-1) {
    return TURBO_STATE_GRAPH_NODE_NOT_FOUND;
  }

  branch_update = turbo_runtime_data_bind_value_clone(send->update);
  if (!branch_update) {
    return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
  }
  status = turbo_state_graph_apply_update(graph, run->state, branch_update);
  if (status != TURBO_STATE_GRAPH_OK) {
    turbo_runtime_data_bind_value_destroy(branch_update);
    return status;
  }
  status = turbo_state_graph_add_history_record(
      graph, run, TURBO_STATE_GRAPH_SOURCE_NODE, send->target_node, "send", NULL,
      send->target_node, step, branch_update, run->state, latest_history);
  turbo_runtime_data_bind_value_destroy(branch_update);
  if (status != TURBO_STATE_GRAPH_OK) {
    return status;
  }

  node_update = turbo_runtime_data_bind_value_create_object();
  if (!node_update) {
    return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
  }
  memset(&branch_ctx, 0, sizeof(branch_ctx));
  branch_ctx.graph = graph;
  branch_ctx.state = run->state;
  branch_ctx.update = node_update;
  branch_ctx.current_node = graph->nodes[target_index].name;
  branch_ctx.step = step;

  callback_status =
      graph->nodes[target_index].bind_fn(&branch_ctx, graph->nodes[target_index].user_data);
  if (callback_status != 0 || branch_ctx.send_count > 0 || branch_ctx.next_node ||
      branch_ctx.stop) {
    turbo_state_graph_free_pending_sends(&branch_ctx);
    turbo_runtime_data_bind_value_destroy(node_update);
    return TURBO_STATE_GRAPH_ERROR;
  }
  status = turbo_state_graph_apply_update(graph, run->state, node_update);
  if (status != TURBO_STATE_GRAPH_OK) {
    turbo_runtime_data_bind_value_destroy(node_update);
    return status;
  }
  status = turbo_state_graph_add_history_record(
      graph, run, TURBO_STATE_GRAPH_SOURCE_NODE, graph->nodes[target_index].name, "send_target",
      graph->nodes[target_index].name, NULL, step, node_update, run->state, latest_history);
  turbo_runtime_data_bind_value_destroy(node_update);
  return status;
}

static turbo_state_graph_status_t turbo_state_graph_seed_defaults(
    turbo_state_graph_t *graph, turbo_runtime_data_bind_value_t *state) {
  size_t i;

  if (!graph || !state) {
    return TURBO_STATE_GRAPH_INVALID_ARGUMENT;
  }

  for (i = 0; i < graph->channel_count; ++i) {
    turbo_runtime_data_bind_value_t *value =
        turbo_state_graph_channel_base_value(&graph->channels[i], NULL);
    if (!value) {
      return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
    }
    if (turbo_runtime_data_bind_object_set(state, graph->channels[i].name, value) !=
        TURBO_RUNTIME_DATA_BIND_OK) {
      turbo_runtime_data_bind_value_destroy(value);
      return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
    }
  }

  return TURBO_STATE_GRAPH_OK;
}

static int turbo_state_graph_should_interrupt_before(
    const turbo_state_graph_run_options_t *options, const char *node_name) {
  size_t i;

  if (!options || !node_name || !options->interrupt_before_nodes ||
      options->interrupt_before_count == 0) {
    return 0;
  }

  for (i = 0; i < options->interrupt_before_count; ++i) {
    const char *candidate = options->interrupt_before_nodes[i];
    if (candidate && strcmp(candidate, node_name) == 0) {
      return 1;
    }
  }

  return 0;
}

static turbo_state_graph_status_t turbo_state_graph_add_thread_record(
    turbo_state_graph_t *graph, const char *requested_thread_id,
    turbo_state_graph_thread_entry_t **out_thread) {
  turbo_state_graph_thread_entry_t *thread = NULL;
  char *thread_id_copy = NULL;

  if (!graph || !out_thread) {
    return TURBO_STATE_GRAPH_INVALID_ARGUMENT;
  }

  *out_thread = NULL;
  if (requested_thread_id && requested_thread_id[0] != '\0') {
    thread = turbo_state_graph_find_thread(graph, requested_thread_id);
    if (thread) {
      *out_thread = thread;
      return TURBO_STATE_GRAPH_OK;
    }
    thread_id_copy = turbo_state_graph_strdup(requested_thread_id);
  } else {
    graph->next_thread_id += 1;
    thread_id_copy = turbo_state_graph_make_id("thread", graph->next_thread_id);
  }

  if (!thread_id_copy) {
    return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
  }
  if (!turbo_state_graph_reserve((void **)&graph->threads, &graph->thread_capacity,
                                 graph->thread_count + 1, sizeof(*graph->threads))) {
    turbo_state_graph_str_free(thread_id_copy);
    return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
  }

  thread = &graph->threads[graph->thread_count++];
  thread->id = thread_id_copy;
  thread->current_run_id = NULL;
  *out_thread = thread;
  return TURBO_STATE_GRAPH_OK;
}

static turbo_state_graph_status_t turbo_state_graph_add_run_record(
    turbo_state_graph_t *graph, const char *thread_id, const char *parent_run_id,
    const char *forked_from_history_id, const turbo_runtime_data_bind_value_t *state,
    const char *current_node, size_t steps, turbo_state_graph_run_entry_t **out_run) {
  turbo_state_graph_run_entry_t *run = NULL;

  if (!graph || !thread_id || !state || !out_run) {
    return TURBO_STATE_GRAPH_INVALID_ARGUMENT;
  }

  if (!turbo_state_graph_reserve((void **)&graph->runs, &graph->run_capacity,
                                 graph->run_count + 1, sizeof(*graph->runs))) {
    return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
  }

  graph->next_run_id += 1;
  run = &graph->runs[graph->run_count++];
  run->id = turbo_state_graph_make_id("run", graph->next_run_id);
  run->thread_id = turbo_state_graph_strdup(thread_id);
  run->parent_run_id = turbo_state_graph_strdup(parent_run_id);
  run->forked_from_history_id = turbo_state_graph_strdup(forked_from_history_id);
  run->latest_history_entry_id = NULL;
  run->current_node = turbo_state_graph_strdup(current_node);
  run->steps = steps;
  run->interrupted = 0;
  run->completed = current_node ? 0 : 1;
  run->state = turbo_runtime_data_bind_value_clone(state);

  if (!run->id || !run->thread_id || !run->state ||
      (current_node && !run->current_node) ||
      (parent_run_id && !run->parent_run_id) ||
      (forked_from_history_id && !run->forked_from_history_id)) {
    turbo_state_graph_free_run(run);
    memset(run, 0, sizeof(*run));
    graph->run_count--;
    return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
  }

  *out_run = run;
  return TURBO_STATE_GRAPH_OK;
}

static turbo_state_graph_status_t turbo_state_graph_add_history_record(
    turbo_state_graph_t *graph, turbo_state_graph_run_entry_t *run,
    turbo_state_graph_source_kind_t source_kind, const char *source_name,
    const char *reason, const char *last_node, const char *next_node, size_t step,
    const turbo_runtime_data_bind_value_t *update,
    const turbo_runtime_data_bind_value_t *state,
    turbo_state_graph_history_entry_t **out_history_entry) {
  turbo_state_graph_history_entry_t *history_entry = NULL;
  turbo_runtime_data_bind_value_t *update_copy = NULL;
  turbo_runtime_data_bind_value_t *state_copy = NULL;

  if (!graph || !run || !state || !out_history_entry) {
    return TURBO_STATE_GRAPH_INVALID_ARGUMENT;
  }

  if (!turbo_state_graph_reserve((void **)&graph->history_entries, &graph->history_capacity,
                                 graph->history_count + 1,
                                 sizeof(*graph->history_entries))) {
    return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
  }

  update_copy = update ? turbo_runtime_data_bind_value_clone(update)
                       : turbo_runtime_data_bind_value_create_object();
  state_copy = turbo_runtime_data_bind_value_clone(state);
  if (!update_copy || !state_copy) {
    turbo_runtime_data_bind_value_destroy(update_copy);
    turbo_runtime_data_bind_value_destroy(state_copy);
    return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
  }

  graph->next_history_id += 1;
  graph->next_checkpoint_id += 1;
  history_entry = &graph->history_entries[graph->history_count++];
  history_entry->id = turbo_state_graph_make_id("history", graph->next_history_id);
  history_entry->checkpoint_id =
      turbo_state_graph_make_id("checkpoint", graph->next_checkpoint_id);
  history_entry->thread_id = turbo_state_graph_strdup(run->thread_id);
  history_entry->run_id = turbo_state_graph_strdup(run->id);
  history_entry->source_name = turbo_state_graph_strdup(source_name);
  history_entry->reason = turbo_state_graph_strdup(reason);
  history_entry->last_node = turbo_state_graph_strdup(last_node);
  history_entry->next_node = turbo_state_graph_strdup(next_node);
  history_entry->source_kind = source_kind;
  history_entry->step = step;
  history_entry->update = update_copy;
  history_entry->state = state_copy;

  if (!history_entry->id || !history_entry->checkpoint_id || !history_entry->thread_id ||
      !history_entry->run_id || (source_name && !history_entry->source_name) ||
      (reason && !history_entry->reason) || (last_node && !history_entry->last_node) ||
      (next_node && !history_entry->next_node)) {
    turbo_state_graph_free_history_entry(history_entry);
    memset(history_entry, 0, sizeof(*history_entry));
    graph->history_count--;
    return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
  }

  turbo_state_graph_str_free(run->latest_history_entry_id);
  run->latest_history_entry_id = turbo_state_graph_strdup(history_entry->id);
  if (!run->latest_history_entry_id) {
    turbo_state_graph_free_history_entry(history_entry);
    memset(history_entry, 0, sizeof(*history_entry));
    graph->history_count--;
    return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
  }

  *out_history_entry = history_entry;
  return TURBO_STATE_GRAPH_OK;
}

static turbo_state_graph_status_t
turbo_state_graph_history_object(const turbo_state_graph_history_entry_t *history_entry,
                                 turbo_runtime_data_bind_value_t **out_object) {
  turbo_runtime_data_bind_value_t *object = NULL;
  turbo_runtime_data_bind_value_t *value = NULL;

  if (!history_entry || !out_object) {
    return TURBO_STATE_GRAPH_INVALID_ARGUMENT;
  }

  object = turbo_runtime_data_bind_value_create_object();
  if (!object) {
    return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
  }

  if (turbo_agent_util_bind_object_set_string(object, "history_entry_id", history_entry->id) != 0 ||
      turbo_agent_util_bind_object_set_string(object, "checkpoint_id",
                                              history_entry->checkpoint_id) != 0 ||
      turbo_agent_util_bind_object_set_string(object, "thread_id", history_entry->thread_id) != 0 ||
      turbo_agent_util_bind_object_set_string(object, "run_id", history_entry->run_id) != 0 ||
      turbo_agent_util_bind_object_set_string(object, "source_kind",
                                              turbo_state_graph_source_kind_text(
                                                  history_entry->source_kind)) != 0 ||
      turbo_state_graph_bind_object_set_nullable_string(object, "source_name",
                                                        history_entry->source_name) != 0 ||
      turbo_state_graph_bind_object_set_nullable_string(object, "reason",
                                                        history_entry->reason) != 0 ||
      turbo_state_graph_bind_object_set_nullable_string(object, "last_node",
                                                        history_entry->last_node) != 0 ||
      turbo_state_graph_bind_object_set_nullable_string(object, "next_node",
                                                        history_entry->next_node) != 0 ||
      turbo_agent_util_bind_object_set_int64(object, "step",
                                             (int64_t)history_entry->step) != 0 ||
      turbo_agent_util_bind_object_set_clone(object, "update", history_entry->update) != 0 ||
      turbo_agent_util_bind_object_set_clone(object, "state", history_entry->state) != 0) {
    turbo_runtime_data_bind_value_destroy(object);
    return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
  }

  value = turbo_runtime_data_bind_value_create_bool(1);
  if (!value) {
    turbo_runtime_data_bind_value_destroy(object);
    return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
  }
  if (turbo_runtime_data_bind_object_set(object, "checkpointable", value) !=
      TURBO_RUNTIME_DATA_BIND_OK) {
    turbo_runtime_data_bind_value_destroy(value);
    turbo_runtime_data_bind_value_destroy(object);
    return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
  }

  *out_object = object;
  return TURBO_STATE_GRAPH_OK;
}

static turbo_state_graph_status_t turbo_state_graph_run_object(
    const turbo_state_graph_run_entry_t *run, turbo_runtime_data_bind_value_t **out_object) {
  turbo_runtime_data_bind_value_t *object = NULL;

  if (!run || !out_object) {
    return TURBO_STATE_GRAPH_INVALID_ARGUMENT;
  }

  object = turbo_runtime_data_bind_value_create_object();
  if (!object) {
    return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
  }

  if (turbo_agent_util_bind_object_set_string(object, "run_id", run->id) != 0 ||
      turbo_agent_util_bind_object_set_string(object, "thread_id", run->thread_id) != 0 ||
      turbo_state_graph_bind_object_set_nullable_string(object, "parent_run_id",
                                                        run->parent_run_id) != 0 ||
      turbo_state_graph_bind_object_set_nullable_string(object, "forked_from_history_id",
                                                        run->forked_from_history_id) != 0 ||
      turbo_state_graph_bind_object_set_nullable_string(object, "latest_history_entry_id",
                                                        run->latest_history_entry_id) != 0 ||
      turbo_state_graph_bind_object_set_nullable_string(object, "next_node",
                                                        run->current_node) != 0 ||
      turbo_agent_util_bind_object_set_string(object, "status",
                                              turbo_state_graph_run_status_text(run)) != 0 ||
      turbo_agent_util_bind_object_set_int64(object, "steps",
                                             (int64_t)run->steps) != 0 ||
      turbo_agent_util_bind_object_set_clone(object, "state", run->state) != 0) {
    turbo_runtime_data_bind_value_destroy(object);
    return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
  }

  *out_object = object;
  return TURBO_STATE_GRAPH_OK;
}

static turbo_state_graph_status_t turbo_state_graph_thread_object(
    const turbo_state_graph_thread_entry_t *thread,
    const turbo_runtime_data_bind_value_t *current_state,
    turbo_runtime_data_bind_value_t **out_object) {
  turbo_runtime_data_bind_value_t *object = NULL;

  if (!thread || !out_object) {
    return TURBO_STATE_GRAPH_INVALID_ARGUMENT;
  }

  object = turbo_runtime_data_bind_value_create_object();
  if (!object) {
    return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
  }

  if (turbo_agent_util_bind_object_set_string(object, "thread_id", thread->id) != 0 ||
      turbo_state_graph_bind_object_set_nullable_string(object, "current_run_id",
                                                        thread->current_run_id) != 0 ||
      (current_state &&
       turbo_agent_util_bind_object_set_clone(object, "current_state", current_state) != 0)) {
    turbo_runtime_data_bind_value_destroy(object);
    return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
  }

  *out_object = object;
  return TURBO_STATE_GRAPH_OK;
}

static turbo_state_graph_status_t turbo_state_graph_branch_tree_object(
    turbo_state_graph_t *graph, const turbo_state_graph_thread_entry_t *thread,
    turbo_runtime_data_bind_value_t **out_object) {
  turbo_runtime_data_bind_value_t *root = NULL;
  turbo_runtime_data_bind_value_t *branches = NULL;
  size_t i;

  if (!graph || !thread || !out_object) {
    return TURBO_STATE_GRAPH_INVALID_ARGUMENT;
  }

  root = turbo_runtime_data_bind_value_create_object();
  branches = turbo_runtime_data_bind_value_create_array();
  if (!root || !branches) {
    turbo_runtime_data_bind_value_destroy(root);
    turbo_runtime_data_bind_value_destroy(branches);
    return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
  }

  if (turbo_agent_util_bind_object_set_string(root, "thread_id", thread->id) != 0 ||
      turbo_state_graph_bind_object_set_nullable_string(root, "current_run_id",
                                                        thread->current_run_id) != 0) {
    turbo_runtime_data_bind_value_destroy(root);
    turbo_runtime_data_bind_value_destroy(branches);
    return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
  }

  for (i = 0; i < graph->run_count; ++i) {
    const turbo_state_graph_run_entry_t *run = &graph->runs[i];
    turbo_runtime_data_bind_value_t *branch = NULL;

    if (strcmp(run->thread_id, thread->id) != 0) {
      continue;
    }

    branch = turbo_runtime_data_bind_value_create_object();
    if (!branch) {
      turbo_runtime_data_bind_value_destroy(root);
      turbo_runtime_data_bind_value_destroy(branches);
      return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
    }

    if (turbo_agent_util_bind_object_set_string(branch, "run_id", run->id) != 0 ||
        turbo_state_graph_bind_object_set_nullable_string(branch, "parent_run_id",
                                                          run->parent_run_id) != 0 ||
        turbo_state_graph_bind_object_set_nullable_string(branch,
                                                          "forked_from_history_id",
                                                          run->forked_from_history_id) != 0 ||
        turbo_state_graph_bind_object_set_nullable_string(branch,
                                                          "latest_history_entry_id",
                                                          run->latest_history_entry_id) != 0 ||
        turbo_state_graph_bind_object_set_nullable_string(branch, "next_node",
                                                          run->current_node) != 0 ||
        turbo_agent_util_bind_object_set_string(branch, "status",
                                                turbo_state_graph_run_status_text(run)) != 0 ||
        turbo_agent_util_bind_object_set_int64(branch, "steps",
                                               (int64_t)run->steps) != 0 ||
        turbo_agent_util_bind_object_set_clone(branch, "state", run->state) != 0) {
      turbo_runtime_data_bind_value_destroy(branch);
      turbo_runtime_data_bind_value_destroy(root);
      turbo_runtime_data_bind_value_destroy(branches);
      return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
    }

    if (turbo_runtime_data_bind_array_append(branches, branch) !=
        TURBO_RUNTIME_DATA_BIND_OK) {
      turbo_runtime_data_bind_value_destroy(branch);
      turbo_runtime_data_bind_value_destroy(root);
      turbo_runtime_data_bind_value_destroy(branches);
      return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
    }
  }

  if (turbo_runtime_data_bind_object_set(root, "branches", branches) !=
      TURBO_RUNTIME_DATA_BIND_OK) {
    turbo_runtime_data_bind_value_destroy(branches);
    turbo_runtime_data_bind_value_destroy(root);
    return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
  }

  *out_object = root;
  return TURBO_STATE_GRAPH_OK;
}

static turbo_state_graph_status_t
turbo_state_graph_execute_run(turbo_state_graph_t *graph, turbo_state_graph_run_entry_t *run,
                              const turbo_state_graph_run_options_t *options,
                              turbo_state_graph_run_result_t *out_result,
                              turbo_runtime_data_bind_value_t **out_state) {
  const char *current_node = NULL;
  size_t steps;
  int skip_initial_interrupt;
  turbo_state_graph_history_entry_t *latest_history = NULL;

  if (!graph || !run || !out_state) {
    return TURBO_STATE_GRAPH_INVALID_ARGUMENT;
  }

  *out_state = NULL;
  current_node = options && options->start_node ? options->start_node : run->current_node;
  steps = run->steps;
  skip_initial_interrupt = options && options->skip_initial_interrupt ? 1 : 0;
  latest_history = turbo_state_graph_find_history(graph, run->latest_history_entry_id);

  if (!current_node) {
    run->completed = 1;
    run->interrupted = 0;
    turbo_state_graph_result_init(out_result, TURBO_STATE_GRAPH_RUN_COMPLETED, run->thread_id,
                                  run->id,
                                  latest_history ? latest_history->id : NULL,
                                  latest_history ? latest_history->checkpoint_id : NULL,
                                  latest_history ? latest_history->last_node : NULL, NULL, steps);
    return TURBO_STATE_GRAPH_RUN_COMPLETED;
  }

  while (current_node) {
    turbo_state_graph_exec_ctx_t ctx;
    turbo_runtime_data_bind_value_t *update = NULL;
    turbo_state_graph_status_t status;
    size_t current_index;
    const char *executed_node = NULL;
    const char *resolved_next = NULL;
    size_t i;
    int saw_outgoing = 0;
    int callback_status;

    if (options && options->max_steps > 0 && steps >= options->max_steps) {
      run->current_node = turbo_state_graph_strdup(current_node);
      run->interrupted = 0;
      run->completed = 0;
      turbo_state_graph_result_init(out_result, TURBO_STATE_GRAPH_STEP_LIMIT, run->thread_id,
                                    run->id,
                                    latest_history ? latest_history->id : NULL,
                                    latest_history ? latest_history->checkpoint_id : NULL,
                                    latest_history ? latest_history->last_node : NULL, current_node,
                                    steps);
      return TURBO_STATE_GRAPH_STEP_LIMIT;
    }

    if (!skip_initial_interrupt &&
        turbo_state_graph_should_interrupt_before(options, current_node)) {
      char *current_copy = turbo_state_graph_strdup(current_node);
      if (!current_copy) {
        return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
      }
      turbo_state_graph_str_free(run->current_node);
      run->current_node = current_copy;
      run->interrupted = 1;
      run->completed = 0;
      turbo_state_graph_result_init(out_result, TURBO_STATE_GRAPH_INTERRUPTED, run->thread_id,
                                    run->id,
                                    latest_history ? latest_history->id : NULL,
                                    latest_history ? latest_history->checkpoint_id : NULL,
                                    latest_history ? latest_history->last_node : NULL,
                                    run->current_node,
                                    steps);
      *out_state = turbo_runtime_data_bind_value_clone(run->state);
      return *out_state ? TURBO_STATE_GRAPH_INTERRUPTED
                        : TURBO_STATE_GRAPH_OUT_OF_MEMORY;
    }
    skip_initial_interrupt = 0;

    current_index = turbo_state_graph_find_node_index(graph, current_node);
    if (current_index == (size_t)-1) {
      return TURBO_STATE_GRAPH_NODE_NOT_FOUND;
    }
    executed_node = graph->nodes[current_index].name;

    update = turbo_runtime_data_bind_value_create_object();
    if (!update) {
      return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
    }

    ctx.graph = graph;
    ctx.state = run->state;
    ctx.update = update;
    ctx.current_node = executed_node;
    ctx.next_node = NULL;
    ctx.sends = NULL;
    ctx.send_count = 0;
    ctx.send_capacity = 0;
    ctx.step = steps;
    ctx.stop = 0;

    callback_status = graph->nodes[current_index].bind_fn(&ctx,
                                                          graph->nodes[current_index].user_data);
    if (callback_status != 0) {
      turbo_state_graph_free_pending_sends(&ctx);
      turbo_runtime_data_bind_value_destroy(update);
      return TURBO_STATE_GRAPH_ERROR;
    }

    status = turbo_state_graph_apply_update(graph, run->state, update);
    if (status != TURBO_STATE_GRAPH_OK) {
      turbo_state_graph_free_pending_sends(&ctx);
      turbo_runtime_data_bind_value_destroy(update);
      return status;
    }

    for (i = 0; i < ctx.send_count; ++i) {
      status = turbo_state_graph_execute_send_node(graph, run, &ctx.sends[i], steps + 1,
                                                   &latest_history);
      if (status != TURBO_STATE_GRAPH_OK) {
        turbo_state_graph_free_pending_sends(&ctx);
        turbo_runtime_data_bind_value_destroy(update);
        return status;
      }
    }

    if (ctx.next_node) {
      resolved_next = ctx.next_node;
    } else {
      for (i = 0; i < graph->edge_count; ++i) {
        const turbo_state_graph_edge_entry_t *edge = &graph->edges[i];
        int matches = 0;

        if (edge->from_index != current_index) {
          continue;
        }
        saw_outgoing = 1;
        matches = edge->predicate ? edge->predicate(run->state, edge->user_data) : 1;
        if (matches) {
          resolved_next = graph->nodes[edge->to_index].name;
          break;
        }
      }
      if (saw_outgoing && !resolved_next) {
        turbo_state_graph_free_pending_sends(&ctx);
        turbo_runtime_data_bind_value_destroy(update);
        return TURBO_STATE_GRAPH_ROUTE_NOT_FOUND;
      }
    }

    steps += 1;
    run->steps = steps;
    turbo_state_graph_str_free(run->current_node);
    run->current_node = turbo_state_graph_strdup(resolved_next);
    run->interrupted = 0;
    run->completed = resolved_next ? 0 : 1;
    if (resolved_next && !run->current_node) {
      turbo_state_graph_free_pending_sends(&ctx);
      turbo_runtime_data_bind_value_destroy(update);
      return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
    }

    status = turbo_state_graph_add_history_record(
        graph, run, TURBO_STATE_GRAPH_SOURCE_NODE, executed_node, NULL, executed_node,
        resolved_next, steps, update, run->state, &latest_history);
    turbo_runtime_data_bind_value_destroy(update);
    turbo_state_graph_free_pending_sends(&ctx);
    if (status != TURBO_STATE_GRAPH_OK) {
      return status;
    }

    if (ctx.stop) {
      turbo_state_graph_str_free(run->current_node);
      run->current_node = NULL;
      run->completed = 1;
      run->interrupted = 0;
      turbo_state_graph_result_init(out_result, TURBO_STATE_GRAPH_STOP, run->thread_id,
                                    run->id, latest_history->id,
                                    latest_history->checkpoint_id,
                                    latest_history->last_node, NULL, steps);
      *out_state = turbo_runtime_data_bind_value_clone(run->state);
      return *out_state ? TURBO_STATE_GRAPH_STOP : TURBO_STATE_GRAPH_OUT_OF_MEMORY;
    }

    current_node = run->current_node;
  }

  turbo_state_graph_result_init(out_result, TURBO_STATE_GRAPH_OK, run->thread_id, run->id,
                                latest_history ? latest_history->id : NULL,
                                latest_history ? latest_history->checkpoint_id : NULL,
                                latest_history ? latest_history->last_node : NULL, NULL, steps);
  *out_state = turbo_runtime_data_bind_value_clone(run->state);
  return *out_state ? TURBO_STATE_GRAPH_OK : TURBO_STATE_GRAPH_OUT_OF_MEMORY;
}

static turbo_state_graph_status_t turbo_state_graph_seed_run(
    turbo_state_graph_t *graph, turbo_state_graph_run_entry_t *run,
    turbo_state_graph_source_kind_t source_kind, const char *source_name, const char *reason,
    const turbo_runtime_data_bind_value_t *update, const char *next_node, size_t step,
    turbo_state_graph_history_entry_t **out_history_entry) {
  turbo_runtime_data_bind_value_t *update_object = NULL;
  turbo_state_graph_status_t status;

  if (!graph || !run || !out_history_entry) {
    return TURBO_STATE_GRAPH_INVALID_ARGUMENT;
  }

  update_object = update ? turbo_runtime_data_bind_value_clone(update)
                         : turbo_runtime_data_bind_value_create_object();
  if (!update_object) {
    return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
  }
  if (turbo_runtime_data_bind_value_kind(update_object) !=
      TURBO_RUNTIME_DATA_BIND_VALUE_OBJECT) {
    turbo_runtime_data_bind_value_destroy(update_object);
    return TURBO_STATE_GRAPH_TYPE_MISMATCH;
  }

  status = turbo_state_graph_apply_update(graph, run->state, update_object);
  if (status != TURBO_STATE_GRAPH_OK) {
    turbo_runtime_data_bind_value_destroy(update_object);
    return status;
  }

  turbo_state_graph_str_free(run->current_node);
  run->current_node = turbo_state_graph_strdup(next_node);
  if (next_node && !run->current_node) {
    turbo_runtime_data_bind_value_destroy(update_object);
    return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
  }
  run->steps = step;
  run->completed = next_node ? 0 : 1;
  run->interrupted = 0;

  status = turbo_state_graph_add_history_record(graph, run, source_kind, source_name, reason, NULL,
                                                next_node, step, update_object, run->state,
                                                out_history_entry);
  turbo_runtime_data_bind_value_destroy(update_object);
  return status;
}

turbo_state_graph_t *turbo_state_graph_create(const char *name) {
  turbo_state_graph_t *graph =
      (turbo_state_graph_t *)calloc(1, sizeof(*graph));

  if (!graph) {
    return NULL;
  }

  graph->name = turbo_state_graph_strdup(name);
  if (name && !graph->name) {
    free(graph);
    return NULL;
  }

  return graph;
}

void turbo_state_graph_destroy(turbo_state_graph_t *graph) {
  size_t i;

  if (!graph) {
    return;
  }

  turbo_state_graph_str_free(graph->name);
  turbo_state_graph_str_free(graph->entry_node);

  for (i = 0; i < graph->channel_count; ++i) {
    turbo_state_graph_free_channel(&graph->channels[i]);
  }
  for (i = 0; i < graph->node_count; ++i) {
    turbo_state_graph_free_node(&graph->nodes[i]);
  }
  for (i = 0; i < graph->thread_count; ++i) {
    turbo_state_graph_free_thread(&graph->threads[i]);
  }
  for (i = 0; i < graph->run_count; ++i) {
    turbo_state_graph_free_run(&graph->runs[i]);
  }
  for (i = 0; i < graph->history_count; ++i) {
    turbo_state_graph_free_history_entry(&graph->history_entries[i]);
  }

  free(graph->channels);
  free(graph->nodes);
  free(graph->edges);
  free(graph->threads);
  free(graph->runs);
  free(graph->history_entries);
  free(graph);
}

turbo_state_graph_status_t turbo_state_graph_add_channel(
    turbo_state_graph_t *graph, const char *name,
    const turbo_state_graph_channel_config_t *config) {
  turbo_state_graph_channel_entry_t *channel = NULL;
  turbo_state_graph_status_t status;

  if (!graph || !name || !config) {
    return TURBO_STATE_GRAPH_INVALID_ARGUMENT;
  }
  if (turbo_state_graph_find_channel_index(graph, name) != (size_t)-1) {
    return TURBO_STATE_GRAPH_DUPLICATE_CHANNEL;
  }
  if (!turbo_state_graph_reserve((void **)&graph->channels, &graph->channel_capacity,
                                 graph->channel_count + 1, sizeof(*graph->channels))) {
    return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
  }

  channel = &graph->channels[graph->channel_count++];
  channel->name = turbo_state_graph_strdup(name);
  channel->description = turbo_state_graph_strdup(config->description);
  channel->reducer = config->reducer;
  channel->value_kind = config->value_kind;
  channel->default_value = config->default_value
                               ? turbo_runtime_data_bind_value_clone(config->default_value)
                               : turbo_runtime_data_bind_value_create_null();
  if (!channel->name || !channel->default_value ||
      (config->description && !channel->description)) {
    turbo_state_graph_free_channel(channel);
    memset(channel, 0, sizeof(*channel));
    graph->channel_count--;
    return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
  }

  status = turbo_state_graph_validate_channel_value(channel, channel->default_value);
  if (status != TURBO_STATE_GRAPH_OK) {
    turbo_state_graph_free_channel(channel);
    memset(channel, 0, sizeof(*channel));
    graph->channel_count--;
    return status;
  }

  return TURBO_STATE_GRAPH_OK;
}

turbo_state_graph_status_t
turbo_state_graph_add_bind_node(turbo_state_graph_t *graph, const char *name,
                                turbo_state_graph_bind_node_fn fn, void *user_data) {
  return turbo_state_graph_add_bind_node_owned(graph, name, fn, user_data, NULL);
}

static turbo_state_graph_status_t turbo_state_graph_add_bind_node_owned(
    turbo_state_graph_t *graph, const char *name, turbo_state_graph_bind_node_fn fn,
    void *user_data, void (*user_data_free)(void *user_data)) {
  turbo_state_graph_node_entry_t *node = NULL;

  if (!graph || !name || !fn) {
    return TURBO_STATE_GRAPH_INVALID_ARGUMENT;
  }
  if (turbo_state_graph_find_node_index(graph, name) != (size_t)-1) {
    return TURBO_STATE_GRAPH_DUPLICATE_NODE;
  }
  if (!turbo_state_graph_reserve((void **)&graph->nodes, &graph->node_capacity,
                                 graph->node_count + 1, sizeof(*graph->nodes))) {
    return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
  }

  node = &graph->nodes[graph->node_count++];
  node->name = turbo_state_graph_strdup(name);
  node->bind_fn = fn;
  node->user_data = user_data;
  node->user_data_free = user_data_free;
  if (!node->name) {
    turbo_state_graph_free_node(node);
    memset(node, 0, sizeof(*node));
    graph->node_count--;
    return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
  }

  return TURBO_STATE_GRAPH_OK;
}

static void turbo_state_graph_free_subgraph_node_data(void *user_data) {
  turbo_state_graph_subgraph_node_data_t *data =
      (turbo_state_graph_subgraph_node_data_t *)user_data;
  size_t i;

  if (!data) {
    return;
  }
  turbo_state_graph_str_free(data->child_thread_id);
  turbo_state_graph_str_free(data->child_thread_id_channel);
  turbo_state_graph_str_free(data->output_channel);
  for (i = 0; i < data->input_channel_count; ++i) {
    turbo_state_graph_str_free(data->input_channels[i]);
  }
  free(data->input_channels);
  free(data);
}

static int turbo_state_graph_subgraph_node_fn(turbo_state_graph_exec_ctx_t *ctx,
                                              void *user_data) {
  turbo_state_graph_subgraph_node_data_t *data =
      (turbo_state_graph_subgraph_node_data_t *)user_data;
  turbo_runtime_data_bind_value_t *child_input = NULL;
  turbo_runtime_data_bind_value_t *child_state = NULL;
  turbo_runtime_data_bind_value_t *output = NULL;
  turbo_state_graph_run_result_t result = {0};
  turbo_state_graph_status_t status;
  const char *child_thread_id = NULL;
  size_t i;

  if (!ctx || !data || !data->child_graph || !data->output_channel) {
    return -1;
  }

  child_thread_id = data->child_thread_id;
  if (data->child_thread_id_channel) {
    const turbo_runtime_data_bind_value_t *thread_value =
        turbo_runtime_data_bind_object_get(ctx->state, data->child_thread_id_channel);
    child_thread_id = turbo_runtime_data_bind_value_as_string(thread_value);
  }
  if (!child_thread_id || child_thread_id[0] == '\0') {
    return -1;
  }

  child_input = turbo_runtime_data_bind_value_create_object();
  if (!child_input) {
    return -1;
  }
  for (i = 0; i < data->input_channel_count; ++i) {
    const turbo_runtime_data_bind_value_t *value =
        turbo_runtime_data_bind_object_get(ctx->state, data->input_channels[i]);
    turbo_runtime_data_bind_value_t *value_copy = NULL;

    if (!value) {
      turbo_runtime_data_bind_value_destroy(child_input);
      return -1;
    }
    value_copy = turbo_runtime_data_bind_value_clone(value);
    if (!value_copy ||
        turbo_runtime_data_bind_object_set(child_input, data->input_channels[i], value_copy) !=
            TURBO_RUNTIME_DATA_BIND_OK) {
      turbo_runtime_data_bind_value_destroy(value_copy);
      turbo_runtime_data_bind_value_destroy(child_input);
      return -1;
    }
  }

  status = turbo_state_graph_start(data->child_graph, child_thread_id, child_input, NULL,
                                   &result, &child_state);
  turbo_runtime_data_bind_value_destroy(child_input);
  if (status != TURBO_STATE_GRAPH_OK || !child_state) {
    turbo_runtime_data_bind_value_destroy(child_state);
    return -1;
  }

  output = turbo_runtime_data_bind_value_create_object();
  if (!output) {
    turbo_runtime_data_bind_value_destroy(child_state);
    return -1;
  }
  if (turbo_agent_util_bind_object_set_string(output, "status",
                                              turbo_state_graph_status_text(status)) != 0 ||
      turbo_state_graph_bind_object_set_nullable_string(output, "thread_id",
                                                        result.thread_id) != 0 ||
      turbo_state_graph_bind_object_set_nullable_string(output, "run_id", result.run_id) != 0 ||
      turbo_state_graph_bind_object_set_nullable_string(output, "history_entry_id",
                                                        result.history_entry_id) != 0 ||
      turbo_state_graph_bind_object_set_nullable_string(output, "checkpoint_id",
                                                        result.checkpoint_id) != 0 ||
      turbo_state_graph_bind_object_set_nullable_string(output, "last_node",
                                                        result.last_node) != 0 ||
      turbo_state_graph_bind_object_set_nullable_string(output, "next_node",
                                                        result.next_node) != 0 ||
      turbo_agent_util_bind_object_set_int64(output, "steps", (int64_t)result.steps) != 0 ||
      turbo_agent_util_bind_object_set_clone(output, "state", child_state) != 0) {
    turbo_runtime_data_bind_value_destroy(output);
    turbo_runtime_data_bind_value_destroy(child_state);
    return -1;
  }
  turbo_runtime_data_bind_value_destroy(child_state);

  if (turbo_runtime_data_bind_object_set(ctx->update, data->output_channel, output) !=
      TURBO_RUNTIME_DATA_BIND_OK) {
    turbo_runtime_data_bind_value_destroy(output);
    return -1;
  }
  return 0;
}

turbo_state_graph_status_t turbo_state_graph_add_subgraph_node(
    turbo_state_graph_t *graph, const char *name,
    const turbo_state_graph_subgraph_node_config_t *config) {
  turbo_state_graph_subgraph_node_data_t *data = NULL;
  turbo_state_graph_status_t status;
  size_t i;

  if (!graph || !name || !config || !config->child_graph || !config->output_channel ||
      ((config->child_thread_id == NULL) == (config->child_thread_id_channel == NULL)) ||
      (config->input_channel_count > 0 && !config->input_channels)) {
    return TURBO_STATE_GRAPH_INVALID_ARGUMENT;
  }

  data = (turbo_state_graph_subgraph_node_data_t *)calloc(1, sizeof(*data));
  if (!data) {
    return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
  }
  data->child_graph = config->child_graph;
  data->child_thread_id = turbo_state_graph_strdup(config->child_thread_id);
  data->child_thread_id_channel = turbo_state_graph_strdup(config->child_thread_id_channel);
  data->output_channel = turbo_state_graph_strdup(config->output_channel);
  data->input_channel_count = config->input_channel_count;
  if ((config->child_thread_id && !data->child_thread_id) ||
      (config->child_thread_id_channel && !data->child_thread_id_channel) ||
      !data->output_channel) {
    turbo_state_graph_free_subgraph_node_data(data);
    return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
  }
  if (data->input_channel_count > 0) {
    data->input_channels = (char **)calloc(data->input_channel_count, sizeof(*data->input_channels));
    if (!data->input_channels) {
      turbo_state_graph_free_subgraph_node_data(data);
      return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
    }
    for (i = 0; i < data->input_channel_count; ++i) {
      if (!config->input_channels[i]) {
        turbo_state_graph_free_subgraph_node_data(data);
        return TURBO_STATE_GRAPH_INVALID_ARGUMENT;
      }
      data->input_channels[i] = turbo_state_graph_strdup(config->input_channels[i]);
      if (!data->input_channels[i]) {
        turbo_state_graph_free_subgraph_node_data(data);
        return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
      }
    }
  }

  status = turbo_state_graph_add_bind_node_owned(graph, name, turbo_state_graph_subgraph_node_fn,
                                                 data, turbo_state_graph_free_subgraph_node_data);
  if (status != TURBO_STATE_GRAPH_OK) {
    turbo_state_graph_free_subgraph_node_data(data);
  }
  return status;
}

turbo_state_graph_status_t
turbo_state_graph_add_bind_edge(turbo_state_graph_t *graph, const char *from, const char *to,
                                turbo_state_graph_bind_edge_predicate_fn predicate,
                                void *user_data) {
  size_t from_index;
  size_t to_index;
  turbo_state_graph_edge_entry_t *edge = NULL;

  if (!graph || !from || !to) {
    return TURBO_STATE_GRAPH_INVALID_ARGUMENT;
  }

  from_index = turbo_state_graph_find_node_index(graph, from);
  to_index = turbo_state_graph_find_node_index(graph, to);
  if (from_index == (size_t)-1 || to_index == (size_t)-1) {
    return TURBO_STATE_GRAPH_NODE_NOT_FOUND;
  }

  if (!turbo_state_graph_reserve((void **)&graph->edges, &graph->edge_capacity,
                                 graph->edge_count + 1, sizeof(*graph->edges))) {
    return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
  }

  edge = &graph->edges[graph->edge_count++];
  edge->from_index = from_index;
  edge->to_index = to_index;
  edge->predicate = predicate;
  edge->user_data = user_data;
  return TURBO_STATE_GRAPH_OK;
}

turbo_state_graph_status_t
turbo_state_graph_set_entry(turbo_state_graph_t *graph, const char *name) {
  char *entry = NULL;

  if (!graph || !name) {
    return TURBO_STATE_GRAPH_INVALID_ARGUMENT;
  }
  if (turbo_state_graph_find_node_index(graph, name) == (size_t)-1) {
    return TURBO_STATE_GRAPH_NODE_NOT_FOUND;
  }

  entry = turbo_state_graph_strdup(name);
  if (!entry) {
    return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
  }
  turbo_state_graph_str_free(graph->entry_node);
  graph->entry_node = entry;
  return TURBO_STATE_GRAPH_OK;
}

const char *turbo_state_graph_get_entry(const turbo_state_graph_t *graph) {
  return graph ? graph->entry_node : NULL;
}

size_t turbo_state_graph_channel_count(const turbo_state_graph_t *graph) {
  return graph ? graph->channel_count : 0;
}

size_t turbo_state_graph_node_count(const turbo_state_graph_t *graph) {
  return graph ? graph->node_count : 0;
}

size_t turbo_state_graph_edge_count(const turbo_state_graph_t *graph) {
  return graph ? graph->edge_count : 0;
}

turbo_state_graph_status_t
turbo_state_graph_ctx_set_next(turbo_state_graph_exec_ctx_t *ctx, const char *next_node) {
  if (!ctx || !ctx->graph || !next_node) {
    return TURBO_STATE_GRAPH_INVALID_ARGUMENT;
  }
  if (turbo_state_graph_find_node_index(ctx->graph, next_node) == (size_t)-1) {
    return TURBO_STATE_GRAPH_NODE_NOT_FOUND;
  }
  ctx->next_node = next_node;
  return TURBO_STATE_GRAPH_OK;
}

turbo_state_graph_status_t
turbo_state_graph_ctx_goto(turbo_state_graph_exec_ctx_t *ctx, const char *next_node) {
  return turbo_state_graph_ctx_set_next(ctx, next_node);
}

turbo_state_graph_status_t turbo_state_graph_ctx_send(
    turbo_state_graph_exec_ctx_t *ctx, const char *target_node,
    const turbo_runtime_data_bind_value_t *update) {
  turbo_state_graph_pending_send_t *send = NULL;

  if (!ctx || !target_node || !update ||
      turbo_runtime_data_bind_value_kind(update) != TURBO_RUNTIME_DATA_BIND_VALUE_OBJECT) {
    return TURBO_STATE_GRAPH_INVALID_ARGUMENT;
  }
  if (!turbo_state_graph_reserve((void **)&ctx->sends, &ctx->send_capacity,
                                 ctx->send_count + 1, sizeof(*ctx->sends))) {
    return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
  }
  send = &ctx->sends[ctx->send_count++];
  send->target_node = turbo_state_graph_strdup(target_node);
  send->update = turbo_runtime_data_bind_value_clone(update);
  if (!send->target_node || !send->update) {
    turbo_state_graph_str_free(send->target_node);
    turbo_runtime_data_bind_value_destroy(send->update);
    memset(send, 0, sizeof(*send));
    ctx->send_count--;
    return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
  }
  return TURBO_STATE_GRAPH_OK;
}

void turbo_state_graph_ctx_stop(turbo_state_graph_exec_ctx_t *ctx) {
  if (ctx) {
    ctx->stop = 1;
  }
}

turbo_state_graph_status_t
turbo_state_graph_start(turbo_state_graph_t *graph, const char *thread_id,
                        const turbo_runtime_data_bind_value_t *input_state,
                        const turbo_state_graph_run_options_t *options,
                        turbo_state_graph_run_result_t *out_result,
                        turbo_runtime_data_bind_value_t **out_state) {
  turbo_state_graph_thread_entry_t *thread = NULL;
  turbo_state_graph_run_entry_t *run = NULL;
  turbo_state_graph_history_entry_t *history_entry = NULL;
  turbo_runtime_data_bind_value_t *seed_state = NULL;
  turbo_state_graph_status_t status;
  const char *start_node;

  if (!graph || !out_state) {
    return TURBO_STATE_GRAPH_INVALID_ARGUMENT;
  }
  *out_state = NULL;

  start_node = options && options->start_node ? options->start_node : graph->entry_node;
  if (!start_node) {
    return TURBO_STATE_GRAPH_INVALID_ARGUMENT;
  }

  seed_state = turbo_runtime_data_bind_value_create_object();
  if (!seed_state) {
    return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
  }

  status = turbo_state_graph_seed_defaults(graph, seed_state);
  if (status != TURBO_STATE_GRAPH_OK) {
    turbo_runtime_data_bind_value_destroy(seed_state);
    return status;
  }

  status = turbo_state_graph_add_thread_record(graph, thread_id, &thread);
  if (status != TURBO_STATE_GRAPH_OK) {
    turbo_runtime_data_bind_value_destroy(seed_state);
    return status;
  }

  status = turbo_state_graph_add_run_record(graph, thread->id, NULL, NULL, seed_state, start_node,
                                            0, &run);
  if (status != TURBO_STATE_GRAPH_OK) {
    turbo_runtime_data_bind_value_destroy(seed_state);
    return status;
  }

  status = turbo_state_graph_seed_run(graph, run, TURBO_STATE_GRAPH_SOURCE_START, "start", NULL,
                                      input_state, start_node, 0, &history_entry);
  turbo_runtime_data_bind_value_destroy(seed_state);
  if (status != TURBO_STATE_GRAPH_OK) {
    return status;
  }

  turbo_state_graph_str_free(thread->current_run_id);
  thread->current_run_id = turbo_state_graph_strdup(run->id);
  if (!thread->current_run_id) {
    return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
  }

  return turbo_state_graph_execute_run(graph, run, options, out_result, out_state);
}

turbo_state_graph_status_t
turbo_state_graph_resume(turbo_state_graph_t *graph, const char *run_id,
                         const turbo_state_graph_run_options_t *options,
                         turbo_state_graph_run_result_t *out_result,
                         turbo_runtime_data_bind_value_t **out_state) {
  turbo_state_graph_run_entry_t *run = NULL;
  turbo_state_graph_thread_entry_t *thread = NULL;

  if (!graph || !run_id || !out_state) {
    return TURBO_STATE_GRAPH_INVALID_ARGUMENT;
  }
  *out_state = NULL;

  run = turbo_state_graph_find_run(graph, run_id);
  if (!run) {
    return TURBO_STATE_GRAPH_RUN_NOT_FOUND;
  }
  if (!run->current_node) {
    return TURBO_STATE_GRAPH_RUN_COMPLETED;
  }

  thread = turbo_state_graph_find_thread(graph, run->thread_id);
  if (!thread) {
    return TURBO_STATE_GRAPH_THREAD_NOT_FOUND;
  }
  turbo_state_graph_str_free(thread->current_run_id);
  thread->current_run_id = turbo_state_graph_strdup(run->id);
  if (!thread->current_run_id) {
    return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
  }

  return turbo_state_graph_execute_run(graph, run, options, out_result, out_state);
}

turbo_state_graph_status_t
turbo_state_graph_update_state(turbo_state_graph_t *graph, const char *run_id,
                               const turbo_runtime_data_bind_value_t *update,
                               const turbo_state_graph_history_options_t *options,
                               turbo_state_graph_run_result_t *out_result,
                               turbo_runtime_data_bind_value_t **out_state) {
  turbo_state_graph_run_entry_t *run = NULL;
  turbo_state_graph_history_entry_t *history_entry = NULL;
  turbo_runtime_data_bind_value_t *update_object = NULL;
  turbo_state_graph_status_t status;
  const char *source_name;

  if (!graph || !run_id || !update || !out_state) {
    return TURBO_STATE_GRAPH_INVALID_ARGUMENT;
  }
  *out_state = NULL;
  if (turbo_runtime_data_bind_value_kind(update) != TURBO_RUNTIME_DATA_BIND_VALUE_OBJECT) {
    return TURBO_STATE_GRAPH_TYPE_MISMATCH;
  }

  run = turbo_state_graph_find_run(graph, run_id);
  if (!run) {
    return TURBO_STATE_GRAPH_RUN_NOT_FOUND;
  }

  update_object = turbo_runtime_data_bind_value_clone(update);
  if (!update_object) {
    return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
  }

  status = turbo_state_graph_apply_update(graph, run->state, update_object);
  if (status != TURBO_STATE_GRAPH_OK) {
    turbo_runtime_data_bind_value_destroy(update_object);
    return status;
  }

  source_name = options && options->as_node ? options->as_node : "host_update";
  status = turbo_state_graph_add_history_record(
      graph, run, TURBO_STATE_GRAPH_SOURCE_HOST, source_name,
      options ? options->reason : NULL, source_name, run->current_node, run->steps,
      update_object, run->state, &history_entry);
  turbo_runtime_data_bind_value_destroy(update_object);
  if (status != TURBO_STATE_GRAPH_OK) {
    return status;
  }

  *out_state = turbo_runtime_data_bind_value_clone(run->state);
  if (!*out_state) {
    return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
  }

  turbo_state_graph_result_init(out_result, TURBO_STATE_GRAPH_OK, run->thread_id, run->id,
                                history_entry->id, history_entry->checkpoint_id,
                                history_entry->last_node, history_entry->next_node,
                                run->steps);
  return TURBO_STATE_GRAPH_OK;
}

static turbo_state_graph_status_t turbo_state_graph_branch_from_history(
    turbo_state_graph_t *graph, const char *history_entry_id,
    const turbo_runtime_data_bind_value_t *update,
    const turbo_state_graph_history_options_t *history_options,
    const turbo_state_graph_run_options_t *run_options, int make_current,
    turbo_state_graph_source_kind_t source_kind,
    turbo_state_graph_run_result_t *out_result,
    turbo_runtime_data_bind_value_t **out_state) {
  turbo_state_graph_history_entry_t *history_entry = NULL;
  turbo_state_graph_run_entry_t *source_run = NULL;
  turbo_state_graph_run_entry_t *run = NULL;
  turbo_state_graph_thread_entry_t *thread = NULL;
  turbo_state_graph_status_t status;
  turbo_state_graph_history_entry_t *seed_history = NULL;
  const char *source_name;

  if (!graph || !history_entry_id || !out_state) {
    return TURBO_STATE_GRAPH_INVALID_ARGUMENT;
  }
  *out_state = NULL;

  history_entry = turbo_state_graph_find_history(graph, history_entry_id);
  if (!history_entry) {
    return TURBO_STATE_GRAPH_HISTORY_NOT_FOUND;
  }
  source_run = turbo_state_graph_find_run(graph, history_entry->run_id);
  if (!source_run) {
    return TURBO_STATE_GRAPH_RUN_NOT_FOUND;
  }
  thread = turbo_state_graph_find_thread(graph, history_entry->thread_id);
  if (!thread) {
    return TURBO_STATE_GRAPH_THREAD_NOT_FOUND;
  }

  status = turbo_state_graph_add_run_record(
      graph, history_entry->thread_id, history_entry->run_id,
      source_kind == TURBO_STATE_GRAPH_SOURCE_FORK ? history_entry->id : NULL,
      history_entry->state, history_entry->next_node, history_entry->step, &run);
  if (status != TURBO_STATE_GRAPH_OK) {
    return status;
  }

  source_name = history_options && history_options->as_node
                    ? history_options->as_node
                    : (source_kind == TURBO_STATE_GRAPH_SOURCE_FORK ? "fork_from_history"
                                                                    : "resume_from_history");
  status = turbo_state_graph_seed_run(
      graph, run, source_kind, source_name, history_options ? history_options->reason : NULL,
      update, history_entry->next_node, history_entry->step, &seed_history);
  if (status != TURBO_STATE_GRAPH_OK) {
    return status;
  }

  if (make_current) {
    turbo_state_graph_str_free(thread->current_run_id);
    thread->current_run_id = turbo_state_graph_strdup(run->id);
    if (!thread->current_run_id) {
      return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
    }
  }

  if (!run->current_node) {
    *out_state = turbo_runtime_data_bind_value_clone(run->state);
    if (!*out_state) {
      return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
    }
    turbo_state_graph_result_init(out_result, TURBO_STATE_GRAPH_OK, run->thread_id, run->id,
                                  seed_history->id, seed_history->checkpoint_id, NULL, NULL,
                                  run->steps);
    return TURBO_STATE_GRAPH_OK;
  }

  return turbo_state_graph_execute_run(graph, run, run_options, out_result, out_state);
}

turbo_state_graph_status_t turbo_state_graph_resume_from_history(
    turbo_state_graph_t *graph, const char *history_entry_id,
    const turbo_runtime_data_bind_value_t *update,
    const turbo_state_graph_history_options_t *history_options,
    const turbo_state_graph_run_options_t *run_options,
    turbo_state_graph_run_result_t *out_result,
    turbo_runtime_data_bind_value_t **out_state) {
  return turbo_state_graph_branch_from_history(
      graph, history_entry_id, update, history_options, run_options, 1,
      TURBO_STATE_GRAPH_SOURCE_HOST, out_result, out_state);
}

turbo_state_graph_status_t turbo_state_graph_fork_from_history(
    turbo_state_graph_t *graph, const char *history_entry_id,
    const turbo_runtime_data_bind_value_t *update,
    const turbo_state_graph_history_options_t *history_options,
    const turbo_state_graph_run_options_t *run_options,
    turbo_state_graph_run_result_t *out_result,
    turbo_runtime_data_bind_value_t **out_state) {
  return turbo_state_graph_branch_from_history(
      graph, history_entry_id, update, history_options, run_options, 0,
      TURBO_STATE_GRAPH_SOURCE_FORK, out_result, out_state);
}

turbo_state_graph_status_t
turbo_state_graph_get_state(turbo_state_graph_t *graph, const char *run_id,
                            turbo_runtime_data_bind_value_t **out_state) {
  turbo_state_graph_run_entry_t *run = NULL;

  if (!graph || !run_id || !out_state) {
    return TURBO_STATE_GRAPH_INVALID_ARGUMENT;
  }
  *out_state = NULL;
  run = turbo_state_graph_find_run(graph, run_id);
  if (!run) {
    return TURBO_STATE_GRAPH_RUN_NOT_FOUND;
  }

  *out_state = turbo_runtime_data_bind_value_clone(run->state);
  return *out_state ? TURBO_STATE_GRAPH_OK : TURBO_STATE_GRAPH_OUT_OF_MEMORY;
}

turbo_state_graph_status_t
turbo_state_graph_get_run(turbo_state_graph_t *graph, const char *run_id,
                          turbo_runtime_data_bind_value_t **out_run) {
  turbo_state_graph_run_entry_t *run = NULL;

  if (!graph || !run_id || !out_run) {
    return TURBO_STATE_GRAPH_INVALID_ARGUMENT;
  }
  *out_run = NULL;

  run = turbo_state_graph_find_run(graph, run_id);
  if (!run) {
    return TURBO_STATE_GRAPH_RUN_NOT_FOUND;
  }

  return turbo_state_graph_run_object(run, out_run);
}

turbo_state_graph_status_t
turbo_state_graph_get_thread(turbo_state_graph_t *graph, const char *thread_id,
                             turbo_runtime_data_bind_value_t **out_thread) {
  turbo_state_graph_thread_entry_t *thread = NULL;
  turbo_state_graph_run_entry_t *run = NULL;

  if (!graph || !thread_id || !out_thread) {
    return TURBO_STATE_GRAPH_INVALID_ARGUMENT;
  }
  *out_thread = NULL;

  thread = turbo_state_graph_find_thread(graph, thread_id);
  if (!thread) {
    return TURBO_STATE_GRAPH_THREAD_NOT_FOUND;
  }
  run = thread->current_run_id ? turbo_state_graph_find_run(graph, thread->current_run_id) : NULL;
  return turbo_state_graph_thread_object(thread, run ? run->state : NULL, out_thread);
}

turbo_state_graph_status_t
turbo_state_graph_get_latest_run(turbo_state_graph_t *graph, const char *thread_id,
                                 turbo_runtime_data_bind_value_t **out_run) {
  turbo_state_graph_run_entry_t *run = NULL;

  if (!graph || !thread_id || !out_run) {
    return TURBO_STATE_GRAPH_INVALID_ARGUMENT;
  }
  *out_run = NULL;

  if (!turbo_state_graph_find_thread(graph, thread_id)) {
    return TURBO_STATE_GRAPH_THREAD_NOT_FOUND;
  }
  run = turbo_state_graph_latest_run_for_thread(graph, thread_id);
  if (!run) {
    return TURBO_STATE_GRAPH_RUN_NOT_FOUND;
  }

  return turbo_state_graph_run_object(run, out_run);
}

turbo_state_graph_status_t
turbo_state_graph_get_pending_run(turbo_state_graph_t *graph, const char *thread_id,
                                  turbo_runtime_data_bind_value_t **out_run) {
  turbo_state_graph_run_entry_t *run = NULL;

  if (!graph || !thread_id || !out_run) {
    return TURBO_STATE_GRAPH_INVALID_ARGUMENT;
  }
  *out_run = NULL;

  if (!turbo_state_graph_find_thread(graph, thread_id)) {
    return TURBO_STATE_GRAPH_THREAD_NOT_FOUND;
  }
  run = turbo_state_graph_pending_run_for_thread(graph, thread_id);
  if (!run) {
    return TURBO_STATE_GRAPH_RUN_NOT_FOUND;
  }

  return turbo_state_graph_run_object(run, out_run);
}

turbo_state_graph_status_t
turbo_state_graph_get_thread_state(turbo_state_graph_t *graph, const char *thread_id,
                                   turbo_runtime_data_bind_value_t **out_state) {
  turbo_state_graph_thread_entry_t *thread = NULL;

  if (!graph || !thread_id || !out_state) {
    return TURBO_STATE_GRAPH_INVALID_ARGUMENT;
  }
  *out_state = NULL;
  thread = turbo_state_graph_find_thread(graph, thread_id);
  if (!thread) {
    return TURBO_STATE_GRAPH_THREAD_NOT_FOUND;
  }
  if (!thread->current_run_id) {
    return TURBO_STATE_GRAPH_RUN_NOT_FOUND;
  }
  return turbo_state_graph_get_state(graph, thread->current_run_id, out_state);
}

turbo_state_graph_status_t
turbo_state_graph_get_history_entry(turbo_state_graph_t *graph,
                                    const char *history_entry_id,
                                    turbo_runtime_data_bind_value_t **out_entry) {
  turbo_state_graph_history_entry_t *history_entry = NULL;

  if (!graph || !history_entry_id || !out_entry) {
    return TURBO_STATE_GRAPH_INVALID_ARGUMENT;
  }
  *out_entry = NULL;

  history_entry = turbo_state_graph_find_history(graph, history_entry_id);
  if (!history_entry) {
    return TURBO_STATE_GRAPH_HISTORY_NOT_FOUND;
  }

  return turbo_state_graph_history_object(history_entry, out_entry);
}

turbo_state_graph_status_t
turbo_state_graph_get_history_entry_state(turbo_state_graph_t *graph,
                                          const char *history_entry_id,
                                          turbo_runtime_data_bind_value_t **out_state) {
  turbo_state_graph_history_entry_t *history_entry = NULL;

  if (!graph || !history_entry_id || !out_state) {
    return TURBO_STATE_GRAPH_INVALID_ARGUMENT;
  }
  *out_state = NULL;

  history_entry = turbo_state_graph_find_history(graph, history_entry_id);
  if (!history_entry) {
    return TURBO_STATE_GRAPH_HISTORY_NOT_FOUND;
  }

  *out_state = turbo_runtime_data_bind_value_clone(history_entry->state);
  return *out_state ? TURBO_STATE_GRAPH_OK : TURBO_STATE_GRAPH_OUT_OF_MEMORY;
}

turbo_state_graph_status_t
turbo_state_graph_get_checkpoint(turbo_state_graph_t *graph, const char *checkpoint_id,
                                 turbo_runtime_data_bind_value_t **out_checkpoint) {
  turbo_state_graph_history_entry_t *history_entry = NULL;

  if (!graph || !checkpoint_id || !out_checkpoint) {
    return TURBO_STATE_GRAPH_INVALID_ARGUMENT;
  }
  *out_checkpoint = NULL;

  history_entry = turbo_state_graph_find_checkpoint(graph, checkpoint_id);
  if (!history_entry) {
    return TURBO_STATE_GRAPH_HISTORY_NOT_FOUND;
  }

  return turbo_state_graph_history_object(history_entry, out_checkpoint);
}

turbo_state_graph_status_t
turbo_state_graph_list_state_history(turbo_state_graph_t *graph, const char *run_id,
                                     turbo_runtime_data_bind_value_t **out_entries) {
  turbo_runtime_data_bind_value_t *entries = NULL;
  size_t i;

  if (!graph || !run_id || !out_entries) {
    return TURBO_STATE_GRAPH_INVALID_ARGUMENT;
  }
  *out_entries = NULL;

  if (!turbo_state_graph_find_run(graph, run_id)) {
    return TURBO_STATE_GRAPH_RUN_NOT_FOUND;
  }

  entries = turbo_runtime_data_bind_value_create_array();
  if (!entries) {
    return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
  }

  for (i = 0; i < graph->history_count; ++i) {
    turbo_state_graph_history_entry_t *history_entry = &graph->history_entries[i];
    turbo_runtime_data_bind_value_t *object = NULL;
    turbo_state_graph_status_t status;

    if (strcmp(history_entry->run_id, run_id) != 0) {
      continue;
    }

    status = turbo_state_graph_history_object(history_entry, &object);
    if (status != TURBO_STATE_GRAPH_OK) {
      turbo_runtime_data_bind_value_destroy(entries);
      return status;
    }
    if (turbo_runtime_data_bind_array_append(entries, object) !=
        TURBO_RUNTIME_DATA_BIND_OK) {
      turbo_runtime_data_bind_value_destroy(object);
      turbo_runtime_data_bind_value_destroy(entries);
      return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
    }
  }

  *out_entries = entries;
  return TURBO_STATE_GRAPH_OK;
}

turbo_state_graph_status_t
turbo_state_graph_list_checkpoints(turbo_state_graph_t *graph, const char *run_id,
                                   turbo_runtime_data_bind_value_t **out_checkpoints) {
  return turbo_state_graph_list_state_history(graph, run_id, out_checkpoints);
}

turbo_state_graph_status_t
turbo_state_graph_list_runs(turbo_state_graph_t *graph, const char *thread_id,
                            turbo_runtime_data_bind_value_t **out_runs) {
  turbo_runtime_data_bind_value_t *runs = NULL;
  size_t i;

  if (!graph || !thread_id || !out_runs) {
    return TURBO_STATE_GRAPH_INVALID_ARGUMENT;
  }
  *out_runs = NULL;

  if (!turbo_state_graph_find_thread(graph, thread_id)) {
    return TURBO_STATE_GRAPH_THREAD_NOT_FOUND;
  }

  runs = turbo_runtime_data_bind_value_create_array();
  if (!runs) {
    return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
  }

  for (i = 0; i < graph->run_count; ++i) {
    turbo_state_graph_run_entry_t *run = &graph->runs[i];
    turbo_runtime_data_bind_value_t *run_object = NULL;
    turbo_state_graph_status_t status;

    if (strcmp(run->thread_id, thread_id) != 0) {
      continue;
    }

    status = turbo_state_graph_run_object(run, &run_object);
    if (status != TURBO_STATE_GRAPH_OK) {
      turbo_runtime_data_bind_value_destroy(runs);
      return status;
    }
    if (turbo_runtime_data_bind_array_append(runs, run_object) !=
        TURBO_RUNTIME_DATA_BIND_OK) {
      turbo_runtime_data_bind_value_destroy(run_object);
      turbo_runtime_data_bind_value_destroy(runs);
      return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
    }
  }

  *out_runs = runs;
  return TURBO_STATE_GRAPH_OK;
}

turbo_state_graph_status_t
turbo_state_graph_get_checkpoint_context(turbo_state_graph_t *graph,
                                         const char *checkpoint_id,
                                         turbo_runtime_data_bind_value_t **out_context) {
  turbo_state_graph_history_entry_t *history_entry = NULL;
  turbo_state_graph_run_entry_t *run = NULL;
  turbo_state_graph_thread_entry_t *thread = NULL;
  turbo_runtime_data_bind_value_t *context = NULL;
  turbo_runtime_data_bind_value_t *checkpoint = NULL;
  turbo_runtime_data_bind_value_t *run_object = NULL;
  turbo_runtime_data_bind_value_t *thread_object = NULL;
  turbo_runtime_data_bind_value_t *branch_tree = NULL;
  turbo_state_graph_status_t status;

  if (!graph || !checkpoint_id || !out_context) {
    return TURBO_STATE_GRAPH_INVALID_ARGUMENT;
  }
  *out_context = NULL;

  history_entry = turbo_state_graph_find_checkpoint(graph, checkpoint_id);
  if (!history_entry) {
    return TURBO_STATE_GRAPH_HISTORY_NOT_FOUND;
  }
  run = turbo_state_graph_find_run(graph, history_entry->run_id);
  if (!run) {
    return TURBO_STATE_GRAPH_RUN_NOT_FOUND;
  }
  thread = turbo_state_graph_find_thread(graph, history_entry->thread_id);
  if (!thread) {
    return TURBO_STATE_GRAPH_THREAD_NOT_FOUND;
  }

  status = turbo_state_graph_history_object(history_entry, &checkpoint);
  if (status != TURBO_STATE_GRAPH_OK) {
    return status;
  }
  status = turbo_state_graph_run_object(run, &run_object);
  if (status != TURBO_STATE_GRAPH_OK) {
    turbo_runtime_data_bind_value_destroy(checkpoint);
    return status;
  }
  status = turbo_state_graph_thread_object(thread, run->state, &thread_object);
  if (status != TURBO_STATE_GRAPH_OK) {
    turbo_runtime_data_bind_value_destroy(run_object);
    turbo_runtime_data_bind_value_destroy(checkpoint);
    return status;
  }
  status = turbo_state_graph_branch_tree_object(graph, thread, &branch_tree);
  if (status != TURBO_STATE_GRAPH_OK) {
    turbo_runtime_data_bind_value_destroy(thread_object);
    turbo_runtime_data_bind_value_destroy(run_object);
    turbo_runtime_data_bind_value_destroy(checkpoint);
    return status;
  }

  context = turbo_runtime_data_bind_value_create_object();
  if (!context) {
    turbo_runtime_data_bind_value_destroy(branch_tree);
    turbo_runtime_data_bind_value_destroy(thread_object);
    turbo_runtime_data_bind_value_destroy(run_object);
    turbo_runtime_data_bind_value_destroy(checkpoint);
    return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
  }

  if (turbo_runtime_data_bind_object_set(context, "checkpoint", checkpoint) !=
          TURBO_RUNTIME_DATA_BIND_OK ||
      turbo_runtime_data_bind_object_set(context, "run", run_object) !=
          TURBO_RUNTIME_DATA_BIND_OK ||
      turbo_runtime_data_bind_object_set(context, "thread", thread_object) !=
          TURBO_RUNTIME_DATA_BIND_OK ||
      turbo_runtime_data_bind_object_set(context, "branch_tree", branch_tree) !=
          TURBO_RUNTIME_DATA_BIND_OK) {
    turbo_runtime_data_bind_value_destroy(branch_tree);
    turbo_runtime_data_bind_value_destroy(thread_object);
    turbo_runtime_data_bind_value_destroy(run_object);
    turbo_runtime_data_bind_value_destroy(checkpoint);
    turbo_runtime_data_bind_value_destroy(context);
    return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
  }

  *out_context = context;
  return TURBO_STATE_GRAPH_OK;
}

turbo_state_graph_status_t
turbo_state_graph_get_branch_tree(turbo_state_graph_t *graph, const char *thread_id,
                                  turbo_runtime_data_bind_value_t **out_tree) {
  turbo_state_graph_thread_entry_t *thread = NULL;

  if (!graph || !thread_id || !out_tree) {
    return TURBO_STATE_GRAPH_INVALID_ARGUMENT;
  }
  *out_tree = NULL;

  thread = turbo_state_graph_find_thread(graph, thread_id);
  if (!thread) {
    return TURBO_STATE_GRAPH_THREAD_NOT_FOUND;
  }

  return turbo_state_graph_branch_tree_object(graph, thread, out_tree);
}

size_t turbo_state_graph_snapshot_schema_version(void) {
  return TURBO_STATE_GRAPH_SNAPSHOT_SCHEMA_VERSION;
}

char *turbo_state_graph_serialize_snapshot(const turbo_state_graph_t *graph, size_t *out_len) {
  turbo_runtime_data_bind_value_t *root = NULL;
  turbo_runtime_data_bind_value_t *topology = NULL;
  turbo_runtime_data_bind_value_t *runtime = NULL;
  turbo_runtime_data_bind_value_t *threads = NULL;
  turbo_runtime_data_bind_value_t *runs = NULL;
  turbo_runtime_data_bind_value_t *history = NULL;
  json_value_t *json = NULL;
  char *serialized = NULL;
  size_t i;

  if (!graph) {
    return NULL;
  }

  root = turbo_runtime_data_bind_value_create_object();
  runtime = turbo_runtime_data_bind_value_create_object();
  threads = turbo_runtime_data_bind_value_create_array();
  runs = turbo_runtime_data_bind_value_create_array();
  history = turbo_runtime_data_bind_value_create_array();
  if (!root || !runtime || !threads || !runs || !history) {
    turbo_runtime_data_bind_value_destroy(root);
    turbo_runtime_data_bind_value_destroy(runtime);
    turbo_runtime_data_bind_value_destroy(threads);
    turbo_runtime_data_bind_value_destroy(runs);
    turbo_runtime_data_bind_value_destroy(history);
    return NULL;
  }

  if (turbo_state_graph_topology_snapshot(graph, &topology) != TURBO_STATE_GRAPH_OK ||
      turbo_agent_util_bind_object_set_int64(root, "snapshot_version",
                                             (int64_t)TURBO_STATE_GRAPH_SNAPSHOT_SCHEMA_VERSION) != 0 ||
      turbo_runtime_data_bind_object_set(root, "topology", topology) != TURBO_RUNTIME_DATA_BIND_OK) {
    turbo_runtime_data_bind_value_destroy(topology);
    turbo_runtime_data_bind_value_destroy(root);
    turbo_runtime_data_bind_value_destroy(runtime);
    turbo_runtime_data_bind_value_destroy(threads);
    turbo_runtime_data_bind_value_destroy(runs);
    turbo_runtime_data_bind_value_destroy(history);
    return NULL;
  }

  for (i = 0; i < graph->thread_count; ++i) {
    turbo_runtime_data_bind_value_t *thread = NULL;
    turbo_state_graph_run_entry_t *run =
        graph->threads[i].current_run_id
            ? turbo_state_graph_find_run((turbo_state_graph_t *)graph,
                                         graph->threads[i].current_run_id)
            : NULL;
    if (turbo_state_graph_thread_object(&graph->threads[i], run ? run->state : NULL, &thread) !=
            TURBO_STATE_GRAPH_OK ||
        turbo_runtime_data_bind_array_append(threads, thread) != TURBO_RUNTIME_DATA_BIND_OK) {
      turbo_runtime_data_bind_value_destroy(thread);
      turbo_runtime_data_bind_value_destroy(root);
      turbo_runtime_data_bind_value_destroy(runtime);
      turbo_runtime_data_bind_value_destroy(threads);
      turbo_runtime_data_bind_value_destroy(runs);
      turbo_runtime_data_bind_value_destroy(history);
      return NULL;
    }
  }

  for (i = 0; i < graph->run_count; ++i) {
    turbo_runtime_data_bind_value_t *run = NULL;
    if (turbo_state_graph_run_object(&graph->runs[i], &run) != TURBO_STATE_GRAPH_OK ||
        turbo_runtime_data_bind_array_append(runs, run) != TURBO_RUNTIME_DATA_BIND_OK) {
      turbo_runtime_data_bind_value_destroy(run);
      turbo_runtime_data_bind_value_destroy(root);
      turbo_runtime_data_bind_value_destroy(runtime);
      turbo_runtime_data_bind_value_destroy(threads);
      turbo_runtime_data_bind_value_destroy(runs);
      turbo_runtime_data_bind_value_destroy(history);
      return NULL;
    }
  }

  for (i = 0; i < graph->history_count; ++i) {
    turbo_runtime_data_bind_value_t *entry = NULL;
    if (turbo_state_graph_history_object(&graph->history_entries[i], &entry) !=
            TURBO_STATE_GRAPH_OK ||
        turbo_runtime_data_bind_array_append(history, entry) != TURBO_RUNTIME_DATA_BIND_OK) {
      turbo_runtime_data_bind_value_destroy(entry);
      turbo_runtime_data_bind_value_destroy(root);
      turbo_runtime_data_bind_value_destroy(runtime);
      turbo_runtime_data_bind_value_destroy(threads);
      turbo_runtime_data_bind_value_destroy(runs);
      turbo_runtime_data_bind_value_destroy(history);
      return NULL;
    }
  }

  if (turbo_agent_util_bind_object_set_int64(runtime, "next_thread_id",
                                             (int64_t)graph->next_thread_id) != 0 ||
      turbo_agent_util_bind_object_set_int64(runtime, "next_run_id",
                                             (int64_t)graph->next_run_id) != 0 ||
      turbo_agent_util_bind_object_set_int64(runtime, "next_history_id",
                                             (int64_t)graph->next_history_id) != 0 ||
      turbo_agent_util_bind_object_set_int64(runtime, "next_checkpoint_id",
                                             (int64_t)graph->next_checkpoint_id) != 0 ||
      turbo_runtime_data_bind_object_set(runtime, "threads", threads) !=
          TURBO_RUNTIME_DATA_BIND_OK ||
      turbo_runtime_data_bind_object_set(runtime, "runs", runs) != TURBO_RUNTIME_DATA_BIND_OK ||
      turbo_runtime_data_bind_object_set(runtime, "history", history) !=
          TURBO_RUNTIME_DATA_BIND_OK ||
      turbo_runtime_data_bind_object_set(root, "runtime", runtime) !=
          TURBO_RUNTIME_DATA_BIND_OK) {
    turbo_runtime_data_bind_value_destroy(runtime);
    turbo_runtime_data_bind_value_destroy(threads);
    turbo_runtime_data_bind_value_destroy(runs);
    turbo_runtime_data_bind_value_destroy(history);
    turbo_runtime_data_bind_value_destroy(root);
    return NULL;
  }

  json = turbo_runtime_data_bind_value_to_json(root);
  turbo_runtime_data_bind_value_destroy(root);
  if (!json) {
    return NULL;
  }

  serialized = turbo_json_serialize(json, out_len);
  turbo_free_json(&json);
  return serialized;
}

static turbo_state_graph_status_t turbo_state_graph_validate_topology_snapshot(
    turbo_state_graph_t *graph, const turbo_runtime_data_bind_value_t *topology) {
  const turbo_runtime_data_bind_value_t *channels = NULL;
  const turbo_runtime_data_bind_value_t *nodes = NULL;
  const turbo_runtime_data_bind_value_t *edges = NULL;
  size_t i;

  if (!graph || !topology) {
    return TURBO_STATE_GRAPH_INVALID_ARGUMENT;
  }

  channels = turbo_runtime_data_bind_object_get(topology, "channels");
  nodes = turbo_runtime_data_bind_object_get(topology, "nodes");
  edges = turbo_runtime_data_bind_object_get(topology, "edges");
  if (!channels || !nodes || !edges ||
      turbo_runtime_data_bind_value_kind(channels) != TURBO_RUNTIME_DATA_BIND_VALUE_ARRAY ||
      turbo_runtime_data_bind_value_kind(nodes) != TURBO_RUNTIME_DATA_BIND_VALUE_ARRAY ||
      turbo_runtime_data_bind_value_kind(edges) != TURBO_RUNTIME_DATA_BIND_VALUE_ARRAY) {
    return TURBO_STATE_GRAPH_ERROR;
  }

  if (turbo_runtime_data_bind_value_size(channels) != graph->channel_count ||
      turbo_runtime_data_bind_value_size(nodes) != graph->node_count ||
      turbo_runtime_data_bind_value_size(edges) != graph->edge_count) {
    return TURBO_STATE_GRAPH_ERROR;
  }

  for (i = 0; i < graph->channel_count; ++i) {
    const turbo_runtime_data_bind_value_t *channel =
        turbo_runtime_data_bind_array_get(channels, i);
    const char *name = turbo_runtime_data_bind_value_as_string(
        turbo_runtime_data_bind_object_get(channel, "name"));
    int64_t reducer = turbo_runtime_data_bind_value_as_int64(
        turbo_runtime_data_bind_object_get(channel, "reducer"), -1);
    int64_t value_kind = turbo_runtime_data_bind_value_as_int64(
        turbo_runtime_data_bind_object_get(channel, "value_kind"), -1);

    if (!channel || !name || strcmp(name, graph->channels[i].name) != 0 ||
        reducer != (int64_t)graph->channels[i].reducer ||
        value_kind != (int64_t)graph->channels[i].value_kind) {
      return TURBO_STATE_GRAPH_ERROR;
    }
  }

  for (i = 0; i < graph->node_count; ++i) {
    const turbo_runtime_data_bind_value_t *node = turbo_runtime_data_bind_array_get(nodes, i);
    const char *name = turbo_runtime_data_bind_value_as_string(node);
    if (!name || strcmp(name, graph->nodes[i].name) != 0) {
      return TURBO_STATE_GRAPH_ERROR;
    }
  }

  for (i = 0; i < graph->edge_count; ++i) {
    const turbo_runtime_data_bind_value_t *edge = turbo_runtime_data_bind_array_get(edges, i);
    const char *from = turbo_runtime_data_bind_value_as_string(
        turbo_runtime_data_bind_object_get(edge, "from"));
    const char *to = turbo_runtime_data_bind_value_as_string(
        turbo_runtime_data_bind_object_get(edge, "to"));
    if (!from || !to ||
        strcmp(from, graph->nodes[graph->edges[i].from_index].name) != 0 ||
        strcmp(to, graph->nodes[graph->edges[i].to_index].name) != 0) {
      return TURBO_STATE_GRAPH_ERROR;
    }
  }

  return TURBO_STATE_GRAPH_OK;
}

static turbo_state_graph_status_t turbo_state_graph_import_threads(
    turbo_state_graph_t *graph, const turbo_runtime_data_bind_value_t *threads) {
  size_t i;

  if (!graph || !threads ||
      turbo_runtime_data_bind_value_kind(threads) != TURBO_RUNTIME_DATA_BIND_VALUE_ARRAY) {
    return TURBO_STATE_GRAPH_INVALID_ARGUMENT;
  }

  for (i = 0; i < turbo_runtime_data_bind_value_size(threads); ++i) {
    const turbo_runtime_data_bind_value_t *thread = turbo_runtime_data_bind_array_get(threads, i);
    const char *thread_id = turbo_runtime_data_bind_value_as_string(
        turbo_runtime_data_bind_object_get(thread, "thread_id"));
    const char *current_run_id = turbo_runtime_data_bind_value_as_string(
        turbo_runtime_data_bind_object_get(thread, "current_run_id"));
    turbo_state_graph_thread_entry_t *entry = NULL;

    if (!thread_id) {
      return TURBO_STATE_GRAPH_ERROR;
    }
    if (!turbo_state_graph_reserve((void **)&graph->threads, &graph->thread_capacity,
                                   graph->thread_count + 1, sizeof(*graph->threads))) {
      return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
    }
    entry = &graph->threads[graph->thread_count++];
    entry->id = turbo_state_graph_strdup(thread_id);
    entry->current_run_id = turbo_state_graph_strdup(current_run_id);
    if (!entry->id || (current_run_id && !entry->current_run_id)) {
      return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
    }
  }

  return TURBO_STATE_GRAPH_OK;
}

static turbo_state_graph_status_t turbo_state_graph_import_runs(
    turbo_state_graph_t *graph, const turbo_runtime_data_bind_value_t *runs) {
  size_t i;

  if (!graph || !runs ||
      turbo_runtime_data_bind_value_kind(runs) != TURBO_RUNTIME_DATA_BIND_VALUE_ARRAY) {
    return TURBO_STATE_GRAPH_INVALID_ARGUMENT;
  }

  for (i = 0; i < turbo_runtime_data_bind_value_size(runs); ++i) {
    const turbo_runtime_data_bind_value_t *run = turbo_runtime_data_bind_array_get(runs, i);
    const char *run_id = turbo_runtime_data_bind_value_as_string(
        turbo_runtime_data_bind_object_get(run, "run_id"));
    const char *thread_id = turbo_runtime_data_bind_value_as_string(
        turbo_runtime_data_bind_object_get(run, "thread_id"));
    const char *parent_run_id = turbo_runtime_data_bind_value_as_string(
        turbo_runtime_data_bind_object_get(run, "parent_run_id"));
    const char *forked_from_history_id = turbo_runtime_data_bind_value_as_string(
        turbo_runtime_data_bind_object_get(run, "forked_from_history_id"));
    const char *latest_history_entry_id = turbo_runtime_data_bind_value_as_string(
        turbo_runtime_data_bind_object_get(run, "latest_history_entry_id"));
    const char *next_node = turbo_runtime_data_bind_value_as_string(
        turbo_runtime_data_bind_object_get(run, "next_node"));
    const char *status = turbo_runtime_data_bind_value_as_string(
        turbo_runtime_data_bind_object_get(run, "status"));
    const turbo_runtime_data_bind_value_t *state =
        turbo_runtime_data_bind_object_get(run, "state");
    turbo_state_graph_run_entry_t *entry = NULL;

    if (!run_id || !thread_id || !state || !status) {
      return TURBO_STATE_GRAPH_ERROR;
    }
    if (!turbo_state_graph_reserve((void **)&graph->runs, &graph->run_capacity,
                                   graph->run_count + 1, sizeof(*graph->runs))) {
      return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
    }
    entry = &graph->runs[graph->run_count++];
    entry->id = turbo_state_graph_strdup(run_id);
    entry->thread_id = turbo_state_graph_strdup(thread_id);
    entry->parent_run_id = turbo_state_graph_strdup(parent_run_id);
    entry->forked_from_history_id = turbo_state_graph_strdup(forked_from_history_id);
    entry->latest_history_entry_id = turbo_state_graph_strdup(latest_history_entry_id);
    entry->current_node = turbo_state_graph_strdup(next_node);
    entry->steps = (size_t)turbo_runtime_data_bind_value_as_int64(
        turbo_runtime_data_bind_object_get(run, "steps"), 0);
    entry->interrupted = strcmp(status, "interrupted") == 0;
    entry->completed = strcmp(status, "completed") == 0;
    entry->state = turbo_runtime_data_bind_value_clone(state);
    if (!entry->id || !entry->thread_id || !entry->state ||
        (parent_run_id && !entry->parent_run_id) ||
        (forked_from_history_id && !entry->forked_from_history_id) ||
        (latest_history_entry_id && !entry->latest_history_entry_id) ||
        (next_node && !entry->current_node)) {
      return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
    }
  }

  return TURBO_STATE_GRAPH_OK;
}

static turbo_state_graph_status_t turbo_state_graph_import_history(
    turbo_state_graph_t *graph, const turbo_runtime_data_bind_value_t *history) {
  size_t i;

  if (!graph || !history ||
      turbo_runtime_data_bind_value_kind(history) != TURBO_RUNTIME_DATA_BIND_VALUE_ARRAY) {
    return TURBO_STATE_GRAPH_INVALID_ARGUMENT;
  }

  for (i = 0; i < turbo_runtime_data_bind_value_size(history); ++i) {
    const turbo_runtime_data_bind_value_t *entry = turbo_runtime_data_bind_array_get(history, i);
    const char *history_entry_id = turbo_runtime_data_bind_value_as_string(
        turbo_runtime_data_bind_object_get(entry, "history_entry_id"));
    const char *checkpoint_id = turbo_runtime_data_bind_value_as_string(
        turbo_runtime_data_bind_object_get(entry, "checkpoint_id"));
    const char *thread_id = turbo_runtime_data_bind_value_as_string(
        turbo_runtime_data_bind_object_get(entry, "thread_id"));
    const char *run_id = turbo_runtime_data_bind_value_as_string(
        turbo_runtime_data_bind_object_get(entry, "run_id"));
    const char *source_kind = turbo_runtime_data_bind_value_as_string(
        turbo_runtime_data_bind_object_get(entry, "source_kind"));
    const char *source_name = turbo_runtime_data_bind_value_as_string(
        turbo_runtime_data_bind_object_get(entry, "source_name"));
    const char *reason = turbo_runtime_data_bind_value_as_string(
        turbo_runtime_data_bind_object_get(entry, "reason"));
    const char *last_node = turbo_runtime_data_bind_value_as_string(
        turbo_runtime_data_bind_object_get(entry, "last_node"));
    const char *next_node = turbo_runtime_data_bind_value_as_string(
        turbo_runtime_data_bind_object_get(entry, "next_node"));
    const turbo_runtime_data_bind_value_t *update =
        turbo_runtime_data_bind_object_get(entry, "update");
    const turbo_runtime_data_bind_value_t *state =
        turbo_runtime_data_bind_object_get(entry, "state");
    turbo_state_graph_history_entry_t *history_entry = NULL;

    if (!history_entry_id || !checkpoint_id || !thread_id || !run_id || !source_kind ||
        !update || !state) {
      return TURBO_STATE_GRAPH_ERROR;
    }
    if (!turbo_state_graph_reserve((void **)&graph->history_entries, &graph->history_capacity,
                                   graph->history_count + 1,
                                   sizeof(*graph->history_entries))) {
      return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
    }
    history_entry = &graph->history_entries[graph->history_count++];
    history_entry->id = turbo_state_graph_strdup(history_entry_id);
    history_entry->checkpoint_id = turbo_state_graph_strdup(checkpoint_id);
    history_entry->thread_id = turbo_state_graph_strdup(thread_id);
    history_entry->run_id = turbo_state_graph_strdup(run_id);
    history_entry->source_name = turbo_state_graph_strdup(source_name);
    history_entry->reason = turbo_state_graph_strdup(reason);
    history_entry->last_node = turbo_state_graph_strdup(last_node);
    history_entry->next_node = turbo_state_graph_strdup(next_node);
    history_entry->source_kind =
        strcmp(source_kind, "start") == 0
            ? TURBO_STATE_GRAPH_SOURCE_START
            : strcmp(source_kind, "node") == 0
                  ? TURBO_STATE_GRAPH_SOURCE_NODE
                  : strcmp(source_kind, "fork") == 0 ? TURBO_STATE_GRAPH_SOURCE_FORK
                                                      : TURBO_STATE_GRAPH_SOURCE_HOST;
    history_entry->step = (size_t)turbo_runtime_data_bind_value_as_int64(
        turbo_runtime_data_bind_object_get(entry, "step"), 0);
    history_entry->update = turbo_runtime_data_bind_value_clone(update);
    history_entry->state = turbo_runtime_data_bind_value_clone(state);
    if (!history_entry->id || !history_entry->checkpoint_id || !history_entry->thread_id ||
        !history_entry->run_id || !history_entry->update || !history_entry->state ||
        (source_name && !history_entry->source_name) ||
        (reason && !history_entry->reason) ||
        (last_node && !history_entry->last_node) ||
        (next_node && !history_entry->next_node)) {
      return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
    }
  }

  return TURBO_STATE_GRAPH_OK;
}

turbo_state_graph_status_t turbo_state_graph_load_snapshot(
    turbo_state_graph_t *graph, const char *json, size_t len) {
  json_value_t *json_value = NULL;
  turbo_runtime_data_bind_value_t *root = NULL;
  const turbo_runtime_data_bind_value_t *topology = NULL;
  const turbo_runtime_data_bind_value_t *runtime = NULL;
  const turbo_runtime_data_bind_value_t *threads = NULL;
  const turbo_runtime_data_bind_value_t *runs = NULL;
  const turbo_runtime_data_bind_value_t *history = NULL;
  turbo_state_graph_status_t status;
  int64_t snapshot_version;

  if (!graph || !json) {
    return TURBO_STATE_GRAPH_INVALID_ARGUMENT;
  }
  if (turbo_parse_json((const uint8_t *)json, len, &json_value) != 0 || !json_value) {
    turbo_free_json(&json_value);
    return TURBO_STATE_GRAPH_ERROR;
  }
  root = turbo_runtime_data_bind_value_from_json(json_value);
  turbo_free_json(&json_value);
  if (!root) {
    return TURBO_STATE_GRAPH_OUT_OF_MEMORY;
  }

  snapshot_version = turbo_runtime_data_bind_value_as_int64(
      turbo_runtime_data_bind_object_get(root, "snapshot_version"), 0);
  topology = turbo_runtime_data_bind_object_get(root, "topology");
  runtime = turbo_runtime_data_bind_object_get(root, "runtime");
  if (snapshot_version != (int64_t)TURBO_STATE_GRAPH_SNAPSHOT_SCHEMA_VERSION || !topology ||
      !runtime) {
    turbo_runtime_data_bind_value_destroy(root);
    return TURBO_STATE_GRAPH_ERROR;
  }

  status = turbo_state_graph_validate_topology_snapshot(graph, topology);
  if (status != TURBO_STATE_GRAPH_OK) {
    turbo_runtime_data_bind_value_destroy(root);
    return status;
  }

  threads = turbo_runtime_data_bind_object_get(runtime, "threads");
  runs = turbo_runtime_data_bind_object_get(runtime, "runs");
  history = turbo_runtime_data_bind_object_get(runtime, "history");
  if (!threads || !runs || !history) {
    turbo_runtime_data_bind_value_destroy(root);
    return TURBO_STATE_GRAPH_ERROR;
  }

  turbo_state_graph_clear_runtime_state(graph);
  graph->next_thread_id = (size_t)turbo_runtime_data_bind_value_as_int64(
      turbo_runtime_data_bind_object_get(runtime, "next_thread_id"), 0);
  graph->next_run_id = (size_t)turbo_runtime_data_bind_value_as_int64(
      turbo_runtime_data_bind_object_get(runtime, "next_run_id"), 0);
  graph->next_history_id = (size_t)turbo_runtime_data_bind_value_as_int64(
      turbo_runtime_data_bind_object_get(runtime, "next_history_id"), 0);
  graph->next_checkpoint_id = (size_t)turbo_runtime_data_bind_value_as_int64(
      turbo_runtime_data_bind_object_get(runtime, "next_checkpoint_id"), 0);

  status = turbo_state_graph_import_threads(graph, threads);
  if (status == TURBO_STATE_GRAPH_OK) {
    status = turbo_state_graph_import_runs(graph, runs);
  }
  if (status == TURBO_STATE_GRAPH_OK) {
    status = turbo_state_graph_import_history(graph, history);
  }

  turbo_runtime_data_bind_value_destroy(root);
  if (status != TURBO_STATE_GRAPH_OK) {
    turbo_state_graph_clear_runtime_state(graph);
  }
  return status;
}

#define TURBO_STATE_GRAPH_STORE_COMPILE_IMPL 1
#include "turbo_state_graph_store.c"
