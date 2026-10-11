#include "tinytest.h"
#include "../src/turbo_agent_knowledge_store_internal.h"
#include <fmt.h>
#include <stdlib.h>
#include <string.h>

static int seed(turbo_agent_knowledge_store_t *store, const char *id, const char *kind, const char *hash) {
  turbo_agent_knowledge_document_t document = {0};
  document.id = id; document.uri = id; document.kind = kind;
  document.title = "Indexed \"source\" 文档"; document.content_hash = hash;
  document.metadata_json = "{\"private\":\"hidden-metadata\"}";
  return turbo_agent_knowledge_store_upsert_text(store, &document, "secret source text", 128);
}

static int seed_body(turbo_agent_knowledge_store_t *store, const char *id,
    const char *uri, const char *body, size_t chunk_bytes) {
  turbo_agent_knowledge_document_t document = {0};
  document.id = id; document.uri = uri; document.kind = "text";
  return turbo_agent_knowledge_store_upsert_text(store, &document, body, chunk_bytes);
}

static int deny_payload(void *context, int action, const char *first, const char *second,
    const char *database, const char *trigger) {
  (void)context; (void)first; (void)database; (void)trigger;
  if (action == SQLITE_READ && second && (!strcmp(second, "text") || !strcmp(second, "metadata_json"))) return SQLITE_DENY;
  if (action == SQLITE_INSERT || action == SQLITE_UPDATE || action == SQLITE_DELETE) return SQLITE_DENY;
  return SQLITE_OK;
}

static int deny_commit(void *context, int action, const char *first, const char *second,
    const char *database, const char *trigger) {
  (void)context; (void)second; (void)database; (void)trigger;
  return action == SQLITE_TRANSACTION && first && !strcmp(first, "COMMIT") ? SQLITE_DENY : SQLITE_OK;
}

typedef struct snapshot_writer_s { sqlite3 *db; int called, status; } snapshot_writer_t;
static int publish_mention_revision(unsigned event, void *context, void *statement, void *detail) {
  snapshot_writer_t *writer = context;
  const char *sql = sqlite3_sql(statement);
  (void)detail;
  if (event == SQLITE_TRACE_STMT && !writer->called && sql && strstr(sql, "typeof(text)")) {
    writer->called = 1;
    writer->status = sqlite3_exec(writer->db,
        "BEGIN IMMEDIATE; UPDATE documents SET content_hash='v2' WHERE id='doc';"
        "UPDATE chunks SET text='gone' WHERE document_id='doc'; COMMIT;", NULL, NULL, NULL);
  }
  return 0;
}
static int publish_revision(unsigned event, void *context, void *statement, void *detail) {
  snapshot_writer_t *writer = context;
  const char *sql = sqlite3_sql(statement);
  (void)detail;
  if (event == SQLITE_TRACE_STMT && !writer->called && sql && strstr(sql, "SELECT id,ordinal")) {
    writer->called = 1;
    writer->status = sqlite3_exec(writer->db,
        "BEGIN IMMEDIATE; UPDATE documents SET content_hash='v2' WHERE id='doc';"
        "UPDATE chunks SET ordinal=5 WHERE document_id='doc'; COMMIT;", NULL, NULL, NULL);
  }
  return 0;
}

