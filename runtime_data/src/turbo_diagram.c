#include "turbo_diagram.h"
#include <fmt.h>
#include <string.h>

static int text_valid(const char *text, size_t length, int empty) {
  if (!text || (!empty && !length) || length > TURBO_DIAGRAM_MAX_TEXT_BYTES ||
      strlen(text) != length || !vstr_utf8_valid(vstr_from_cstr(text))) return 0;
  for (size_t i = 0; i < length; ++i) {
    unsigned char c = (unsigned char)text[i];
    if ((c < 32 && c != '\n' && c != '\r' && c != '\t') || c == 127) return 0;
  }
  return 1;
}

static const char *field(const json_value_t *object, const char *name, int empty) {
  const json_value_t *value = json_object_get(object, name);
  const char *text = json_string(value);
  return json_type(value) == JSON_STRING && text_valid(text, json_string_len(value), empty)
      ? text : NULL;
}

static int view_valid(const json_value_t *view) {
  const char *kind = field(view, "kind", 0);
  return json_type(view) == JSON_OBJECT && kind &&
      (!strcmp(kind, "plan") || !strcmp(kind, "execution") || !strcmp(kind, "knowledge")) &&
      turbo_runtime_json_value_as_int64(json_object_get(view, "schema_version"), -1) == 1 &&
      json_type(json_object_get(view, "nodes")) == JSON_ARRAY &&
      json_type(json_object_get(view, "edges")) == JSON_ARRAY;
}

static size_t node_index(const json_value_t *nodes, const char *id) {
  for (size_t i = 0; i < json_array_size(nodes); ++i) {
    const char *candidate = field(json_array_get(nodes, i), "id", 0);
    if (candidate && !strcmp(candidate, id)) return i;
  }
  return SIZE_MAX;
}

static int graph_validate(const json_value_t *view) {
  const json_value_t *nodes, *edges;
  if (!view_valid(view)) return SALTS_EPROTO;
  nodes = json_object_get(view, "nodes"); edges = json_object_get(view, "edges");
  if (!json_array_size(nodes)) return SALTS_EPROTO;
  if (json_array_size(nodes) > TURBO_DIAGRAM_MAX_NODES ||
      json_array_size(edges) > TURBO_DIAGRAM_MAX_EDGES) return SALTS_ENOSPC;
  for (size_t i = 0; i < json_array_size(nodes); ++i) {
    const json_value_t *node = json_array_get(nodes, i);
    const char *id = field(node, "id", 0);
    if (!id || !field(node, "label", 0) || node_index(nodes, id) != i) return SALTS_EPROTO;
  }
  for (size_t i = 0; i < json_array_size(edges); ++i) {
    const json_value_t *edge = json_array_get(edges, i);
    const char *from = field(edge, "from", 0), *to = field(edge, "to", 0);
    if (!from || !to || !field(edge, "label", 1) || node_index(nodes, from) == SIZE_MAX ||
        node_index(nodes, to) == SIZE_MAX) return SALTS_EPROTO;
  }
  return SALTS_OK;
}

json_value_t *turbo_diagram_create(const char *kind) {
  json_value_t *view = NULL, *nodes = NULL, *edges = NULL;
  if (!kind || (strcmp(kind, "plan") && strcmp(kind, "execution") && strcmp(kind, "knowledge"))) return NULL;
  view = json_create_object(); nodes = json_create_array(); edges = json_create_array();
  if (!view || !nodes || !edges ||
      turbo_runtime_json_object_set_int64(view, "schema_version", 1) != 0 ||
      turbo_runtime_json_object_set_string(view, "kind", kind) != 0 ||
      turbo_runtime_json_object_set(view, "nodes", nodes) != 0) goto fail;
  nodes = NULL;
  if (turbo_runtime_json_object_set(view, "edges", edges) != 0) goto fail;
  return view;
fail:
  json_free(nodes); json_free(edges); json_free(view);
  return NULL;
}

