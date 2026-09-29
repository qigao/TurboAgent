#include "tinytest.h"
#include "turbo_agent_test_support.h"
#include "turbo_agent_memory_store.h"
#include <json_parser.h>

#include <platform.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

static char *query_only_memory_store_strdup(const char *text) {
  size_t length;
  char *copy;

  if (!text) {
    return NULL;
  }
  length = strlen(text);
  copy = (char *)malloc(length + 1);
  check_not_null(copy);
  memcpy(copy, text, length + 1);
  return copy;
}

static int query_only_memory_store_query(void *user_data, const char *namespace_prefix,
                                         const char *kind, const char *key_prefix,
                                         const char *text_substring,
                                         char **out_records_json) {
  (void)user_data;
  if (!out_records_json) {
    return -1;
  }
  if (namespace_prefix && strcmp(namespace_prefix, "project") != 0 &&
      strcmp(namespace_prefix, "project/") != 0) {
    return -1;
  }
  if (kind && strcmp(kind, "context") != 0) {
    *out_records_json = query_only_memory_store_strdup("[]");
    return *out_records_json ? 0 : -1;
  }
  if (key_prefix && strcmp(key_prefix, "con") != 0) {
    *out_records_json = query_only_memory_store_strdup("[]");
    return *out_records_json ? 0 : -1;
  }
  if (text_substring && strcmp(text_substring, "remember") != 0) {
    *out_records_json = query_only_memory_store_strdup("[]");
    return *out_records_json ? 0 : -1;
  }
  *out_records_json =
      query_only_memory_store_strdup("[{\"id\":\"project/demo::context\","
                                     "\"namespace\":\"project/demo\","
                                     "\"kind\":\"context\",\"key\":\"context\","
                                     "\"text\":\"remember this\","
                                     "\"metadata\":{\"scope\":\"project\","
                                     "\"path\":\"/tmp/notes.md\"},"
                                     "\"created_at\":null}]");
  return *out_records_json ? 0 : -1;
}

static turbo_agent_memory_store_t query_only_memory_store_create(void) {
  turbo_agent_memory_store_t store = {0};
  store.query = query_only_memory_store_query;
  return store;
}

static int sortable_query_only_memory_store_query(void *user_data, const char *namespace_prefix,
                                                  const char *kind, const char *key_prefix,
                                                  const char *text_substring,
                                                  char **out_records_json) {
  (void)user_data;
  (void)namespace_prefix;
  (void)kind;
  (void)key_prefix;
  (void)text_substring;
  if (!out_records_json) {
    return -1;
  }
  *out_records_json = query_only_memory_store_strdup(
      "[{\"id\":\"project/demo::gamma\",\"namespace\":\"project/demo\","
      "\"kind\":\"context\",\"key\":\"gamma\",\"text\":\"gamma\","
      "\"metadata\":{\"scope\":\"project\",\"path\":\"/tmp/gamma.md\"},\"created_at\":null},"
      "{\"id\":\"project/demo::alpha\",\"namespace\":\"project/demo\","
      "\"kind\":\"context\",\"key\":\"alpha\",\"text\":\"alpha\","
      "\"metadata\":{\"scope\":\"project\",\"path\":\"/tmp/alpha.md\"},"
      "\"created_at\":\"2026-01-10T09:00:00Z\"},"
      "{\"id\":\"project/demo::beta\",\"namespace\":\"project/demo\","
      "\"kind\":\"context\",\"key\":\"beta\",\"text\":\"beta\","
      "\"metadata\":{\"scope\":\"team\",\"path\":\"/var/beta.md\"},"
      "\"created_at\":\"2026-02-15T18:30:00Z\"}]");
  return *out_records_json ? 0 : -1;
}

static turbo_agent_memory_store_t sortable_query_only_memory_store_create(void) {
  turbo_agent_memory_store_t store = {0};
  store.query = sortable_query_only_memory_store_query;
  return store;
}

static int malformed_query_only_memory_store_query(void *user_data, const char *namespace_prefix,
                                                   const char *kind, const char *key_prefix,
                                                   const char *text_substring,
                                                   char **out_records_json) {
  (void)user_data;
  (void)namespace_prefix;
  (void)kind;
  (void)key_prefix;
  (void)text_substring;
  if (!out_records_json) {
    return -1;
  }
  *out_records_json = query_only_memory_store_strdup(
      "[{\"id\":\"project/demo::broken\",\"namespace\":\"project/demo\",\"kind\":\"context\"}]");
  return *out_records_json ? 0 : -1;
}

