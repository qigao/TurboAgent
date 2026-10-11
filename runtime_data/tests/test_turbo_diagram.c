#include "tinytest.h"
#include "turbo_diagram.h"
#include <fmt.h>
#include <string.h>

spec("Bounded Mermaid view rendering") {
  static json_value_t *view, *record;
  static tstr text, again, id;
  before_each() {
    view = turbo_diagram_create("plan");
    check_not_null(view);
  }
  after_each() {
    json_free(view); json_free(record); view = record = NULL;
    tstr_free(text); tstr_free(again); tstr_free(id);
    text = again = id = NULL;
  }
  it("renders stable dependencies using generated identifiers and retains Unicode") {
    check_equal(turbo_diagram_add_node(view, "end", "读取文档"), SALTS_OK);
    check_equal(turbo_diagram_add_node(view, "id with spaces", "Verify"), SALTS_OK);
    check_equal(turbo_diagram_add_edge(view, "end", "id with spaces", "required by"), SALTS_OK);
    check_equal(turbo_diagram_mermaid(view, &text), SALTS_OK);
    check_contains(text, "Planned dependencies - not execution");
    check_contains(text, "n0[\"读取文档\"]");
    check_contains(text, "n0 -->|\"required by\"| n1");
    check_equal(turbo_diagram_mermaid(view, &again), SALTS_OK);
    check_equal(text, again);
    check_equal(json_get_string(json_array_get(json_object_get(view, "nodes"), 0), "id"), "end");
  }
  it("quotes syntax and treats labels as text instead of graph commands") {
    check_equal(turbo_diagram_add_node(view, "a", "\"]\nclick n0 callback <tag> #35; `x` | \\"), SALTS_OK);
    check_equal(turbo_diagram_mermaid(view, &text), SALTS_OK);
    check_contains(text, "#34;#93; click n0 callback #60;tag#62; #35;35; #96;x#96; #124; #92;");
    check_null(strstr(text, "\nclick")); check_null(strstr(text, "<tag>"));
    check_equal(turbo_diagram_add_node(view, "control", "bad\x01"), SALTS_EINVAL);
  }
  it("rejects empty views, duplicate identities and unresolved endpoints without partial output") {
    check_equal(turbo_diagram_mermaid(view, &text), SALTS_EPROTO); check_null(text);
    check_equal(turbo_diagram_add_node(view, "a", "A"), SALTS_OK);
    check_equal(turbo_diagram_add_node(view, "a", "B"), SALTS_EPROTO);
    check_equal(turbo_diagram_add_edge(view, "a", "missing", ""), SALTS_EPROTO);
    record = json_clone(json_array_get(json_object_get(view, "nodes"), 0));
    check_not_null(record);
    check_equal(turbo_runtime_json_array_append(json_object_get(view, "nodes"), record), 0); record = NULL;
    check_equal(turbo_diagram_mermaid(view, &text), SALTS_EPROTO); check_null(text);
  }
  it("rejects malformed imported views including embedded NUL and unknown versions") {
    check_equal(turbo_diagram_add_node(view, "a", "A"), SALTS_OK);
    check_equal(turbo_runtime_json_object_set_int64(view, "schema_version", 2), 0);
    check_equal(turbo_diagram_mermaid(view, &text), SALTS_EPROTO); check_null(text);
    json_free(view);
    view = json_parse("{\"schema_version\":1,\"kind\":\"plan\",\"nodes\":[{\"id\":\"a\\u0000b\",\"label\":\"A\"}],\"edges\":[]}",
                      strlen("{\"schema_version\":1,\"kind\":\"plan\",\"nodes\":[{\"id\":\"a\\u0000b\",\"label\":\"A\"}],\"edges\":[]}"));
    check_not_null(view);
    check_equal(turbo_diagram_mermaid(view, &text), SALTS_EPROTO); check_null(text);
  }
  it("bounds nodes and expanded output without publishing a truncated diagram") {
    static char large[TURBO_DIAGRAM_MAX_TEXT_BYTES + 1];
    memset(large, ']', sizeof(large) - 1); large[sizeof(large) - 1] = 0;
    for (size_t i = 0; i < TURBO_DIAGRAM_MAX_NODES; ++i) {
      id = tstr_format("id{}", i); check_not_null(id);
      check_equal(turbo_diagram_add_node(view, id, large), SALTS_OK);
      tstr_free(id); id = NULL;
    }
    check_equal(turbo_diagram_add_node(view, "overflow", "A"), SALTS_ENOSPC);
    check_equal(turbo_diagram_mermaid(view, &text), SALTS_ENOSPC); check_null(text);
  }
  it("permits relation cycles while enforcing the edge budget") {
    check_equal(turbo_diagram_add_node(view, "a", "A"), SALTS_OK);
    for (size_t i = 0; i < TURBO_DIAGRAM_MAX_EDGES; ++i)
      check_equal(turbo_diagram_add_edge(view, "a", "a", "related"), SALTS_OK);
    check_equal(turbo_diagram_add_edge(view, "a", "a", "related"), SALTS_ENOSPC);
    check_equal(turbo_diagram_mermaid(view, &text), SALTS_OK);
  }
  it("revalidates imported endpoints and rejects invalid UTF-8") {
    check_equal(turbo_diagram_add_node(view, "a", "A"), SALTS_OK);
    check_equal(turbo_diagram_add_node(view, "invalid", "\xc0\xaf"), SALTS_EINVAL);
    check_equal(turbo_diagram_add_edge(view, "a", "a", "related"), SALTS_OK);
    check_equal(turbo_runtime_json_object_set_string(
        json_array_get(json_object_get(view, "edges"), 0), "to", "missing"), 0);
    check_equal(turbo_diagram_mermaid(view, &text), SALTS_EPROTO); check_null(text);
  }
}