int turbo_diagram_add_node(json_value_t *view, const char *id, const char *label) {
  json_value_t *nodes, *node;
  if (!view_valid(view) || !id || !label || !text_valid(id, strlen(id), 0) ||
      !text_valid(label, strlen(label), 0)) return SALTS_EINVAL;
  nodes = json_object_get(view, "nodes");
  if (json_array_size(nodes) >= TURBO_DIAGRAM_MAX_NODES) return SALTS_ENOSPC;
  if (node_index(nodes, id) != SIZE_MAX) return SALTS_EPROTO;
  node = json_create_object();
  if (!node || turbo_runtime_json_object_set_string(node, "id", id) != 0 ||
      turbo_runtime_json_object_set_string(node, "label", label) != 0 ||
      turbo_runtime_json_array_append(nodes, node) != 0) { json_free(node); return SALTS_ENOMEM; }
  return SALTS_OK;
}

int turbo_diagram_add_edge(json_value_t *view, const char *from, const char *to, const char *label) {
  json_value_t *nodes, *edges, *edge;
  if (!view_valid(view) || !from || !to || !label ||
      !text_valid(from, strlen(from), 0) || !text_valid(to, strlen(to), 0) ||
      !text_valid(label, strlen(label), 1)) return SALTS_EINVAL;
  nodes = json_object_get(view, "nodes"); edges = json_object_get(view, "edges");
  if (json_array_size(nodes) == 0) return SALTS_EPROTO;
  if (json_array_size(nodes) > TURBO_DIAGRAM_MAX_NODES ||
      json_array_size(edges) >= TURBO_DIAGRAM_MAX_EDGES) return SALTS_ENOSPC;
  if (node_index(nodes, from) == SIZE_MAX || node_index(nodes, to) == SIZE_MAX) return SALTS_EPROTO;
  edge = json_create_object();
  if (!edge || turbo_runtime_json_object_set_string(edge, "from", from) != 0 ||
      turbo_runtime_json_object_set_string(edge, "to", to) != 0 ||
      turbo_runtime_json_object_set_string(edge, "label", label) != 0 ||
      turbo_runtime_json_array_append(edges, edge) != 0) { json_free(edge); return SALTS_ENOMEM; }
  return SALTS_OK;
}

static int append(tstr *out, const char *text) {
  size_t size = strlen(text);
  tstr next;
  if (size > TURBO_DIAGRAM_MAX_OUTPUT_BYTES - tstr_len(*out)) return SALTS_ENOSPC;
  next = tstr_cat_len(*out, text, size);
  if (!next) return SALTS_ENOMEM;
  *out = next;
  return SALTS_OK;
}

static int label_append(tstr *out, const char *label) {
  for (size_t i = 0; label[i]; ++i) {
    unsigned char c = (unsigned char)label[i];
    const char *escaped = NULL;
    char one[2] = {(char)c, 0};
    int rc;
    /* Mermaid decimal entity syntax; protect syntax and HTML/Markdown tokens. */
    switch (c) {
    case '"': escaped = "#34;"; break; case '#': escaped = "#35;"; break;
    case '&': escaped = "#38;"; break; case '<': escaped = "#60;"; break;
    case '>': escaped = "#62;"; break; case '`': escaped = "#96;"; break;
    case '\\': escaped = "#92;"; break; case '[': escaped = "#91;"; break;
    case ']': escaped = "#93;"; break; case '|': escaped = "#124;"; break;
    case '{': escaped = "#123;"; break; case '}': escaped = "#125;"; break;
    case '\n': case '\r': case '\t': escaped = " "; break;
    default: break;
    }
    rc = append(out, escaped ? escaped : one);
    if (rc != SALTS_OK) return rc;
  }
  return SALTS_OK;
}