static turbo_agent_memory_store_t malformed_query_only_memory_store_create(void) {
  turbo_agent_memory_store_t store = {0};
  store.query = malformed_query_only_memory_store_query;
  return store;
}

static char *memory_store_temp_root(void) {
  int needed;
  char *path;
  unsigned long long tick = (unsigned long long)salts_hrtime();

#ifdef _WIN32
  char temp_dir[MAX_PATH];
  DWORD len = GetTempPathA(MAX_PATH, temp_dir);
  check_true(len > 0);
  needed = snprintf(NULL, 0, "%sturbonet_memory_store_%llx", temp_dir, tick);
#else
  const char *temp_dir = "/tmp/";
  needed = snprintf(NULL, 0, "%sturbonet_memory_store_%llx", temp_dir, tick);
#endif
  path = (char *)malloc((size_t)needed + 1);
  check_not_null(path);
#ifdef _WIN32
  snprintf(path, (size_t)needed + 1, "%sturbonet_memory_store_%llx", temp_dir, tick);
  _mkdir(path);
#else
  snprintf(path, (size_t)needed + 1, "%sturbonet_memory_store_%llx", temp_dir, tick);
  mkdir(path, 0777);
#endif
  return path;
}

spec("turbo agent memory store api") {

  it("should round-trip namespaced memory in heap-backed store") {
    turbo_agent_memory_store_t store = turbo_agent_memory_store_memory_create();
    char *value_json = NULL;
    json_value_t *records = NULL;
    json_value_t *record_views = NULL;
    json_value_t *record_view = NULL;
    json_value_t *queried = NULL;
    turbo_agent_memory_query_options_t options = {0};
    const json_value_t *record;

    check_equal(
        turbo_agent_memory_put(&store, "user/alice", "profile", "{\"name\":\"Alice\"}"), 0);
    check_equal(
        turbo_agent_memory_get(&store, "user/alice", "profile", &value_json), 0);
    check_equal(value_json, "{\"name\":\"Alice\"}");
    free(value_json);

    check_equal(turbo_agent_memory_list(&store, "user/", &records), 0);
    check_equal(json_array_size(records), 1);
    record = json_array_get(records, 0);
    check_equal(json_get_string(record, "namespace"), "user/alice");
    check_equal(json_get_string(record, "key"), "profile");
    check_equal(json_get_string(record, "value_json"), "{\"name\":\"Alice\"}");

    check_equal(turbo_agent_memory_put(&store, "project/demo", "notes", "{\"text\":\"remember\"}"),
                 0);
    check_equal(turbo_agent_memory_list_records(&store, "project/", &record_views), 0);
    check_equal(json_array_size(record_views), 1);
    turbo_agent_test_check_memory_record_fixture(json_array_get(record_views, 0),
                                                 "memory_json_record.golden.json", NULL);
    json_free(record_views); record_views = NULL;
    check_equal(
        turbo_agent_memory_put(&store, "project/demo", "context",
                               "{\"scope\":\"project\",\"path\":\"/tmp/notes.md\",\"text\":\"remember this\"}"),
        0);

    check_equal(
        turbo_agent_memory_query_records(&store, "project/", "context", "con",
                                         "remember", &queried),
        0);
    check_equal(json_array_size(queried), 1);
    record = json_array_get(queried, 0);
    check_equal(json_get_string(record, "id"), "project/demo::context");
    check_equal(json_get_string(record, "namespace"), "project/demo");
    check_equal(json_get_string(record, "kind"), "context");
    check_equal(json_get_string(record, "key"), "context");
    check_equal(json_get_string(record, "text"), "remember this");
    check_equal(json_get_string(json_object_get(record, "metadata"), "scope"),
                 "project");
    check_equal(json_get_string(json_object_get(record, "metadata"), "path"),
                 "/tmp/notes.md");
    check_true(json_type(json_object_get(record, "created_at")) == JSON_NULL);
    options.namespace_prefix = "project/";
    options.kind = "context";
    options.key_prefix = "con";
    options.text_substring = "remember";
    check_equal(turbo_agent_memory_query_records_ex(&store, &options, &record_views), 0);
    turbo_agent_test_check_memory_record_array_fixture(record_views,
                                                       "memory_query_results.golden.json",
                                                       "context_query");
    json_free(record_views); record_views = NULL;
    record_views = NULL;
    check_equal(turbo_agent_memory_get_record(&store, "project/demo", "context", &record_view), 0);
    check_equal(turbo_agent_memory_validate_record(record_view), 0);
    turbo_agent_test_check_memory_record_fixture(record_view, "memory_context_record.golden.json",
                                                 NULL);

    check_equal(turbo_agent_memory_delete(&store, "user/alice", "profile"), 0);
    check_equal(turbo_agent_memory_get(&store, "user/alice", "profile", &value_json), -1);

    json_free(record_view); record_view = NULL;
    json_free(queried); queried = NULL;
    json_free(record_views); record_views = NULL;
    json_free(records); records = NULL;
    turbo_agent_memory_store_destroy(&store);
  }

  it("should persist namespaced memory in file-backed store and fail on corrupted json") {
    char *root_dir = memory_store_temp_root();
    turbo_agent_memory_store_t store = turbo_agent_memory_store_file_create(root_dir);
    turbo_agent_memory_store_t reopened;
    char *value_json = NULL;
    json_value_t *records = NULL;
    char *record_path;
    FILE *fp;

    check_not_null(root_dir);
    check_equal(
        turbo_agent_memory_put(&store, "project/demo", "notes", "{\"done\":true}"), 0);
    check_equal(
        turbo_agent_memory_get(&store, "project/demo", "notes", &value_json), 0);
    check_equal(value_json, "{\"done\":true}");
    free(value_json);
    turbo_agent_memory_store_destroy(&store);

    reopened = turbo_agent_memory_store_file_create(root_dir);
    check_equal(
        turbo_agent_memory_get(&reopened, "project/demo", "notes", &value_json), 0);
    check_equal(value_json, "{\"done\":true}");
    free(value_json);
    check_equal(turbo_agent_memory_list(&reopened, "project/", &records), 0);
    check_equal(json_array_size(records), 1);
    json_free(records); records = NULL;

    record_path = (char *)malloc(strlen(root_dir) + 128);
    check_not_null(record_path);
    snprintf(record_path, strlen(root_dir) + 128,
             "%s/records/70726f6a6563742f64656d6f__6e6f746573.json", root_dir);
    fp = fopen(record_path, "wb");
    check_not_null(fp);
    fputs("{bad json", fp);
    fclose(fp);

    check_equal(turbo_agent_memory_get(&reopened, "project/demo", "notes", &value_json), -1);
    check_equal(turbo_agent_memory_list(&reopened, "project/", &records), -1);

    free(record_path);
    free(root_dir);
    turbo_agent_memory_store_destroy(&reopened);
  }

  it("should expose canonical records through one query-only store callback") {
    turbo_agent_memory_store_t store = query_only_memory_store_create();
    json_value_t *record_views = NULL;
    json_value_t *queried = NULL;

    check_equal(turbo_agent_memory_list_records(&store, "project", &record_views), 0);
    check_equal(json_array_size(record_views), 1);
    turbo_agent_test_check_memory_record_fixture(json_array_get(record_views, 0),
                                                 "memory_context_record.golden.json", NULL);

    check_equal(
        turbo_agent_memory_query_records(&store, "project", "context", "con", "remember",
                                         &queried),
        0);
    turbo_agent_test_check_memory_record_array_fixture(queried, "memory_query_results.golden.json",
                                                       "context_query");

    json_free(queried); queried = NULL;
    json_free(record_views); record_views = NULL;
  }

  it("should sort and limit canonical memory query results locally") {
    turbo_agent_memory_store_t store = sortable_query_only_memory_store_create();
    turbo_agent_memory_query_options_t options = {0};
    json_value_t *queried = NULL;
    const json_value_t *record;

    options.sort_by = "key";
    options.sort_order = "asc";
    check_equal(turbo_agent_memory_query_records_ex(&store, &options, &queried), 0);
    check_equal(json_array_size(queried), 3);
    record = json_array_get(queried, 0);
    check_equal(json_get_string(record, "key"), "alpha");
    record = json_array_get(queried, 1);
    check_equal(json_get_string(record, "key"), "beta");
    record = json_array_get(queried, 2);
    check_equal(json_get_string(record, "key"), "gamma");
    json_free(queried); queried = NULL;
    queried = NULL;

    options.sort_order = "desc";
    check_equal(turbo_agent_memory_query_records_ex(&store, &options, &queried), 0);
    check_equal(json_array_size(queried), 3);
    record = json_array_get(queried, 0);
    check_equal(json_get_string(record, "key"), "gamma");
    record = json_array_get(queried, 1);
    check_equal(json_get_string(record, "key"), "beta");
    record = json_array_get(queried, 2);
    check_equal(json_get_string(record, "key"), "alpha");
    json_free(queried); queried = NULL;
    queried = NULL;

    options.sort_by = NULL;
    options.sort_order = NULL;
    options.limit = 2;
    check_equal(turbo_agent_memory_query_records_ex(&store, &options, &queried), 0);
    check_equal(json_array_size(queried), 2);
    record = json_array_get(queried, 0);
    check_equal(json_get_string(record, "key"), "gamma");
    record = json_array_get(queried, 1);
    check_equal(json_get_string(record, "key"), "alpha");

    json_free(queried); queried = NULL;
  }

  it("should reject invalid canonical memory query sort options") {
    turbo_agent_memory_store_t store = sortable_query_only_memory_store_create();
    turbo_agent_memory_query_options_t options = {0};
    json_value_t *queried = NULL;

    options.sort_by = "created_at";
    options.sort_order = "asc";
    check_equal(turbo_agent_memory_query_records_ex(&store, &options, &queried), -1);
    check_null(queried);

    options.sort_by = "key";
    options.sort_order = "sideways";
    check_equal(turbo_agent_memory_query_records_ex(&store, &options, &queried), -1);
    check_null(queried);
  }

  it("should filter canonical memory query results by id and metadata locally") {
    turbo_agent_memory_store_t store = sortable_query_only_memory_store_create();
    turbo_agent_memory_query_options_t options = {0};
    json_value_t *queried = NULL;
    const json_value_t *record;

    options.id_prefix = "project/demo::a";
    check_equal(turbo_agent_memory_query_records_ex(&store, &options, &queried), 0);
    check_equal(json_array_size(queried), 1);
    record = json_array_get(queried, 0);
    check_equal(json_get_string(record, "key"), "alpha");
    json_free(queried); queried = NULL;
    queried = NULL;

    memset(&options, 0, sizeof(options));
    options.metadata_scope = "project";
    options.metadata_path_prefix = "/tmp/";
    options.sort_by = "key";
    options.sort_order = "asc";
    check_equal(turbo_agent_memory_query_records_ex(&store, &options, &queried), 0);
    check_equal(json_array_size(queried), 2);
    record = json_array_get(queried, 0);
    check_equal(json_get_string(record, "key"), "alpha");
    record = json_array_get(queried, 1);
    check_equal(json_get_string(record, "key"), "gamma");
    json_free(queried); queried = NULL;
    queried = NULL;

    memset(&options, 0, sizeof(options));
    options.metadata_scope = "team";
    options.metadata_path_prefix = "/var/";
    check_equal(turbo_agent_memory_query_records_ex(&store, &options, &queried), 0);
    check_equal(json_array_size(queried), 1);
    record = json_array_get(queried, 0);
    check_equal(json_get_string(record, "key"), "beta");
    json_free(queried); queried = NULL;
    queried = NULL;

    options.metadata_path_prefix = "/tmp/";
    check_equal(turbo_agent_memory_query_records_ex(&store, &options, &queried), 0);
    check_equal(json_array_size(queried), 0);
    json_free(queried); queried = NULL;
  }

  it("should filter canonical memory query results by created_at window locally") {
    turbo_agent_memory_store_t store = sortable_query_only_memory_store_create();
    turbo_agent_memory_query_options_t options = {0};
    json_value_t *queried = NULL;
    const json_value_t *record;

    options.created_after = "2026-01-10T09:00:00Z";
    options.created_before = "2026-01-31T23:59:59Z";
    check_equal(turbo_agent_memory_query_records_ex(&store, &options, &queried), 0);
    check_equal(json_array_size(queried), 1);
    record = json_array_get(queried, 0);
    check_equal(json_get_string(record, "key"), "alpha");
    json_free(queried); queried = NULL;
    queried = NULL;

    memset(&options, 0, sizeof(options));
    options.created_after = "2026-02-01T00:00:00Z";
    options.sort_by = "key";
    options.sort_order = "asc";
    check_equal(turbo_agent_memory_query_records_ex(&store, &options, &queried), 0);
    check_equal(json_array_size(queried), 1);
    record = json_array_get(queried, 0);
    check_equal(json_get_string(record, "key"), "beta");
    json_free(queried); queried = NULL;
    queried = NULL;

    memset(&options, 0, sizeof(options));
    options.created_before = "2026-02-01T00:00:00Z";
    check_equal(turbo_agent_memory_query_records_ex(&store, &options, &queried), 0);
    check_equal(json_array_size(queried), 1);
    record = json_array_get(queried, 0);
    check_equal(json_get_string(record, "key"), "alpha");
    json_free(queried); queried = NULL;
    queried = NULL;

    memset(&options, 0, sizeof(options));
    options.created_after = "2026-01-01T00:00:00Z";
    options.created_before = "2026-12-31T23:59:59Z";
    check_equal(turbo_agent_memory_query_records_ex(&store, &options, &queried), 0);
    check_equal(json_array_size(queried), 2);
    record = json_array_get(queried, 0);
    check_equal(json_get_string(record, "key"), "alpha");
    record = json_array_get(queried, 1);
    check_equal(json_get_string(record, "key"), "beta");
    json_free(queried); queried = NULL;
  }

  it("should fail loudly on malformed canonical query results") {
    turbo_agent_memory_store_t store = malformed_query_only_memory_store_create();
    json_value_t *queried = NULL;

    check_equal(
        turbo_agent_memory_query_records(&store, "project", "context", NULL, NULL, &queried), -1);
    check_null(queried);
  }

  it("should persist one canonical context record through record helpers") {
    turbo_agent_memory_store_t store = turbo_agent_memory_store_memory_create();
    json_value_t *record = turbo_agent_test_load_fixture_json("memory_context_record.golden.json");
    json_value_t *loaded = NULL;

    check_not_null(record);
    check_equal(turbo_agent_memory_validate_record(record), 0);
    check_equal(turbo_agent_memory_put_record(&store, record), 0);
    check_equal(turbo_agent_memory_get_record(&store, "project/demo", "context", &loaded), 0);
    turbo_agent_test_check_memory_record_fixture(loaded, "memory_context_record.golden.json", NULL);

    json_free(loaded); loaded = NULL;
    json_free(record); record = NULL;
    turbo_agent_memory_store_destroy(&store);
  }

  it("should reject malformed canonical records through record helpers") {
    turbo_agent_memory_store_t store = turbo_agent_memory_store_memory_create();
    json_value_t *record = json_create_object();

    check_not_null(record);
    json_object_set_string(record, "id", "project/demo::context");
    json_object_set_string(record, "namespace", "project/demo");
    json_object_set_string(record, "kind", "context");
    json_object_set_string(record, "key", "context");
    json_object_set_string(record, "text", "remember this");
    json_object_set_null(record, "metadata");
    json_object_set_null(record, "created_at");

    check_equal(turbo_agent_memory_validate_record(record), -1);
    check_equal(turbo_agent_memory_put_record(&store, record), -1);

    json_free(record); record = NULL;
    turbo_agent_memory_store_destroy(&store);
  }

  it("should keep memory record and query golden fixtures parseable") {
    json_value_t *record_fixture =
        turbo_agent_test_load_fixture_json("memory_context_record.golden.json");
    json_value_t *json_record_fixture =
        turbo_agent_test_load_fixture_json("memory_json_record.golden.json");
    json_value_t *query_fixture =
        turbo_agent_test_load_fixture_json("memory_query_results.golden.json");

    turbo_agent_test_check_memory_record_fixture(record_fixture, "memory_context_record.golden.json",
                                                 NULL);
    turbo_agent_test_check_memory_record_fixture(json_record_fixture,
                                                 "memory_json_record.golden.json", NULL);
    turbo_agent_test_check_memory_record_array_fixture(
        json_object_get(query_fixture, "context_query"), "memory_query_results.golden.json",
        "context_query");
    turbo_agent_test_check_memory_record_array_fixture(
        json_object_get(query_fixture, "json_query"), "memory_query_results.golden.json",
        "json_query");

    json_free(query_fixture); query_fixture = NULL;
    json_free(json_record_fixture); json_record_fixture = NULL;
    json_free(record_fixture); record_fixture = NULL;
  }
}