static const json_value_t *selected_node(const json_value_t *view, const char *id) {
  const json_value_t *nodes = json_object_get(view, "nodes");
  for (size_t i = 0; i < json_array_size(nodes); ++i) {
    const json_value_t *node = json_array_get(nodes, i);
    if (!strcmp(json_get_string(node, "id"), id)) return node;
  }
  return NULL;
}

spec("Diagram neighborhood and path navigation") {
  static json_value_t *view, *selected, *extra, *again;
  static tstr text, id;
  before_each() {
    view = turbo_diagram_create("plan"); check_not_null(view);
    check_equal(turbo_diagram_add_node(view, "a", "Source"), SALTS_OK);
    check_equal(turbo_diagram_add_node(view, "b", "Branch B"), SALTS_OK);
    check_equal(turbo_diagram_add_node(view, "c", "Branch C"), SALTS_OK);
    check_equal(turbo_diagram_add_node(view, "d", "Join"), SALTS_OK);
    check_equal(turbo_diagram_add_node(view, "z", "Disconnected"), SALTS_OK);
    /* Reverse dependency order, cycle, self edge and equal-length paths. */
    check_equal(turbo_diagram_add_edge(view, "d", "b", "cycle"), SALTS_OK);
    check_equal(turbo_diagram_add_edge(view, "b", "d", "B output"), SALTS_OK);
    check_equal(turbo_diagram_add_edge(view, "c", "d", "C output"), SALTS_OK);
    check_equal(turbo_diagram_add_edge(view, "a", "b", "first branch"), SALTS_OK);
    check_equal(turbo_diagram_add_edge(view, "a", "c", "second branch"), SALTS_OK);
    check_equal(turbo_diagram_add_edge(view, "d", "d", "self"), SALTS_OK);
  }
  after_each() {
    json_free(view); json_free(selected); json_free(extra); json_free(again);
    view = selected = extra = again = NULL;
    tstr_free(text); tstr_free(id); text = id = NULL;
  }
  it("selects one minimum-hop path deterministically instead of an induced subgraph") {
    check_equal(turbo_diagram_add_edge(view, "a", "b", "parallel alternative"), SALTS_OK);
    check_equal(turbo_diagram_shortest_path(view, "a", "d", TURBO_DIAGRAM_OUTGOING, 127, &selected), SALTS_OK);
    const json_value_t *nodes = json_object_get(selected, "nodes"), *edges = json_object_get(selected, "edges");
    const json_value_t *selection = json_object_get(selected, "selection");
    check_equal(json_array_size(nodes), 3u); check_equal(json_array_size(edges), 2u);
    check_equal(json_get_string(json_array_get(nodes, 2), "id"), "d");
    check_equal(json_get_string(json_array_get(edges, 0), "label"), "B output");
    check_equal(json_get_string(json_array_get(edges, 1), "label"), "first branch");
    check_null(selected_node(selected, "c")); check_null(selected_node(selected, "z"));
    check_equal(json_get_int(selected_node(selected, "a"), "selection_distance", -1), 0);
    check_equal(json_get_int(selected_node(selected, "b"), "selection_distance", -1), 1);
    check_equal(json_get_int(selected_node(selected, "d"), "selection_distance", -1), 2);
    check_equal(json_get_string(selection, "mode"), "shortest_path");
    check_equal(json_get_string(selection, "scope"), "input_view");
    check_equal(json_get_string(selection, "target_id"), "d");
    check_equal(json_get_int(selection, "hop_count", -1), 2);
    check_equal(json_get_int(selection, "omitted_nodes", -1), 2);
    check_equal(json_get_int(selection, "omitted_edges", -1), 5);
    check_equal(turbo_diagram_shortest_path(view, "a", "d", TURBO_DIAGRAM_OUTGOING, 127, &again), SALTS_OK);
    check_equal(json_get_string(json_array_get(json_object_get(again, "edges"), 0), "label"), "B output");
    check_equal(turbo_diagram_mermaid(selected, &text), SALTS_OK);
    check_contains(text, "One minimum-hop path"); check_null(strstr(text, "parallel alternative"));
  }
  it("chooses a direct edge over an earlier longer connection") {
    check_equal(turbo_diagram_add_edge(view, "a", "d", "shortcut"), SALTS_OK);
    check_equal(turbo_diagram_shortest_path(view, "a", "d", TURBO_DIAGRAM_OUTGOING, 127, &selected), SALTS_OK);
    check_equal(json_array_size(json_object_get(selected, "nodes")), 2u);
    check_equal(json_array_size(json_object_get(selected, "edges")), 1u);
    check_equal(json_get_string(json_array_get(json_object_get(selected, "edges"), 0), "label"), "shortcut");
    check_equal(json_get_int(json_object_get(selected, "selection"), "hop_count", -1), 1);
  }
  it("keeps original edge directions when following incoming or bidirectional paths") {
    check_equal(turbo_diagram_shortest_path(view, "d", "a", TURBO_DIAGRAM_INCOMING, 2, &selected), SALTS_OK);
    check_equal(json_get_int(selected_node(selected, "d"), "selection_distance", -1), 0);
    check_equal(json_get_int(selected_node(selected, "a"), "selection_distance", -1), 2);
    const json_value_t *edge = json_array_get(json_object_get(selected, "edges"), 0);
    check_equal(json_get_string(edge, "from"), "b"); check_equal(json_get_string(edge, "to"), "d");
    check_equal(turbo_diagram_shortest_path(view, "a", "d", TURBO_DIAGRAM_BOTH, 2, &again), SALTS_OK);
    edge = json_array_get(json_object_get(again, "edges"), 0);
    check_equal(json_get_string(edge, "from"), "d"); check_equal(json_get_string(edge, "to"), "b");
    check_equal(json_get_string(edge, "label"), "cycle");
    check_equal(json_get_int(selected_node(again, "d"), "selection_distance", -1), 2);
  }
  it("selects equal endpoints without self edges and reports no connection within scope") {
    check_equal(turbo_diagram_shortest_path(view, "d", "d", TURBO_DIAGRAM_BOTH, 0, &selected), SALTS_OK);
    check_equal(json_array_size(json_object_get(selected, "nodes")), 1u);
    check_equal(json_array_size(json_object_get(selected, "edges")), 0u);
    check_equal(json_get_int(json_object_get(selected, "selection"), "hop_count", -1), 0);
    check_equal(turbo_diagram_shortest_path(view, "a", "d", TURBO_DIAGRAM_OUTGOING, 1, &again), SALTS_ENOENT);
    check_null(again);
    check_equal(turbo_diagram_shortest_path(view, "a", "b", TURBO_DIAGRAM_OUTGOING, 0, &again), SALTS_ENOENT);
    check_equal(turbo_diagram_shortest_path(view, "a", "d", TURBO_DIAGRAM_INCOMING, 127, &again), SALTS_ENOENT);
    check_equal(turbo_diagram_shortest_path(view, "a", "z", TURBO_DIAGRAM_BOTH, 127, &again), SALTS_ENOENT);
    check_equal(turbo_diagram_shortest_path(view, "a", "absent", TURBO_DIAGRAM_BOTH, 127, &again), SALTS_ENOENT);
    check_equal(turbo_diagram_shortest_path(view, "absent", "a", TURBO_DIAGRAM_BOTH, 127, &again), SALTS_ENOENT);
    check_equal(turbo_diagram_shortest_path(selected, "d", "a", TURBO_DIAGRAM_BOTH, 127, &again), SALTS_ENOENT);
    check_null(again);
    check_equal(turbo_diagram_neighborhood(selected, "d", TURBO_DIAGRAM_BOTH, 0, &again), SALTS_OK);
    check_null(json_object_get(json_object_get(again, "selection"), "mode"));
    check_null(json_object_get(json_object_get(again, "selection"), "target_id"));
  }
  it("rejects invalid path endpoints, directions and hop bounds") {
    const char *invalid[] = {NULL, "", "\xc0\xaf", "\x01"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
      check_equal(turbo_diagram_shortest_path(view, "a", invalid[i], TURBO_DIAGRAM_BOTH, 2, &selected), SALTS_EINVAL);
      check_null(selected);
    }
    check_equal(turbo_diagram_shortest_path(view, "a", "b", TURBO_DIAGRAM_BOTH, 128, &selected), SALTS_EINVAL);
    check_equal(turbo_diagram_shortest_path(view, "a", "b", (turbo_diagram_direction_t)99, 2, &selected), SALTS_EINVAL);
    check_equal(turbo_diagram_shortest_path(view, "a", "b", TURBO_DIAGRAM_BOTH, 2, NULL), SALTS_EINVAL);
    check_null(selected);
  }
  it("finds downstream neighbors by hop count and reports the omitted scope") {
    check_equal(turbo_diagram_neighborhood(view, "a", TURBO_DIAGRAM_OUTGOING, 1, &selected), SALTS_OK);
    check_equal(json_array_size(json_object_get(selected, "nodes")), 3u);
    check_equal(json_array_size(json_object_get(selected, "edges")), 2u);
    check_equal(json_get_int(selected_node(selected, "a"), "selection_distance", -1), 0);
    check_equal(json_get_int(selected_node(selected, "b"), "selection_distance", -1), 1);
    check_equal(json_get_int(selected_node(selected, "c"), "selection_distance", -1), 1);
    check_null(selected_node(selected, "d")); check_null(selected_node(selected, "z"));
    check_equal(json_get_string(json_object_get(selected, "selection"), "scope"), "input_view");
    check_equal(json_get_int(json_object_get(selected, "selection"), "omitted_nodes", -1), 2);
    check_equal(json_get_int(json_object_get(selected, "selection"), "omitted_edges", -1), 4);
    check_equal(turbo_diagram_mermaid(selected, &text), SALTS_OK);
    check_contains(text, "Planned dependencies (focused)"); check_null(strstr(text, "Join"));
  }
  it("follows incoming edges to prerequisites without pulling in successors") {
    check_equal(turbo_diagram_neighborhood(view, "c", TURBO_DIAGRAM_INCOMING, 1, &selected), SALTS_OK);
    check_equal(json_array_size(json_object_get(selected, "nodes")), 2u);
    check_equal(json_array_size(json_object_get(selected, "edges")), 1u);
    check_not_null(selected_node(selected, "a")); check_not_null(selected_node(selected, "c"));
    check_null(selected_node(selected, "d"));
    check_equal(json_get_string(json_object_get(selected, "selection"), "direction"), "incoming");
  }
  it("includes every induced edge and keeps original directions during bidirectional traversal") {
    check_equal(turbo_diagram_neighborhood(view, "b", TURBO_DIAGRAM_BOTH, 1, &selected), SALTS_OK);
    check_equal(json_array_size(json_object_get(selected, "nodes")), 3u);
    check_equal(json_array_size(json_object_get(selected, "edges")), 4u);
    check_null(selected_node(selected, "c")); check_not_null(selected_node(selected, "d"));
    check_equal(json_get_string(json_array_get(json_object_get(selected, "edges"), 0), "from"), "d");
    check_equal(json_get_string(json_array_get(json_object_get(selected, "edges"), 0), "to"), "b");
  }
  it("terminates cycles with shortest distances independent of edge ordering") {
    check_equal(turbo_diagram_neighborhood(view, "a", TURBO_DIAGRAM_OUTGOING, 127, &selected), SALTS_OK);
    check_equal(json_array_size(json_object_get(selected, "nodes")), 4u);
    check_equal(json_array_size(json_object_get(selected, "edges")), 6u);
    check_equal(json_get_int(selected_node(selected, "b"), "selection_distance", -1), 1);
    check_equal(json_get_int(selected_node(selected, "d"), "selection_distance", -1), 2);
    check_equal(turbo_diagram_neighborhood(view, "a", TURBO_DIAGRAM_OUTGOING, 127, &again), SALTS_OK);
    check_equal(json_get_int(selected_node(again, "d"), "selection_distance", -1), 2);
  }
  it("preserves opaque provenance in independent copies after the source is destroyed") {
    json_value_t *node = json_array_get(json_object_get(view, "nodes"), 0);
    check_equal(turbo_runtime_json_object_set_string(view, "plan_hash_decimal", "18446744073709551615"), 0);
    check_equal(turbo_runtime_json_object_set_string(node, "collection", "documents"), 0);
    check_equal(turbo_runtime_json_object_set_string(node, "record_id", "doc:1"), 0);
    extra = json_create_object(); check_not_null(extra);
    check_equal(turbo_runtime_json_object_set_string(extra, "revision", "r1"), 0);
    check_equal(turbo_runtime_json_object_set(node, "source", extra), 0); extra = NULL;
    check_equal(turbo_runtime_json_object_set_string(json_array_get(json_object_get(view, "edges"), 3),
        "evidence", "observed:17"), 0);
    check_equal(turbo_diagram_neighborhood(view, "a", TURBO_DIAGRAM_OUTGOING, 1, &selected), SALTS_OK);
    check_null(json_object_get(view, "selection")); check_null(json_object_get(node, "selection_distance"));
    check_equal(turbo_diagram_shortest_path(view, "a", "b", TURBO_DIAGRAM_OUTGOING, 1, &again), SALTS_OK);
    check_equal(json_array_size(json_object_get(view, "nodes")), 5u);
    check_equal(turbo_runtime_json_object_set_string(json_object_get(node, "source"), "revision", "r2"), 0);
    json_free(view); view = NULL;
    check_equal(json_get_string(selected, "plan_hash_decimal"), "18446744073709551615");
    check_equal(json_get_string(selected_node(selected, "a"), "collection"), "documents");
    check_equal(json_get_string(selected_node(selected, "a"), "record_id"), "doc:1");
    check_equal(json_get_string(json_object_get(selected_node(selected, "a"), "source"), "revision"), "r1");
    check_equal(json_get_string(json_array_get(json_object_get(selected, "edges"), 0), "evidence"), "observed:17");
    check_equal(json_get_string(json_object_get(selected_node(again, "a"), "source"), "revision"), "r1");
    check_equal(json_get_string(json_array_get(json_object_get(again, "edges"), 0), "evidence"), "observed:17");
  }
  it("selects zero-hop self edges and cannot rediscover omitted nodes when queried again") {
    check_equal(turbo_diagram_neighborhood(view, "d", TURBO_DIAGRAM_BOTH, 0, &selected), SALTS_OK);
    check_equal(json_array_size(json_object_get(selected, "nodes")), 1u);
    check_equal(json_array_size(json_object_get(selected, "edges")), 1u);
    check_equal(turbo_diagram_neighborhood(selected, "d", TURBO_DIAGRAM_BOTH, 127, &again), SALTS_OK);
    check_equal(json_get_int(json_object_get(again, "selection"), "input_nodes", -1), 1);
    check_equal(json_get_string(json_object_get(again, "selection"), "scope"), "input_view");
    check_null(selected_node(again, "a"));
  }
  it("rejects unknown IDs, invalid query parameters and damage outside the requested neighborhood") {
    check_equal(turbo_diagram_neighborhood(view, "missing", TURBO_DIAGRAM_BOTH, 1, &selected), SALTS_ENOENT);
    check_null(selected);
    check_equal(turbo_diagram_neighborhood(view, "a", TURBO_DIAGRAM_BOTH, 128, &selected), SALTS_EINVAL);
    check_equal(turbo_diagram_neighborhood(view, "a", (turbo_diagram_direction_t)99, 1, &selected), SALTS_EINVAL);
    check_equal(turbo_diagram_neighborhood(view, "", TURBO_DIAGRAM_BOTH, 1, &selected), SALTS_EINVAL);
    check_equal(turbo_diagram_neighborhood(view, "\xc0\xaf", TURBO_DIAGRAM_BOTH, 1, &selected), SALTS_EINVAL);
    check_equal(turbo_runtime_json_object_set_string(json_array_get(json_object_get(view, "edges"), 0), "to", "missing"), 0);
    check_equal(turbo_diagram_neighborhood(view, "z", TURBO_DIAGRAM_BOTH, 0, &selected), SALTS_EPROTO);
    check_null(selected);
    check_equal(turbo_diagram_shortest_path(view, "z", "z", TURBO_DIAGRAM_BOTH, 0, &selected), SALTS_EPROTO);
    check_null(selected);
  }
  it("bounds copied metadata bytes before allocating an output view") {
    static char large[TURBO_DIAGRAM_MAX_QUERY_BYTES + 1];
    memset(large, 'x', sizeof(large) - 1); large[sizeof(large) - 1] = 0;
    check_equal(turbo_runtime_json_object_set_string(view, "payload", large), 0);
    check_equal(turbo_diagram_neighborhood(view, "z", TURBO_DIAGRAM_BOTH, 0, &selected), SALTS_ENOSPC);
    check_null(selected); check_null(json_object_get(view, "selection"));
    check_equal(turbo_diagram_shortest_path(view, "z", "z", TURBO_DIAGRAM_BOTH, 0, &selected), SALTS_ENOSPC);
    check_null(selected);
  }
  it("bounds metadata value count even for empty scalar payloads") {
    extra = json_create_array(); check_not_null(extra);
    for (size_t i = 0; i < TURBO_DIAGRAM_MAX_QUERY_VALUES; ++i)
      check_true(json_array_add_checked(extra, json_create_null()));
    check_equal(turbo_runtime_json_object_set(view, "payload", extra), 0); extra = NULL;
    check_equal(turbo_diagram_neighborhood(view, "z", TURBO_DIAGRAM_BOTH, 0, &selected), SALTS_ENOSPC);
    check_null(selected);
    check_equal(turbo_diagram_shortest_path(view, "z", "z", TURBO_DIAGRAM_BOTH, 0, &selected), SALTS_ENOSPC);
    check_null(selected);
  }
  it("admits nesting at the limit and refuses one more level") {
    json_value_t *cursor = view;
    for (size_t depth = 1; depth <= TURBO_DIAGRAM_MAX_QUERY_DEPTH; ++depth) {
      extra = json_create_object(); check_not_null(extra);
      check_equal(turbo_runtime_json_object_set(cursor, "nested", extra), 0);
      cursor = extra; extra = NULL;
    }
    check_equal(turbo_diagram_neighborhood(view, "a", TURBO_DIAGRAM_BOTH, 0, &selected), SALTS_OK);
    check_equal(turbo_runtime_json_object_set_int64(cursor, "too_deep", 1), 0);
    check_equal(turbo_diagram_neighborhood(view, "a", TURBO_DIAGRAM_BOTH, 0, &again), SALTS_ENOSPC);
    check_null(again);
    check_equal(turbo_diagram_shortest_path(view, "a", "b", TURBO_DIAGRAM_BOTH, 1, &again), SALTS_ENOSPC);
    check_null(again);
  }
  it("reaches the last node at the maximum hop count without overflowing workspace") {
    json_free(view); view = turbo_diagram_create("execution"); check_not_null(view);
    for (size_t i = 0; i < TURBO_DIAGRAM_MAX_NODES; ++i) {
      id = tstr_format("n{}", i); check_not_null(id);
      check_equal(turbo_diagram_add_node(view, id, id), SALTS_OK);
      tstr_free(id); id = NULL;
    }
    for (size_t i = TURBO_DIAGRAM_MAX_NODES - 1; i > 0; --i) {
      const json_value_t *nodes = json_object_get(view, "nodes");
      check_equal(turbo_diagram_add_edge(view, json_get_string(json_array_get(nodes, i - 1), "id"),
          json_get_string(json_array_get(nodes, i), "id"), "next"), SALTS_OK);
    }
    check_equal(turbo_diagram_neighborhood(view, "n0", TURBO_DIAGRAM_OUTGOING, 127, &selected), SALTS_OK);
    check_equal(json_array_size(json_object_get(selected, "nodes")), 128u);
    check_equal(json_get_int(selected_node(selected, "n127"), "selection_distance", -1), 127);
    check_equal(turbo_diagram_shortest_path(view, "n0", "n127", TURBO_DIAGRAM_OUTGOING, 127, &again), SALTS_OK);
    check_equal(json_array_size(json_object_get(again, "nodes")), 128u);
    check_equal(json_array_size(json_object_get(again, "edges")), 127u);
    check_equal(json_get_int(selected_node(again, "n127"), "selection_distance", -1), 127);
  }
}