int turbo_diagram_mermaid(const json_value_t *view, tstr *out_text) {
  const json_value_t *nodes, *edges;
  tstr out = NULL, part = NULL;
  int rc = SALTS_EPROTO;
  if (out_text) *out_text = NULL;
  if (!view || !out_text) return SALTS_EINVAL;
  rc = graph_validate(view);
  if (rc != SALTS_OK) return rc;
  nodes = json_object_get(view, "nodes"); edges = json_object_get(view, "edges");
  if (!strcmp(field(view, "kind", 0), "plan"))
    out = tstr_dup(json_object_get(view, "selection")
        ? "flowchart TD\n  subgraph diagram[\"Planned dependencies (focused) - not execution\"]\n"
        : "flowchart TD\n  subgraph diagram[\"Planned dependencies - not execution\"]\n");
  else if (!strcmp(field(view, "kind", 0), "knowledge"))
    out = tstr_dup(json_object_get(view, "selection")
        ? "flowchart TD\n  subgraph diagram[\"Indexed knowledge (focused) - source freshness unverified\"]\n"
        : "flowchart TD\n  subgraph diagram[\"Indexed knowledge - source freshness unverified\"]\n");
  else if (field(view, "record_scope", 0) && !strcmp(field(view, "record_scope", 0), "published_and_current"))
    out = tstr_dup(json_object_get(view, "selection")
        ? "flowchart TD\n  subgraph diagram[\"Published execution and current journal observations (focused) - not a step trace\"]\n"
        : "flowchart TD\n  subgraph diagram[\"Published execution and current journal observations - not a step trace\"]\n");
  else
    out = tstr_dup(json_object_get(view, "selection")
        ? "flowchart TD\n  subgraph diagram[\"Published execution records (focused) - not a step trace\"]\n"
        : "flowchart TD\n  subgraph diagram[\"Published execution records - not a step trace\"]\n");
  if (!out) return SALTS_ENOMEM;
  if (json_object_get(view, "selection")) {
    rc = append(&out, "    %% Focused view; consult JSON selection for scope and omitted records.\n");
    if (rc != SALTS_OK) goto done;
    const char *mode = field(json_object_get(view, "selection"), "mode", 0);
    if (mode && !strcmp(mode, "shortest_path")) {
      rc = append(&out, "    %% One minimum-hop path; original edge directions retained; not a causality claim.\n");
      if (rc != SALTS_OK) goto done;
    }
  }
  for (size_t i = 0; i < json_array_size(nodes); ++i) {
    const json_value_t *node = json_array_get(nodes, i);
    const char *label = field(node, "label", 0);
    part = tstr_format("    n{}[\"", i);
    if (!part) { rc = SALTS_ENOMEM; goto done; }
    rc = append(&out, part); tstr_free(part); part = NULL;
    if (rc != 0 || (rc = label_append(&out, label)) != 0 || (rc = append(&out, "\"]\n")) != 0) goto done;
  }
  for (size_t i = 0; i < json_array_size(edges); ++i) {
    const json_value_t *edge = json_array_get(edges, i);
    const char *from = field(edge, "from", 0), *to = field(edge, "to", 0), *label = field(edge, "label", 1);
    size_t a = node_index(nodes, from), b = node_index(nodes, to);
    part = tstr_format("    n{} -->|\"", a);
    if (!part) { rc = SALTS_ENOMEM; goto done; }
    rc = append(&out, part); tstr_free(part); part = NULL;
    if (rc != 0 || (rc = label_append(&out, label[0] ? label : "related")) != 0) goto done;
    part = tstr_format("\"| n{}\n", b);
    if (!part) { rc = SALTS_ENOMEM; goto done; }
    rc = append(&out, part); tstr_free(part); part = NULL;
    if (rc != 0) goto done;
  }
  rc = append(&out, "  end\n");
  if (rc == SALTS_OK) { *out_text = out; out = NULL; }
done:
  tstr_free(part); tstr_free(out);
  return rc;
}

/* Bound opaque metadata before json_clone, including values the renderer ignores.
 * The source is already an owned JSON tree; this walk performs no allocation. */
static int query_budget(const json_value_t *value, size_t depth, size_t *values, size_t *bytes) {
  size_t size = 0, count = 0;
  json_type_t type = json_type(value);
  if (depth > TURBO_DIAGRAM_MAX_QUERY_DEPTH || !*values) return SALTS_ENOSPC;
  --*values;
  if (type == JSON_STRING) size = json_string_len(value);
  else if (type == JSON_NUMBER) (void)json_number_text(value, &size);
  else if (type == JSON_ARRAY) count = json_array_size(value);
  else if (type == JSON_OBJECT) count = json_object_size(value);
  if (size > *bytes || count > *values) return SALTS_ENOSPC;
  *bytes -= size;
  for (size_t i = 0; i < count; ++i) {
    const json_value_t *child;
    int rc;
    if (type == JSON_OBJECT) {
      size = json_object_key_len(value, i);
      if (size > *bytes) return SALTS_ENOSPC;
      *bytes -= size;
      child = json_object_value(value, i);
    } else child = json_array_get(value, i);
    rc = query_budget(child, depth + 1, values, bytes);
    if (rc != SALTS_OK) return rc;
  }
  return SALTS_OK;
}