spec("Knowledge diagram snapshots") {
  static turbo_agent_knowledge_store_t *store;
  static turbo_tool_registry_t *tools;
  static turbo_action_tool_registry_t *actions;
  static json_value_t *view, *selected, *args, *result;
  static tstr text, path, id;
  static char *directory, *serialized, *value_serialized;
  static snapshot_writer_t writer;
  before_each() {
    store = turbo_agent_knowledge_store_sqlite_open(":memory:"); check_not_null(store);
    check_equal(seed(store, "doc", "markdown", "v1"), 0);
    check_equal(seed(store, "code", "code", "c1"), 0);
  }
  after_each() {
    turbo_tool_registry_destroy(tools); tools = NULL;
    turbo_action_tool_registry_destroy(actions); actions = NULL;
    turbo_agent_knowledge_store_close(store); store = NULL;
    if (writer.db) sqlite3_close(writer.db);
    writer = (snapshot_writer_t){0};
    json_free(view); json_free(selected); json_free(args); json_free(result);
    view = selected = args = result = NULL;
    tstr_free(text); tstr_free(path); tstr_free(id); text = path = id = NULL;
    json_serialize_free(serialized); serialized = NULL;
    json_serialize_free(value_serialized); value_serialized = NULL;
    if (directory) tt_remove_tree(directory);
    free(directory); directory = NULL;
  }
  it("projects selected documents, code files and revision-relative chunk identities without payload reads") {
    const char *ids[] = {"code", "doc"};
    const json_value_t *nodes, *edges;
    int changes = sqlite3_total_changes(store->db);
    check_equal(sqlite3_set_authorizer(store->db, deny_payload, NULL), SQLITE_OK);
    check_equal(turbo_agent_knowledge_store_diagram(store, ids, 2, &view), SALTS_OK);
    check_equal(sqlite3_total_changes(store->db), changes);
    check_equal(sqlite3_get_autocommit(store->db), 1);
    nodes = json_object_get(view, "nodes"); edges = json_object_get(view, "edges");
    check_equal(json_get_string(view, "kind"), "knowledge");
    check_equal(json_get_string(view, "freshness"), "indexed_snapshot");
    check_equal(json_array_size(nodes), 4u); check_equal(json_array_size(edges), 2u);
    check_equal(json_get_string(json_array_get(nodes, 0), "id"), "document:code");
    check_equal(json_get_string(json_array_get(nodes, 0), "artifact_kind"), "code");
    check_equal(json_get_string(json_array_get(nodes, 1), "id"), "chunk:code#0");
    check_equal(json_get_string(json_array_get(nodes, 1), "document_id"), "code");
    check_equal(json_get_string(json_array_get(nodes, 1), "content_hash"), "c1");
    check_equal(json_get_string(json_array_get(nodes, 2), "record_id"), "doc");
    check_equal(json_get_string(json_array_get(edges, 0), "relation"), "contains");
    check_equal(json_get_string(json_array_get(edges, 0), "evidence"), "chunks.document_id");
    serialized = json_serialize(view, NULL); check_not_null(serialized);
    check_null(strstr(serialized, "secret source text")); check_null(strstr(serialized, "hidden-metadata"));
    check_equal(turbo_diagram_neighborhood(view, "chunk:code#0", TURBO_DIAGRAM_INCOMING, 1, &selected), SALTS_OK);
    check_equal(json_array_size(json_object_get(selected, "nodes")), 2u);
    check_equal(turbo_diagram_mermaid(selected, &text), SALTS_OK);
    check_contains(text, "Indexed knowledge (focused) - source freshness unverified");
    check_contains(text, "#34;source#34; 文档");
  }
  it("retains an old owned snapshot after reindexing and closing the source") {
    const char *ids[] = {"doc"};
    check_equal(turbo_agent_knowledge_store_diagram(store, ids, 1, &view), SALTS_OK);
    check_equal(seed(store, "doc", "markdown", "v2"), 0);
    check_equal(turbo_agent_knowledge_store_diagram(store, ids, 1, &selected), SALTS_OK);
    turbo_agent_knowledge_store_close(store); store = NULL;
    check_equal(json_get_string(json_array_get(json_object_get(view, "nodes"), 1), "content_hash"), "v1");
    check_equal(json_get_string(json_array_get(json_object_get(selected, "nodes"), 1), "content_hash"), "v2");
    check_equal(json_get_string(json_array_get(json_object_get(view, "nodes"), 1), "id"), "chunk:doc#0");
    check_equal(json_get_string(json_array_get(json_object_get(selected, "nodes"), 1), "id"), "chunk:doc#0");
  }
  it("records every bounded literal URI occurrence with revision and byte evidence") {
    const char *ids[] = {"doc", "code"};
    const json_value_t *edges, *edge;
    check_equal(seed_body(store, "doc", "readme.md", "文档 [worker](src/worker.c) `src/worker.c`", 2048), 0);
    check_equal(seed_body(store, "code", "src/worker.c", "", 2048), 0);
    check_equal(turbo_agent_knowledge_store_diagram_ex(store, ids, 2, TURBO_KNOWLEDGE_DIAGRAM_URI_MENTIONS, &view), SALTS_OK);
    edges = json_object_get(view, "edges"); check_equal(json_array_size(edges), 4u);
    edge = json_array_get(edges, 2);
    check_equal(json_get_string(edge, "from"), "document:doc");
    check_equal(json_get_string(edge, "to"), "document:code");
    check_equal(json_get_string(edge, "relation"), "mentions_uri");
    check_equal(json_get_string(edge, "evidence"), "chunks.text");
    check_equal(json_get_int(edge, "source_byte_offset", -1), 16);
    check_equal(json_get_int(edge, "source_byte_length", -1), 12);
    check_equal(json_get_string(edge, "source_chunk_id"), "doc#0");
    check_equal(json_get_int(edge, "source_chunk_byte_offset", -1), 16);
    check_equal(json_get_int(edge, "end_chunk_byte_offset_exclusive", -1), 28);
    check_equal(json_get_string(edge, "source_content_hash"),
        json_get_string(json_array_get(json_object_get(view, "nodes"), 0), "content_hash"));
    check_equal(json_get_string(edge, "target_content_hash"),
        json_get_string(json_array_get(json_object_get(view, "nodes"), 2), "content_hash"));
    check_equal(json_get_int(json_array_get(edges, 3), "source_byte_offset", -1), 31);
    check_equal(turbo_diagram_neighborhood(view, "document:code", TURBO_DIAGRAM_INCOMING, 1, &selected), SALTS_OK);
    check_equal(json_array_size(json_object_get(selected, "nodes")), 2u);
    check_equal(turbo_diagram_mermaid(selected, &text), SALTS_OK);
    check_contains(text, "mentions URI");
    check_equal(seed_body(store, "doc", "readme.md", "removed", 2048), 0);
    json_free(selected); selected = NULL;
    check_equal(turbo_agent_knowledge_store_diagram_ex(store, ids, 2, TURBO_KNOWLEDGE_DIAGRAM_URI_MENTIONS, &selected), SALTS_OK);
    check_equal(json_array_size(json_object_get(selected, "edges")), 2u);
    check_equal(json_array_size(json_object_get(view, "edges")), 4u);
  }
  it("does not normalize paths or match URI prefixes and still recognizes literal code mentions") {
    const char *ids[] = {"doc", "code"};
    check_equal(seed_body(store, "code", "src/worker.c", "", 2048), 0);
    check_equal(seed_body(store, "doc", "readme.md",
        "src/worker.cpp ./src/worker.c SRC/worker.c src/worker.c#part src/worker.c?q "
        "src%2Fworker.c src/worker.c/more src/worker.c, xsrc/worker.c "
        "const char *p=\"src/worker.c\"; [src/worker.c]", 2048), 0);
    check_equal(turbo_agent_knowledge_store_diagram_ex(store, ids, 2, TURBO_KNOWLEDGE_DIAGRAM_URI_MENTIONS, &view), SALTS_OK);
    check_equal(json_array_size(json_object_get(view, "edges")), 4u);
    check_equal(turbo_agent_knowledge_store_diagram_ex(store, ids, 1, TURBO_KNOWLEDGE_DIAGRAM_URI_MENTIONS, &selected), SALTS_OK);
    check_equal(json_array_size(json_object_get(selected, "edges")), 1u);
  }
  it("reassembles split UTF-8 and reports URI evidence across byte chunks") {
    const char *ids[] = {"doc", "code"};
    const json_value_t *edge;
    check_equal(seed_body(store, "doc", "readme.md", "文 code", 2048), 0);
    check_equal(sqlite3_exec(store->db,
        "UPDATE chunks SET text=CAST(X'E6' AS TEXT) WHERE document_id='doc';"
        "INSERT INTO chunks(id,document_id,ordinal,text) VALUES('middle','doc',1,CAST(X'968720636F' AS TEXT));"
        "INSERT INTO chunks(id,document_id,ordinal,text) VALUES('last','doc',2,'de');", NULL, NULL, NULL), SQLITE_OK);
    check_equal(turbo_agent_knowledge_store_diagram_ex(store, ids, 2, TURBO_KNOWLEDGE_DIAGRAM_URI_MENTIONS, &view), SALTS_OK);
    edge = json_array_get(json_object_get(view, "edges"), 4);
    check_equal(json_get_string(edge, "source_chunk_id"), "middle");
    check_equal(json_get_int(edge, "source_byte_offset", -1), 4);
    check_equal(json_get_int(edge, "source_chunk_byte_offset", -1), 3);
    check_equal(json_get_string(edge, "end_chunk_id"), "last");
    check_equal(json_get_int(edge, "end_chunk_byte_offset_exclusive", -1), 2);
  }
  it("keeps the opt-in flag strict and default or false requests payload-free") {
    const char *ids[] = {"doc"};
    const char *request = "{\"document_ids\":[\"doc\"],\"include_uri_mentions\":false}";
    check_equal(turbo_agent_knowledge_store_diagram_ex(store, ids, 1, 2u, &view), SALTS_EINVAL);
    check_null(view);
    check_equal(sqlite3_set_authorizer(store->db, deny_payload, NULL), SQLITE_OK);
    args = json_parse(request, strlen(request)); check_not_null(args);
    check_equal(turbo_knowledge_diagram_tool_value(args, &result, store), 0);
    check_equal(turbo_agent_knowledge_store_diagram_ex(store, ids, 1, TURBO_KNOWLEDGE_DIAGRAM_URI_MENTIONS, &view), SALTS_EIO);
    check_null(view); check_equal(sqlite3_get_autocommit(store->db), 1);
  }
  it("preserves self and cyclic mentions and accepts an empty document without chunks") {
    const char *ids[] = {"doc", "code"};
    check_equal(seed_body(store, "doc", "doc", "doc code", 128), 0);
    check_equal(seed_body(store, "code", "code", "doc", 128), 0);
    check_equal(turbo_agent_knowledge_store_diagram_ex(store, ids, 2, TURBO_KNOWLEDGE_DIAGRAM_URI_MENTIONS, &view), SALTS_OK);
    check_equal(json_array_size(json_object_get(view, "edges")), 5u);
    check_equal(turbo_diagram_neighborhood(view, "document:doc", TURBO_DIAGRAM_INCOMING, 1, &selected), SALTS_OK);
    check_equal(json_array_size(json_object_get(selected, "edges")), 3u);
    json_free(selected); selected = NULL;
    check_equal(seed_body(store, "doc", "doc", "", 128), 0);
    check_equal(sqlite3_exec(store->db, "DELETE FROM chunks WHERE document_id='doc'", NULL, NULL, NULL), SQLITE_OK);
    check_equal(turbo_agent_knowledge_store_diagram_ex(store, ids, 1, TURBO_KNOWLEDGE_DIAGRAM_URI_MENTIONS, &selected), SALTS_OK);
    check_equal(json_array_size(json_object_get(selected, "nodes")), 1u);
    check_equal(json_array_size(json_object_get(selected, "edges")), 0u);
  }
  const char *body_damage[] = {
      "UPDATE documents SET size=size+1 WHERE id='doc'",
      "UPDATE chunks SET ordinal=1 WHERE document_id='doc'",
      "DELETE FROM chunks WHERE document_id='doc'",
      "UPDATE chunks SET text=CAST(X'610062' AS TEXT) WHERE document_id='doc'",
      "UPDATE documents SET size=2 WHERE id='doc'; UPDATE chunks SET text=CAST(X'C0AF' AS TEXT) WHERE document_id='doc'",
      "UPDATE chunks SET text=X'6162' WHERE document_id='doc'"};
  for (size_t i = 0; i < sizeof(body_damage) / sizeof(body_damage[0]); ++i) {
    it("rejects inconsistent indexed text case %zu only when extraction is requested", i) {
      const char *ids[] = {"doc"};
      check_equal(sqlite3_exec(store->db, body_damage[i], NULL, NULL, NULL), SQLITE_OK);
      check_equal(turbo_agent_knowledge_store_diagram(store, ids, 1, &view), SALTS_OK);
      check_equal(turbo_agent_knowledge_store_diagram_ex(store, ids, 1, TURBO_KNOWLEDGE_DIAGRAM_URI_MENTIONS, &selected), SALTS_EPROTO);
      check_null(selected); check_equal(sqlite3_get_autocommit(store->db), 1);
    }
  }
  it("admits the text budgets exactly and fails oversized documents or selections atomically") {
    const char *ids[] = {"a", "b", "c", "d", "code"};
    text = tstr_new_len(NULL, TURBO_KNOWLEDGE_DIAGRAM_MAX_DOCUMENT_BYTES + 1); check_not_null(text);
    memset(text, 'x', tstr_len(text));
    check_equal(seed_body(store, "a", "a", text, tstr_len(text)), 0);
    check_equal(turbo_agent_knowledge_store_diagram_ex(store, ids, 1, TURBO_KNOWLEDGE_DIAGRAM_URI_MENTIONS, &view), SALTS_ENOSPC);
    check_null(view); check_equal(sqlite3_get_autocommit(store->db), 1);
    text[TURBO_KNOWLEDGE_DIAGRAM_MAX_DOCUMENT_BYTES] = 0;
    for (size_t i = 0; i < 4; ++i) check_equal(seed_body(store, ids[i], ids[i], text, tstr_len(text)), 0);
    check_equal(turbo_agent_knowledge_store_diagram_ex(store, ids, 4, TURBO_KNOWLEDGE_DIAGRAM_URI_MENTIONS, &view), SALTS_OK);
    check_equal(turbo_agent_knowledge_store_diagram_ex(store, ids, 5, TURBO_KNOWLEDGE_DIAGRAM_URI_MENTIONS, &selected), SALTS_ENOSPC);
    check_null(selected); check_equal(sqlite3_get_autocommit(store->db), 1);
  }
  it("counts parallel mentions against the shared edge budget without truncation") {
    const char *ids[] = {"doc", "code"};
    text = tstr_new(); check_not_null(text);
    for (size_t i = 0; i < TURBO_DIAGRAM_MAX_EDGES - 2; ++i) {
      tstr next = tstr_cat_len(text, "code ", 5); check_not_null(next); text = next;
    }
    check_equal(seed_body(store, "doc", "doc", text, tstr_len(text) + 5), 0);
    check_equal(turbo_agent_knowledge_store_diagram_ex(store, ids, 2, TURBO_KNOWLEDGE_DIAGRAM_URI_MENTIONS, &view), SALTS_OK);
    check_equal(json_array_size(json_object_get(view, "edges")), TURBO_DIAGRAM_MAX_EDGES);
    {
      tstr next = tstr_cat_len(text, "code ", 5); check_not_null(next); text = next;
    }
    check_equal(seed_body(store, "doc", "doc", text, tstr_len(text)), 0);
    check_equal(turbo_agent_knowledge_store_diagram_ex(store, ids, 2, TURBO_KNOWLEDGE_DIAGRAM_URI_MENTIONS, &selected), SALTS_ENOSPC);
    check_null(selected); check_equal(sqlite3_get_autocommit(store->db), 1);
  }
  it("keeps URI evidence and hashes on one snapshot during a concurrent text replacement") {
    const char *ids[] = {"doc", "code"};
    int changes;
    turbo_agent_knowledge_store_close(store); store = NULL;
    directory = tt_make_temp_dir("knowledge-uri-snapshot"); check_not_null(directory);
    path = tstr_format("{}/index.db", directory); check_not_null(path);
    store = turbo_agent_knowledge_store_sqlite_open(path); check_not_null(store);
    check_equal(sqlite3_exec(store->db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL), SQLITE_OK);
    check_equal(seed_body(store, "doc", "doc", "code", 128), 0);
    check_equal(seed(store, "code", "code", "c1"), 0);
    changes = sqlite3_total_changes(store->db);
    check_equal(sqlite3_open(path, &writer.db), SQLITE_OK);
    check_equal(sqlite3_trace_v2(store->db, SQLITE_TRACE_STMT, publish_mention_revision, &writer), SQLITE_OK);
    check_equal(turbo_agent_knowledge_store_diagram_ex(store, ids, 2, TURBO_KNOWLEDGE_DIAGRAM_URI_MENTIONS, &view), SALTS_OK);
    check_equal(writer.called, 1); check_equal(writer.status, SQLITE_OK);
    check_equal(sqlite3_total_changes(store->db), changes);
    check_equal(json_array_size(json_object_get(view, "edges")), 3u);
    check_not_equal(json_get_string(json_array_get(json_object_get(view, "edges"), 2), "source_content_hash"), "v2");
    check_equal(sqlite3_trace_v2(store->db, 0, NULL, NULL), SQLITE_OK);
    check_equal(turbo_agent_knowledge_store_diagram_ex(store, ids, 2, TURBO_KNOWLEDGE_DIAGRAM_URI_MENTIONS, &selected), SALTS_OK);
    check_equal(json_array_size(json_object_get(selected, "edges")), 2u);
    check_equal(json_get_string(json_array_get(json_object_get(selected, "nodes"), 0), "content_hash"), "v2");
  }
  it("rolls back a missing document selection and leaves caller-owned transactions alone") {
    const char *ids[] = {"doc", "missing"}, *duplicates[] = {"doc", "doc"};
    check_equal(turbo_agent_knowledge_store_diagram(store, duplicates, 2, &view), SALTS_EINVAL);
    check_equal(turbo_agent_knowledge_store_diagram(store, ids, 0, &view), SALTS_EINVAL);
    check_equal(turbo_agent_knowledge_store_diagram(store, ids, 33, &view), SALTS_EINVAL);
    check_equal(turbo_agent_knowledge_store_diagram(store, ids, 2, &view), SALTS_ENOENT);
    check_null(view); check_equal(sqlite3_get_autocommit(store->db), 1);
    check_equal(sqlite3_exec(store->db, "BEGIN", NULL, NULL, NULL), SQLITE_OK);
    check_equal(turbo_agent_knowledge_store_diagram(store, ids, 1, &view), SALTS_EBUSY);
    check_equal(sqlite3_get_autocommit(store->db), 0);
    check_equal(sqlite3_exec(store->db, "ROLLBACK", NULL, NULL, NULL), SQLITE_OK);
    check_equal(turbo_agent_knowledge_store_diagram(store, ids, 1, &view), SALTS_OK);
  }
  it("retains documents without chunks and orders chunk locators by ordinal") {
    const char *ids[] = {"doc", "code"};
    check_equal(sqlite3_exec(store->db, "DELETE FROM chunks WHERE document_id='doc';"
        "UPDATE chunks SET ordinal=9 WHERE document_id='code';"
        "INSERT INTO chunks(id,document_id,ordinal,text) VALUES('earlier','code',3,'secret');", NULL, NULL, NULL), SQLITE_OK);
    check_equal(turbo_agent_knowledge_store_diagram(store, ids, 2, &view), SALTS_OK);
    check_equal(json_array_size(json_object_get(view, "nodes")), 4u);
    check_equal(json_get_string(json_array_get(json_object_get(view, "nodes"), 2), "record_id"), "earlier");
    check_equal(json_get_int(json_array_get(json_object_get(view, "nodes"), 3), "ordinal", -1), 9);
  }
  it("discards a complete view if ending its snapshot fails and then releases the transaction") {
    const char *ids[] = {"doc"};
    check_equal(sqlite3_set_authorizer(store->db, deny_commit, NULL), SQLITE_OK);
    check_equal(turbo_agent_knowledge_store_diagram(store, ids, 1, &view), SALTS_EIO);
    check_null(view); check_equal(sqlite3_get_autocommit(store->db), 1);
    check_equal(sqlite3_set_authorizer(store->db, NULL, NULL), SQLITE_OK);
    check_equal(turbo_agent_knowledge_store_diagram(store, ids, 1, &view), SALTS_OK);
  }
  it("accepts the full document selection budget through the tool surface") {
    json_value_t *ids;
    args = json_create_object(); check_not_null(args);
    result = json_create_array(); check_not_null(result);
    check_equal(turbo_runtime_json_object_set(args, "document_ids", result), 0);
    ids = result; result = NULL;
    for (size_t i = 0; i < TURBO_KNOWLEDGE_DIAGRAM_MAX_DOCUMENTS; ++i) {
      id = tstr_format("item{}", i); check_not_null(id);
      check_equal(seed(store, id, "note", "v1"), 0);
      result = json_create_string(id); check_not_null(result);
      check_equal(turbo_runtime_json_array_append(ids, result), 0); result = NULL;
      tstr_free(id); id = NULL;
    }
    actions = turbo_action_tool_registry_create(); check_not_null(actions);
    check_equal(turbo_agent_knowledge_store_add_action_tools(actions, store), 0);
    check_equal(turbo_action_tool_registry_execute(actions, "agent.knowledge.diagram", args, &result), TURBO_ACTION_TOOL_OK);
    check_equal(json_array_size(json_object_get(json_object_get(result, "diagram"), "nodes")), 64u);
  }
  it("admits exactly 128 nodes and rejects one additional chunk without a partial view") {
    const char *ids[] = {"doc"};
    check_equal(sqlite3_exec(store->db,
        "WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<126) "
        "INSERT INTO chunks(id,document_id,ordinal,text) SELECT 'extra-'||x,'doc',x,'' FROM n;", NULL, NULL, NULL), SQLITE_OK);
    check_equal(turbo_agent_knowledge_store_diagram(store, ids, 1, &view), SALTS_OK);
    check_equal(json_array_size(json_object_get(view, "nodes")), 128u);
    check_equal(sqlite3_exec(store->db,
        "INSERT INTO chunks(id,document_id,ordinal,text) VALUES('overflow','doc',127,'');", NULL, NULL, NULL), SQLITE_OK);
    check_equal(turbo_agent_knowledge_store_diagram(store, ids, 1, &selected), SALTS_ENOSPC);
    check_null(selected); check_equal(sqlite3_get_autocommit(store->db), 1);
  }
  const char *damage[] = {
      "UPDATE documents SET title=CAST(X'610062' AS TEXT) WHERE id='doc'",
      "UPDATE documents SET title=CAST(X'C0AF' AS TEXT) WHERE id='doc'",
      "UPDATE documents SET size=-1 WHERE id='doc'",
      "UPDATE chunks SET ordinal=-1 WHERE document_id='doc'",
      "INSERT INTO chunks(id,document_id,ordinal,text) VALUES('duplicate-ordinal','doc',0,'')",
      "DROP TABLE chunks"};
  for (size_t i = 0; i < sizeof(damage) / sizeof(damage[0]); ++i) {
    it("rejects malformed stored records or schema case %zu and releases its transaction", i) {
      const char *ids[] = {"doc"};
      check_equal(sqlite3_exec(store->db, damage[i], NULL, NULL, NULL), SQLITE_OK);
      check_equal(turbo_agent_knowledge_store_diagram(store, ids, 1, &view), SALTS_EPROTO);
      check_null(view); check_equal(sqlite3_get_autocommit(store->db), 1);
    }
  }
  it("rejects oversized stored metadata without truncating identity") {
    const char *ids[] = {"doc"};
    check_equal(sqlite3_exec(store->db,
        "UPDATE documents SET title=replace(hex(zeroblob(1025)),'00','x') WHERE id='doc';", NULL, NULL, NULL), SQLITE_OK);
    check_equal(turbo_agent_knowledge_store_diagram(store, ids, 1, &view), SALTS_ENOSPC);
    check_null(view); check_equal(sqlite3_get_autocommit(store->db), 1);
  }
  it("reads a coherent snapshot while another connection commits a replacement revision") {
    const char *ids[] = {"doc"};
    turbo_agent_knowledge_store_close(store); store = NULL;
    directory = tt_make_temp_dir("knowledge-diagram"); check_not_null(directory);
    path = tstr_format("{}/index.db", directory); check_not_null(path);
    store = turbo_agent_knowledge_store_sqlite_open(path); check_not_null(store);
    check_equal(sqlite3_exec(store->db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL), SQLITE_OK);
    check_equal(seed(store, "doc", "markdown", "v1"), 0);
    check_equal(sqlite3_open(path, &writer.db), SQLITE_OK);
    check_equal(sqlite3_trace_v2(store->db, SQLITE_TRACE_STMT, publish_revision, &writer), SQLITE_OK);
    check_equal(turbo_agent_knowledge_store_diagram(store, ids, 1, &view), SALTS_OK);
    check_equal(writer.called, 1); check_equal(writer.status, SQLITE_OK);
    check_equal(sqlite3_trace_v2(store->db, 0, NULL, NULL), SQLITE_OK);
    check_equal(json_get_string(json_array_get(json_object_get(view, "nodes"), 0), "content_hash"), "v1");
    check_equal(json_get_int(json_array_get(json_object_get(view, "nodes"), 1), "ordinal", -1), 0);
    check_equal(turbo_agent_knowledge_store_diagram(store, ids, 1, &selected), SALTS_OK);
    check_equal(json_get_string(json_array_get(json_object_get(selected, "nodes"), 0), "content_hash"), "v2");
    check_equal(json_get_int(json_array_get(json_object_get(selected, "nodes"), 1), "ordinal", -1), 5);
  }
  it("exposes the same graph through string, JSON and observe action tool calls") {
    const char *request = "{\"document_ids\":[\"doc\",\"code\"],\"focus_id\":\"document:doc\",\"direction\":\"outgoing\",\"max_hops\":1}";
    tools = turbo_tool_registry_create(); actions = turbo_action_tool_registry_create();
    check_not_null(tools); check_not_null(actions);
    check_equal(turbo_agent_knowledge_store_add_tools(tools, store), 0);
    check_equal(turbo_agent_knowledge_store_add_action_tools(actions, store), 0);
    check_equal(turbo_action_tool_registry_find(actions, "agent.knowledge.diagram")->kind, TURBO_ACTION_OBSERVE);
    check_equal(turbo_tool_registry_execute(tools, "agent.knowledge.diagram", request, &serialized), TURBO_TOOL_OK);
    view = json_parse(serialized, strlen(serialized)); check_not_null(view);
    check_equal(json_array_size(json_object_get(json_object_get(view, "diagram"), "nodes")), 2u);
    args = json_parse(request, strlen(request)); check_not_null(args);
    check_equal(turbo_action_tool_registry_execute(actions, "agent.knowledge.diagram", args, &result), TURBO_ACTION_TOOL_OK);
    check_equal(json_get_string(json_object_get(result, "diagram"), "freshness"), "indexed_snapshot");
    check_equal(turbo_runtime_json_object_set_string(args, "format", "mermaid"), 0);
    check_equal(turbo_tool_registry_execute_json_value(tools, "agent.knowledge.diagram", args, &selected), TURBO_TOOL_OK);
    check_contains(json_get_string(selected, "mermaid"), "Indexed knowledge (focused)");
  }
  it("publishes matching knowledge contracts with preserved Action mutation policy") {
    tools = turbo_tool_registry_create(); actions = turbo_action_tool_registry_create();
    check_not_null(tools); check_not_null(actions);
    check_equal(turbo_agent_knowledge_store_add_tools(tools, store), 0);
    check_equal(turbo_agent_knowledge_store_add_action_tools(actions, store), 0);
    check_equal(turbo_tool_registry_count(tools), 11u);
    check_equal(turbo_action_tool_registry_count(actions), 11);
    for (size_t i = 0; i < turbo_tool_registry_count(tools); ++i) {
      turbo_tool_definition_t normal;
      const turbo_action_tool_definition_t *action;
      int mutates;
      check_equal(turbo_tool_registry_get_definition(tools, i, &normal), TURBO_TOOL_OK);
      action = turbo_action_tool_registry_find(actions, normal.name); check_not_null(action);
      check_equal(normal.description, action->description);
      check_equal(normal.parameters_json, action->parameters_json);
      check_equal(normal.strict, 1); check_equal(action->idempotent, 1);
      check_not_null(normal.handler); check_not_null(normal.json_value_handler); check_not_null(action->handler);
      check_true(normal.user_data == store); check_true(action->user_data == store);
      mutates = !strcmp(normal.name, "agent.knowledge.upsert_text") ||
          !strcmp(normal.name, "agent.knowledge.index_file") ||
          !strcmp(normal.name, "agent.knowledge.index_directory") ||
          !strcmp(normal.name, "agent.knowledge.delete_document");
      check_equal(action->kind, mutates ? TURBO_ACTION_MUTATE : TURBO_ACTION_OBSERVE);
    }
    check_equal(turbo_agent_knowledge_store_add_tools(tools, store), -1);
    check_equal(turbo_agent_knowledge_store_add_action_tools(actions, store), -1);
    check_equal(turbo_tool_registry_count(tools), 11u);
    check_equal(turbo_action_tool_registry_count(actions), 11);
  }
  const struct {
    const char *name, *request;
    int status;
  } adapter_cases[] = {
      {"agent.knowledge.search", "{\"query\":\"secret\"}", 0},
      {"agent.knowledge.build_context", "{\"query\":\"secret\"}", 0},
      {"agent.knowledge.stats", "{}", 0},
      {"agent.knowledge.tool_graph", "{}", 0},
      {"agent.knowledge.list_documents", "{}", 0},
      {"agent.knowledge.get_document", "{\"document_id\":\"doc\"}", 0},
      {"agent.knowledge.diagram", "{\"document_ids\":[\"doc\",\"code\"],\"include_uri_mentions\":true}", 0},
      {"agent.knowledge.upsert_text", "{\"id\":\"added\",\"text\":\"新的内容\"}", 0},
      {"agent.knowledge.index_file", "{\"path\":\"\"}", -1},
      {"agent.knowledge.index_directory", "{\"root_dir\":\"\"}", -1},
      {"agent.knowledge.delete_document", "{\"document_id\":\"doc\"}", 0}};
  for (size_t i = 0; i < sizeof(adapter_cases) / sizeof(adapter_cases[0]); ++i) {
    it("keeps the direct string ABI aligned with JSON execution for %s", adapter_cases[i].name) {
      turbo_tool_definition_t definition = {0};
      int found = 0;
      tools = turbo_tool_registry_create(); check_not_null(tools);
      check_equal(turbo_agent_knowledge_store_add_tools(tools, store), 0);
      for (size_t j = 0; j < turbo_tool_registry_count(tools); ++j) {
        check_equal(turbo_tool_registry_get_definition(tools, j, &definition), TURBO_TOOL_OK);
        if (!strcmp(definition.name, adapter_cases[i].name)) { found = 1; break; }
      }
      check_equal(found, 1);
      /* Registry execution may prefer the JSON callback; call the string ABI
       * itself so a broken generated adapter cannot hide behind that path. */
      check_equal(definition.handler("{", &serialized, definition.user_data), -1);
      check_null(serialized);
      args = json_parse(adapter_cases[i].request, strlen(adapter_cases[i].request)); check_not_null(args);
      check_equal(definition.handler(adapter_cases[i].request, &serialized, definition.user_data), adapter_cases[i].status);
      check_equal(definition.json_value_handler(args, &result, definition.user_data), adapter_cases[i].status);
      if (adapter_cases[i].status == 0) {
        value_serialized = json_serialize(result, NULL); check_not_null(value_serialized);
        check_equal(serialized, value_serialized);
        /* Returned JSON text is owned independently of the parsed arguments. */
        json_free(args); args = NULL;
        view = json_parse(serialized, strlen(serialized)); check_not_null(view);
        check_true(json_get_bool(view, "ok", 0));
      } else {
        check_null(serialized); check_null(result);
      }
    }
  }
  it("offers reverse URI lookup through all registered adapters and renders mention edges") {
    const char *request = "{\"document_ids\":[\"doc\",\"code\"],\"include_uri_mentions\":true,"
        "\"focus_id\":\"document:code\",\"direction\":\"incoming\",\"max_hops\":1}";
    check_equal(seed_body(store, "doc", "doc", "code", 128), 0);
    tools = turbo_tool_registry_create(); actions = turbo_action_tool_registry_create();
    check_not_null(tools); check_not_null(actions);
    check_equal(turbo_agent_knowledge_store_add_tools(tools, store), 0);
    check_equal(turbo_agent_knowledge_store_add_action_tools(actions, store), 0);
    check_equal(turbo_tool_registry_execute(tools, "agent.knowledge.diagram", request, &serialized), TURBO_TOOL_OK);
    view = json_parse(serialized, strlen(serialized)); check_not_null(view);
    check_equal(json_array_size(json_object_get(json_object_get(view, "diagram"), "nodes")), 2u);
    args = json_parse(request, strlen(request)); check_not_null(args);
    check_equal(turbo_action_tool_registry_execute(actions, "agent.knowledge.diagram", args, &result), TURBO_ACTION_TOOL_OK);
    check_equal(json_get_string(json_array_get(json_object_get(json_object_get(result, "diagram"), "edges"), 0), "relation"), "mentions_uri");
    check_equal(turbo_runtime_json_object_set_string(args, "format", "mermaid"), 0);
    check_equal(turbo_tool_registry_execute_json_value(tools, "agent.knowledge.diagram", args, &selected), TURBO_TOOL_OK);
    check_contains(json_get_string(selected, "mermaid"), "mentions URI");
  }
  const char *invalid[] = {
      "{\"document_ids\":[\"doc\"],\"include_uri_mentions\":null}",
      "{\"document_ids\":[\"doc\"],\"include_uri_mentions\":1}",
      "{\"document_ids\":[\"doc\"],\"include_uri_mentions\":\"true\"}",
      "{\"document_ids\":[]}", "{\"document_ids\":[\"doc\",\"doc\"]}",
      "{\"document_ids\":[\"doc\\u0000x\"]}", "{\"document_ids\":[\"missing\"]}",
      "{\"document_ids\":[\"doc\"],\"direction\":\"both\"}",
      "{\"document_ids\":[\"doc\"],\"format\":\"svg\"}",
      "{\"document_ids\":[\"doc\"],\"focus_id\":\"document:doc\",\"max_hops\":1.5}",
      "{\"document_ids\":[\"doc\"],\"focus_id\":\"document:doc\",\"max_hops\":128}",
      "{\"document_ids\":[\"doc\"],\"unexpected\":true}"};
  for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
    it("rejects invalid tool request %zu without returning an incomplete result", i) {
      actions = turbo_action_tool_registry_create(); check_not_null(actions);
      check_equal(turbo_agent_knowledge_store_add_action_tools(actions, store), 0);
      args = json_parse(invalid[i], strlen(invalid[i])); check_not_null(args);
      check_not_equal(turbo_action_tool_registry_execute(actions, "agent.knowledge.diagram", args, &result), TURBO_ACTION_TOOL_OK);
      check_null(result); check_equal(sqlite3_get_autocommit(store->db), 1);
    }
  }
}