static int diagram_select(const json_value_t *view, const char *focus_id, const char *target_id,
    turbo_diagram_direction_t direction, size_t max_hops, json_value_t **out_view) {
  const json_value_t *nodes, *edges;
  json_value_t *out = NULL, *selected_nodes = NULL, *selected_edges = NULL, *selection = NULL, *copy = NULL;
  size_t distances[TURBO_DIAGRAM_MAX_NODES];
  size_t predecessor[TURBO_DIAGRAM_MAX_NODES], predecessor_edge[TURBO_DIAGRAM_MAX_NODES];
  unsigned char path_nodes[TURBO_DIAGRAM_MAX_NODES] = {0}, path_edges[TURBO_DIAGRAM_MAX_EDGES] = {0};
  size_t from[TURBO_DIAGRAM_MAX_EDGES], to[TURBO_DIAGRAM_MAX_EDGES];
  size_t node_count, edge_count, focus, values = TURBO_DIAGRAM_MAX_QUERY_VALUES,
         bytes = TURBO_DIAGRAM_MAX_QUERY_BYTES;
  const char *direction_name;
  size_t target = SIZE_MAX, hop_count = 0;
  int rc;
  if (out_view) *out_view = NULL;
  if (!view || !focus_id || !out_view || !text_valid(focus_id, strlen(focus_id), 0) ||
      (target_id && !text_valid(target_id, strlen(target_id), 0)) ||
      max_hops >= TURBO_DIAGRAM_MAX_NODES ||
      (direction != TURBO_DIAGRAM_INCOMING && direction != TURBO_DIAGRAM_OUTGOING &&
       direction != TURBO_DIAGRAM_BOTH)) return SALTS_EINVAL;
  rc = query_budget(view, 0, &values, &bytes);
  if (rc != SALTS_OK) return rc;
  rc = graph_validate(view);
  if (rc != SALTS_OK) return rc;
  nodes = json_object_get(view, "nodes"); edges = json_object_get(view, "edges");
  node_count = json_array_size(nodes); edge_count = json_array_size(edges);
  focus = node_index(nodes, focus_id);
  if (focus == SIZE_MAX) return SALTS_ENOENT;
  if (target_id) {
    target = node_index(nodes, target_id);
    if (target == SIZE_MAX) return SALTS_ENOENT;
  }
  for (size_t i = 0; i < node_count; ++i) distances[i] = SIZE_MAX;
  distances[focus] = 0;
  for (size_t i = 0; i < edge_count; ++i) {
    const json_value_t *edge = json_array_get(edges, i);
    from[i] = node_index(nodes, field(edge, "from", 0));
    to[i] = node_index(nodes, field(edge, "to", 0));
  }
  /* Distance layers avoid a queue and preserve shortest hops even when source
   * edge order is reversed. O(V^2 + V*E), bounded by 128 nodes and 512 edges. */
  for (size_t depth = 0; depth < max_hops; ++depth) {
    int discovered = 0;
    for (size_t i = 0; i < edge_count; ++i) {
      size_t a = from[i], b = to[i];
      if (direction != TURBO_DIAGRAM_INCOMING && distances[a] == depth && distances[b] == SIZE_MAX) {
        distances[b] = depth + 1; discovered = 1;
        predecessor[b] = a; predecessor_edge[b] = i;
      }
      if (direction != TURBO_DIAGRAM_OUTGOING && distances[b] == depth && distances[a] == SIZE_MAX) {
        distances[a] = depth + 1; discovered = 1;
        predecessor[a] = b; predecessor_edge[a] = i;
      }
    }
    if (!discovered) break;
  }
  if (target_id) {
    if (distances[target] == SIZE_MAX) return SALTS_ENOENT;
    hop_count = distances[target];
    /* Every predecessor has distance one less; cycles cannot enter this chain.
     * Keep only that chain's edges, including one of any parallel relations. */
    for (size_t cursor = target; cursor != focus; cursor = predecessor[cursor]) {
      path_nodes[cursor] = 1; path_edges[predecessor_edge[cursor]] = 1;
    }
    path_nodes[focus] = 1;
    for (size_t i = 0; i < node_count; ++i) if (!path_nodes[i]) distances[i] = SIZE_MAX;
  }
  out = json_clone(view); selected_nodes = json_create_array(); selected_edges = json_create_array();
  selection = json_create_object();
  rc = SALTS_ENOMEM;
  if (!out || !selected_nodes || !selected_edges || !selection) goto done;
  for (size_t i = 0; i < node_count; ++i) {
    if (distances[i] == SIZE_MAX) continue;
    copy = json_clone(json_array_get(nodes, i));
    if (!copy || turbo_runtime_json_object_set_int64(copy, "selection_distance", (int64_t)distances[i]) != 0 ||
        turbo_runtime_json_array_append(selected_nodes, copy) != 0) goto done;
    copy = NULL;
  }
  for (size_t i = 0; i < edge_count; ++i) {
    if (target_id && !path_edges[i]) continue;
    if (distances[from[i]] == SIZE_MAX || distances[to[i]] == SIZE_MAX) continue;
    copy = json_clone(json_array_get(edges, i));
    if (!copy || turbo_runtime_json_array_append(selected_edges, copy) != 0) goto done;
    copy = NULL;
  }
  direction_name = direction == TURBO_DIAGRAM_INCOMING ? "incoming" :
      direction == TURBO_DIAGRAM_OUTGOING ? "outgoing" : "both";
  if (target_id &&
      (turbo_runtime_json_object_set_string(selection, "mode", "shortest_path") != 0 ||
       turbo_runtime_json_object_set_string(selection, "target_id", target_id) != 0 ||
       turbo_runtime_json_object_set_int64(selection, "hop_count", (int64_t)hop_count) != 0)) goto done;
  if (turbo_runtime_json_object_set_string(selection, "scope", "input_view") != 0 ||
      turbo_runtime_json_object_set_string(selection, "focus_id", focus_id) != 0 ||
      turbo_runtime_json_object_set_string(selection, "direction", direction_name) != 0 ||
      turbo_runtime_json_object_set_int64(selection, "max_hops", (int64_t)max_hops) != 0 ||
      turbo_runtime_json_object_set_int64(selection, "input_nodes", (int64_t)node_count) != 0 ||
      turbo_runtime_json_object_set_int64(selection, "input_edges", (int64_t)edge_count) != 0 ||
      turbo_runtime_json_object_set_int64(selection, "omitted_nodes", (int64_t)(node_count - json_array_size(selected_nodes))) != 0 ||
      turbo_runtime_json_object_set_int64(selection, "omitted_edges", (int64_t)(edge_count - json_array_size(selected_edges))) != 0 ||
      turbo_runtime_json_object_set(out, "selection", selection) != 0) goto done;
  selection = NULL;
  if (turbo_runtime_json_object_set(out, "nodes", selected_nodes) != 0) goto done;
  selected_nodes = NULL;
  if (turbo_runtime_json_object_set(out, "edges", selected_edges) != 0) goto done;
  selected_edges = NULL;
  *out_view = out; out = NULL; rc = SALTS_OK;
done:
  json_free(out); json_free(selected_nodes); json_free(selected_edges); json_free(selection); json_free(copy);
  return rc;
}

int turbo_diagram_neighborhood(const json_value_t *view, const char *focus_id,
    turbo_diagram_direction_t direction, size_t max_hops, json_value_t **out_view) {
  return diagram_select(view, focus_id, NULL, direction, max_hops, out_view);
}

int turbo_diagram_shortest_path(const json_value_t *view, const char *focus_id,
    const char *target_id, turbo_diagram_direction_t direction, size_t max_hops,
    json_value_t **out_view) {
  if (!target_id) {
    if (out_view) *out_view = NULL;
    return SALTS_EINVAL;
  }
  return diagram_select(view, focus_id, target_id, direction, max_hops, out_view);
}
