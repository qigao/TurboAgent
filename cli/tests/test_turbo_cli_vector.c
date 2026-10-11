#include "tinytest.h"
#include "turbo_cli.h"
#include "turbo_cli_vector.h"
#include "turbo_cli_knowledge.h"
#include "turbo_vector_store.h"
#include "turbo_agent_http_test_support.h"
#include <cmeta_fs.h>
#include <cmeta_process.h>
#include <fmt.h>
#include <salts/clock.h>
#include <sqlite3.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef struct embedding_reply_s {
  atomic_int calls, invalid_request, mode, fail_at;
  atomic_int defer_at, defer_state, ready;
  chttp_server_deferred deferred;
  int defer_status;
} embedding_reply_t;

enum { VECTOR_DEFER_DISABLED, VECTOR_DEFER_PENDING, VECTOR_DEFER_CLAIMED,
       VECTOR_CANCEL_WAIT_MS = 5000, VECTOR_SERVER_IDLE_MS = 60000 };

static atomic_int vector_signal_interrupted;
static void vector_signal_handler(int signal_number) {
  (void)signal_number;
  atomic_store_explicit(&vector_signal_interrupted, 1, memory_order_relaxed);
}

typedef struct vector_job_s {
  const turbo_cli_options_t *options;
  const turbo_agent_config_t *config;
  FILE *input, *output, *diagnostics;
  atomic_int done;
  int result;
} vector_job_t;

static void vector_job_run(void *user) {
  vector_job_t *job = user;
  job->result = turbo_cli_run(job->options, job->input, job->output, job->diagnostics,
      job->config, &vector_signal_interrupted);
  atomic_store_explicit(&job->done, 1, memory_order_release);
}

static void embedding_release(embedding_reply_t *reply) {
  int previous = atomic_exchange(&reply->defer_state, VECTOR_DEFER_DISABLED);
  if (previous == VECTOR_DEFER_CLAIMED) {
    uint64_t deadline = cmeta_monotonic_ms() + VECTOR_CANCEL_WAIT_MS;
    while (!atomic_load_explicit(&reply->ready, memory_order_acquire) && cmeta_monotonic_ms() < deadline)
      cmeta_thread_yield();
    check_true(atomic_load_explicit(&reply->ready, memory_order_acquire));
  }
  if (atomic_load_explicit(&reply->ready, memory_order_acquire) && reply->deferred.impl) {
    int status = chttp_server_deferred_cancel(&reply->deferred);
    check_true(status == SALTS_OK || status == SALTS_ENOENT);
    reply->deferred = (chttp_server_deferred){0};
  }
}

/* These deterministic vectors model synonym similarity independently of FTS. */
static int embedding_reply(void *user, const chttp_server_request_view *request,
    chttp_server_response *response) {
  embedding_reply_t *reply = user;
  json_value_t *body = json_parse(request->body, request->body_size);
  const char *input = json_get_string(body, "input");
  const char *model = json_get_string(body, "model");
  const char *format = json_get_string(body, "encoding_format");
  const char *result;
  int mode = atomic_load(&reply->mode);
  unsigned status = 200;
  int call = atomic_fetch_add(&reply->calls, 1) + 1;
  if (call == atomic_load(&reply->fail_at)) mode = 1;
  if (!input || !input[0] || !vstr_utf8_valid(vstr_from_cstr(input)) ||
      !model || strcmp(model, "fixture-embedding") || !format || strcmp(format, "float"))
    atomic_store(&reply->invalid_request, 1);
  result = input && (strstr(input, "fruit") || strstr(input, "banana"))
      ? "{\"model\":\"fixture-embedding\",\"data\":[{\"index\":0,\"embedding\":[0,2]}]}"
      : "{\"model\":\"fixture-embedding\",\"data\":[{\"index\":0,\"embedding\":[3,0]}]}";
  switch (mode) {
    case 1: status = 429; result = "{\"error\":\"PRIVATE_PROVIDER_BODY\"}"; break;
    case 2: result = "{\"model\":\"wrong-model\",\"data\":[{\"index\":0,\"embedding\":[1,0]}]}"; break;
    case 3: result = "{\"model\":\"fixture-embedding\",\"data\":[{\"index\":1,\"embedding\":[1,0]}]}"; break;
    case 4: result = "{\"model\":\"fixture-embedding\",\"data\":[{\"index\":0,\"embedding\":[0,0]}]}"; break;
    case 5: result = "{\"model\":\"fixture-embedding\",\"data\":[{\"index\":0,\"embedding\":[1,\"bad\"]}]}"; break;
    case 6: result = "{\"model\":\"fixture-embedding\",\"data\":[{\"index\":0,\"embedding\":[1,0,0]}]}"; break;
    case 7: result = "not json"; break;
    case 8: result = "{\"model\":\"fixture-embedding\",\"data\":[{\"index\":0,\"embedding\":[1e999,0]}]}"; break;
    case 9: result = "{\"model\":\"fixture-embedding\",\"data\":[]}"; break;
    case 11: result = "{\"model\":\"fixture-embedding\",\"data\":[{\"index\":0,\"embedding\":[1,2,3]}]}"; break;
    case 12: result = "{\"model\":\"fixture-embedding\",\"data\":[{\"index\":0,\"embedding\":[1e308,-1e308]}]}"; break;
    case 13: result = "{\"model\":\"fixture-embedding\",\"data\":[{\"index\":0,\"embedding\":[1e-300,1e-300]}]}"; break;
    default: break;
  }
  json_free(body);
  if (call == atomic_load(&reply->defer_at)) {
    int expected = VECTOR_DEFER_PENDING;
    if (atomic_compare_exchange_strong(&reply->defer_state, &expected, VECTOR_DEFER_CLAIMED)) {
      reply->defer_status = chttp_server_response_defer(response, &reply->deferred);
      atomic_store_explicit(&reply->ready, 1, memory_order_release);
      return reply->defer_status;
    }
  }
  if (mode == 10) {
    tstr oversized = tstr_new_len(NULL, 256 * 1024 + 1);
    if (!oversized) return SALTS_ENOMEM;
    memset(oversized, 'x', tstr_len(oversized));
    int rc = chttp_server_reply(response, 200, "application/json", oversized, tstr_len(oversized));
    tstr_free(oversized);
    return rc;
  }
  return chttp_server_reply(response, status, "application/json", result, strlen(result));
}

typedef struct vector_chat_s { int calls, evidence, source, coding; } vector_chat_t;
static int vector_chat(const char *request, char **out, void *user) {
  vector_chat_t *chat = user;
  const char response[] = "{\"output\":[{\"type\":\"message\",\"role\":\"assistant\","
      "\"content\":[{\"type\":\"output_text\",\"text\":\"saved_answer\"}]}]}";
  ++chat->calls;
  chat->evidence = strstr(request, "car_evidence_marker") != NULL;
  chat->source = strstr(request, "note.md") != NULL;
  chat->coding = strstr(request, "fs_read") != NULL;
  *out = malloc(sizeof(response));
  if (!*out) return -1;
  memcpy(*out, response, sizeof(response));
  return 0;
}

static int vector_write(const char *root, const char *text) {
  tstr path = tstr_format("{}/note.md", root);
  cmeta_fs_buf_t bytes = {(char *)text, strlen(text)};
  int rc = path ? cmeta_fs_write_file(path, &bytes) : -1;
  tstr_free(path);
  return rc;
}

static int vector_sql(const char *path, const char *sql) {
  sqlite3 *db = NULL;
  int rc = sqlite3_open(path, &db);
  if (rc == SQLITE_OK) rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
  if (db) { int closed = sqlite3_close(db); if (rc == SQLITE_OK) rc = closed; }
  return rc;
}

/* Isolate this fixture from credentials in the launching shell. */
static int vector_test_key(const char *value) {
#ifdef _WIN32
  return _putenv_s("TURBOAGENT_EMBEDDING_API_KEY", value ? value : "");
#else
  return value ? setenv("TURBOAGENT_EMBEDDING_API_KEY", value, 1) : unsetenv("TURBOAGENT_EMBEDDING_API_KEY");
#endif
}

static int vector_spawn(const turbo_cli_options_t *options, const char *root,
    const char *mode, cmeta_process_t **process) {
  const char *args[] = {options->vector_db, options->embedding_url, root, mode, NULL};
  cmeta_process_options_t child_options;
  cmeta_process_options_init(&child_options);
  child_options.program = CLI_VECTOR_FIXTURE; child_options.args = args;
  child_options.flags |= SALTS_PROCESS_PIPE_STDIN;
  child_options.timeout_ms = 15000; child_options.max_output_bytes = 4096;
  return cmeta_process_spawn(&child_options, process);
}

static int vector_wait_ready(cmeta_process_t *process) {
  uint64_t deadline = cmeta_monotonic_ms() + VECTOR_CANCEL_WAIT_MS;
  while (cmeta_monotonic_ms() < deadline) {
    char ready = 0;
    size_t count = 0;
    int rc = cmeta_process_read_stdout(process, &ready, 1, &count);
    if (count) return ready == 'R' ? SALTS_OK : SALTS_EPROTO;
    if (rc != SALTS_OK) return rc;
    cmeta_thread_yield();
  }
  return SALTS_ETIMEDOUT;
}

spec("CLI persistent vector retrieval") {
  static chttp_server server;
  static int started;
  static embedding_reply_t reply;
  static vector_chat_t chat;
  static char *root;
  static tstr path, session, url, text, saved_key;
  static FILE *input, *output, *diagnostics;
  static turbo_cli_options_t options;
  static turbo_agent_config_t config;
  static turbo_cli_vector_t *vector;
  static json_value_t *state;
  static cmeta_threadpool_t *worker;
  static cmeta_process_t *process;
  static vector_job_t job;
  static void (*previous_signal_handler)(int);
  static int signal_installed;

  before_each() {
    uint16_t port = 0;
    const char *key = getenv("TURBOAGENT_EMBEDDING_API_KEY");
    saved_key = key ? tstr_dup(key) : NULL;
    if (key) check_not_null(saved_key);
    check_equal(vector_test_key("fixture-only-key"), 0);
    atomic_init(&reply.calls, 0); atomic_init(&reply.invalid_request, 0); atomic_init(&reply.mode, 0);
    atomic_init(&reply.fail_at, 0);
    atomic_init(&reply.defer_at, 0); atomic_init(&reply.defer_state, VECTOR_DEFER_DISABLED);
    atomic_init(&reply.ready, 0); reply.deferred = (chttp_server_deferred){0}; reply.defer_status = SALTS_EINVAL;
    atomic_store(&vector_signal_interrupted, 0);
    job = (vector_job_t){0}; atomic_init(&job.done, 0);
    chttp_server_config server_config = agent_http_test_server_config();
    server_config.network.read_timeout_ms = VECTOR_SERVER_IDLE_MS;
    server_config.max_response_body_bytes = 512 * 1024;
    server_config.network.max_send_bytes = 512 * 1024;
    check_equal(chttp_server_init(&server, &server_config), SALTS_OK);
    check_equal(chttp_server_route(&server, CHTTP_METHOD_POST, "/v1/embeddings", embedding_reply, &reply), SALTS_OK);
    check_equal(chttp_server_start(&server), SALTS_OK); started = 1;
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    root = tt_make_temp_dir("turbo-cli-vector"); check_not_null(root);
    path = tstr_format("{}/vector.db", root); session = tstr_format("{}/session.db", root);
    url = tstr_format("http://127.0.0.1:{}/v1/embeddings", (unsigned)port);
    check_not_null(path); check_not_null(session); check_not_null(url);
    input = tmpfile(); output = tmpfile(); diagnostics = tmpfile();
    check_not_null(input); check_not_null(output); check_not_null(diagnostics);
    options = (turbo_cli_options_t){.workspace = root, .mode = "general", .prompt = "automobile",
        .vector_db = path, .embedding_url = url, .embedding_model = "fixture-embedding"};
    chat = (vector_chat_t){0};
    config = (turbo_agent_config_t){.model = "fixture-chat", .transport_fn = vector_chat, .transport_user_data = &chat};
  }
  after_each() {
    cmeta_process_destroy(process); process = NULL;
    atomic_store(&vector_signal_interrupted, 1);
    if (worker) {
      check_equal(cmeta_threadpool_wait_status(worker), SALTS_OK);
      cmeta_threadpool_destroy(worker); worker = NULL;
    }
    if (signal_installed) {
      signal(SIGINT, previous_signal_handler); signal_installed = 0;
    }
    embedding_release(&reply);
    check_equal(turbo_cli_vector_close(vector), SALTS_OK); vector = NULL;
    json_free(state); state = NULL;
    if (started) { check_equal(chttp_server_stop(&server, 5000), SALTS_OK); started = 0; }
    if (server.impl) check_equal(chttp_server_destroy(&server), SALTS_OK);
    check_equal(atomic_load(&reply.invalid_request), 0);
    if (input) fclose(input); if (output) fclose(output); if (diagnostics) fclose(diagnostics);
    input = output = diagnostics = NULL;
    tstr_free(path); path = NULL; tstr_free(session); session = NULL;
    tstr_free(url); url = NULL; tstr_free(text); text = NULL;
    if (root) tt_remove_tree(root); free(root); root = NULL;
    check_equal(vector_test_key(saved_key), 0); tstr_free(saved_key); saved_key = NULL;
  }

  it("parses explicit embedding settings without changing defaults") {
    char *args[] = {"turboagent", "--vector-db=vector.db", "--embedding-url=https://example.com/v1/embeddings",
        "--embedding-model=fixture-embedding", NULL};
    turbo_cli_parse_options(4, args, &options);
    check_equal(options.vector_db, "vector.db"); check_equal(options.embedding_model, "fixture-embedding");
    check_equal(options.embedding_url, "https://example.com/v1/embeddings");
    check_equal(turbo_cli_vector_validate(&options), SALTS_OK);
    turbo_cli_parse_options(1, args, &options);
    check_null(options.vector_db); check_null(options.embedding_url); check_null(options.embedding_model);
  }

  it("rejects incompatible options and unsafe endpoints before chat or HTTP") {
    const char *invalid[] = {"http://example.com/v1/embeddings", "https://user:pass@example.com/v1/embeddings",
        "https://example.com/v1/embeddings?key=secret", "https://example.com/v1/embeddings#fragment",
        "file:///embeddings", "https://example.com/with space"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
      options.embedding_url = invalid[i];
      check_equal(turbo_cli_run(&options, input, output, diagnostics, &config, NULL), 2);
    }
    options.embedding_url = url; options.knowledge_db = path;
    check_equal(turbo_cli_vector_validate(&options), SALTS_EINVAL);
    options.knowledge_db = NULL; options.embedding_model = NULL;
    check_equal(turbo_cli_vector_validate(&options), SALTS_EINVAL);
    options.vector_db = NULL;
    check_equal(turbo_cli_vector_validate(&options), SALTS_EINVAL);
    check_equal(chat.calls, 0); check_equal(atomic_load(&reply.calls), 0);
    cmeta_fs_stat_t info; check_not_equal(cmeta_fs_stat(path, &info), 0);
  }

  it("retrieves semantic evidence and reopens the index while resuming a persisted conversation") {
    check_equal(vector_write(root, "car car_evidence_marker"), 0);
    options.index_dir = root; options.session_db = session;
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &config, NULL), 0);
    check_equal(atomic_load(&reply.calls), 2); check_true(chat.evidence); check_true(chat.source); check_false(chat.coding);
    chat = (vector_chat_t){0}; options.index_dir = NULL; options.resume = 1;
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &config, NULL), 0);
    check_equal(atomic_load(&reply.calls), 3); check_equal(chat.calls, 1); check_true(chat.evidence);
  }

  it("adds vector evidence to the coding agent") {
    check_equal(vector_write(root, "car car_evidence_marker"), 0);
    options.mode = "coding"; options.read_only = 1; options.index_dir = root;
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &config, NULL), 0);
    check_true(chat.coding); check_true(chat.evidence); check_true(chat.source);
  }

  for (int exit_kind = 0; exit_kind < 3; ++exit_kind) {
    it("excludes a competing process and recovers after %s",
        exit_kind == 0 ? "orderly close" : exit_kind == 1 ? "abrupt exit" : "forced termination") {
      check_equal(vector_write(root, "car car_evidence_marker"), 0);
      check_equal(turbo_cli_vector_open(&options, root, NULL, &vector), SALTS_OK);
      check_equal(turbo_cli_vector_close(vector), SALTS_OK); vector = NULL;
      check_equal(vector_spawn(&options, root, "hold", &process), SALTS_OK);
      check_equal(vector_wait_ready(process), SALTS_OK);
      check_true(cmeta_process_is_running(process));
      check_equal(turbo_cli_vector_open(&options, root, NULL, &vector), SALTS_EBUSY);
      check_null(vector);
      options.index_dir = root;
      check_equal(turbo_cli_run(&options, input, output, diagnostics, &config, NULL), 1);
      check_equal(chat.calls, 0); check_equal(atomic_load(&reply.calls), 1);

      if (exit_kind == 2) check_equal(cmeta_process_terminate(process), SALTS_OK);
      else {
        size_t written = 0;
        check_equal(cmeta_process_write_stdin(process, exit_kind ? "X" : "C", 1, &written), SALTS_OK);
        check_equal(written, 1u);
      }
      cmeta_process_result_t result;
      check_equal(cmeta_process_wait_for(process, VECTOR_CANCEL_WAIT_MS, &result), SALTS_OK);
      check_equal(result.state, exit_kind == 2 ? SALTS_PROCESS_TERMINATED : SALTS_PROCESS_EXITED);
      if (exit_kind != 2) check_equal(result.exit_code, exit_kind ? 23 : 0);
      /* Leave the sidecar in place: OS ownership, not file existence, controls admission. */
      options.index_dir = NULL;
      check_equal(turbo_cli_run(&options, input, output, diagnostics, &config, NULL), 0);
      check_equal(chat.calls, 1); check_true(chat.evidence); check_true(chat.source);
      check_equal(atomic_load(&reply.calls), 2);
    }
  }

  it("rejects a child importer while the parent owns the index") {
    check_equal(vector_write(root, "car car_evidence_marker"), 0);
    check_equal(turbo_cli_vector_open(&options, root, NULL, &vector), SALTS_OK);
    check_equal(vector_spawn(&options, root, "import", &process), SALTS_OK);
    cmeta_process_result_t result;
    check_equal(cmeta_process_wait_for(process, VECTOR_CANCEL_WAIT_MS, &result), SALTS_OK);
    check_equal(result.state, SALTS_PROCESS_EXITED); check_equal(result.exit_code, 24);
    check_equal(atomic_load(&reply.calls), 1);
    state = turbo_agent_state_create(); check_not_null(state);
    check_equal(turbo_cli_vector_prepare(vector, "automobile", state), SALTS_OK);
    check_contains(json_get_string(json_array_get(turbo_agent_state_memory_layers(state), 0), "text"),
        "car_evidence_marker");
  }

  it("preserves committed chunks when a child dies during replacement embedding") {
    check_equal(vector_write(root, "car car_evidence_marker"), 0);
    check_equal(turbo_cli_vector_open(&options, root, NULL, &vector), SALTS_OK);
    check_equal(turbo_cli_vector_close(vector), SALTS_OK); vector = NULL;
    text = tstr_new_len(NULL, 4096); check_not_null(text); memset(text, 'a', tstr_len(text));
    check_equal(vector_write(root, text), 0);
    atomic_store(&reply.defer_at, 3); /* Old chunk, then two replacement chunks. */
    atomic_store(&reply.defer_state, VECTOR_DEFER_PENDING);
    check_equal(vector_spawn(&options, root, "import", &process), SALTS_OK);
    uint64_t deadline = cmeta_monotonic_ms() + VECTOR_CANCEL_WAIT_MS;
    while (!atomic_load_explicit(&reply.ready, memory_order_acquire) &&
        cmeta_process_is_running(process) && cmeta_monotonic_ms() < deadline)
      cmeta_thread_yield();
    check_true(atomic_load_explicit(&reply.ready, memory_order_acquire));
    check_equal(reply.defer_status, SALTS_OK);
    check_equal(atomic_load(&reply.calls), 3);
    check_equal(turbo_cli_vector_open(&options, NULL, NULL, &vector), SALTS_EBUSY);
    check_null(vector);
    check_equal(cmeta_process_terminate(process), SALTS_OK);
    cmeta_process_result_t result;
    check_equal(cmeta_process_wait_for(process, VECTOR_CANCEL_WAIT_MS, &result), SALTS_OK);
    check_equal(result.state, SALTS_PROCESS_TERMINATED);
    embedding_release(&reply);
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &config, NULL), 0);
    check_equal(chat.calls, 1); check_true(chat.evidence); check_true(chat.source);
    check_equal(atomic_load(&reply.calls), 4);
  }

  it("embeds a query against an empty index without claiming evidence") {
    check_equal(turbo_cli_vector_open(&options, NULL, NULL, &vector), SALTS_OK);
    state = turbo_agent_state_create(); check_not_null(state);
    int rc = turbo_cli_vector_prepare(vector, "automobile", state);
    check_equal(atomic_load(&reply.calls), 1);
    check_equal(rc, SALTS_OK);
    check_equal(json_array_size(turbo_agent_state_memory_layers(state)), 1u);
  }

  it("ranks documents by embedding similarity for each fresh query") {
    check_equal(vector_write(root, "car car_evidence_marker"), 0);
    text = tstr_format("{}/fruit.txt", root); check_not_null(text);
    char fruit[] = "fruit fruit_evidence_marker";
    cmeta_fs_buf_t bytes = {fruit, sizeof(fruit) - 1};
    check_equal(cmeta_fs_write_file(text, &bytes), 0);
    check_equal(turbo_cli_vector_open(&options, root, NULL, &vector), SALTS_OK);
    state = turbo_agent_state_create(); check_not_null(state);
    check_equal(turbo_cli_vector_prepare(vector, "automobile", state), SALTS_OK);
    check_contains(json_get_string(json_array_get(turbo_agent_state_memory_layers(state), 0), "text"), "car_evidence_marker");
    json_free(state); state = turbo_agent_state_create(); check_not_null(state);
    check_equal(turbo_cli_vector_prepare(vector, "banana", state), SALTS_OK);
    check_contains(json_get_string(json_array_get(turbo_agent_state_memory_layers(state), 0), "text"), "fruit_evidence_marker");
  }

  it("replaces text on reimport and removes chunks for an empty file without embedding it") {
    check_equal(vector_write(root, "car car_evidence_marker"), 0);
    check_equal(turbo_cli_vector_open(&options, root, NULL, &vector), SALTS_OK);
    check_equal(turbo_cli_vector_close(vector), SALTS_OK); vector = NULL;
    check_equal(vector_write(root, "fruit replacement_marker"), 0);
    check_equal(turbo_cli_vector_open(&options, root, NULL, &vector), SALTS_OK);
    state = turbo_agent_state_create(); check_not_null(state);
    check_equal(turbo_cli_vector_prepare(vector, "banana", state), SALTS_OK);
    check_equal(json_array_size(turbo_agent_state_memory_layers(state)), 2u);
    check_contains(json_get_string(json_array_get(turbo_agent_state_memory_layers(state), 0), "text"), "replacement_marker");
    check_equal(turbo_cli_vector_close(vector), SALTS_OK); vector = NULL;
    check_equal(vector_write(root, ""), 0);
    int calls = atomic_load(&reply.calls);
    check_equal(turbo_cli_vector_open(&options, root, NULL, &vector), SALTS_OK);
    check_equal(atomic_load(&reply.calls), calls);
    json_free(state); state = turbo_agent_state_create(); check_not_null(state);
    check_equal(turbo_cli_vector_prepare(vector, "automobile", state), SALTS_OK);
    check_equal(json_array_size(turbo_agent_state_memory_layers(state)), 1u);
  }

  it("preserves old document chunks when embedding a replacement fails") {
    check_equal(vector_write(root, "car car_evidence_marker"), 0);
    check_equal(turbo_cli_vector_open(&options, root, NULL, &vector), SALTS_OK);
    check_equal(turbo_cli_vector_close(vector), SALTS_OK); vector = NULL;
    text = tstr_new_len(NULL, 4096); check_not_null(text); memset(text, 'a', tstr_len(text));
    check_equal(vector_write(root, text), 0);
    atomic_store(&reply.fail_at, 3); /* Old document, then two replacement chunks. */
    check_not_equal(turbo_cli_vector_open(&options, root, NULL, &vector), SALTS_OK); check_null(vector);
    check_equal(atomic_load(&reply.calls), 3);
    atomic_store(&reply.fail_at, 0);
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &config, NULL), 0);
    check_true(chat.evidence);
  }

  it("refuses a different endpoint or model without changing the persisted identity") {
    check_equal(turbo_cli_vector_open(&options, NULL, NULL, &vector), SALTS_OK);
    check_equal(turbo_cli_vector_close(vector), SALTS_OK); vector = NULL;
    options.embedding_model = "different-model";
    check_equal(turbo_cli_vector_open(&options, NULL, NULL, &vector), SALTS_EPROTO); check_null(vector);
    options.embedding_model = "fixture-embedding"; options.embedding_url = "https://example.com/v1/embeddings";
    check_equal(turbo_cli_vector_open(&options, NULL, NULL, &vector), SALTS_EPROTO); check_null(vector);
    options.embedding_url = url;
    check_equal(turbo_cli_vector_open(&options, NULL, NULL, &vector), SALTS_OK);
    check_equal(atomic_load(&reply.calls), 0);
  }

  for (int mode = 1; mode <= 10; ++mode) {
    it("rejects provider failure mode %d before chat without retry or fallback", mode) {
      check_equal(vector_write(root, "car car_evidence_marker"), 0);
      check_equal(turbo_cli_vector_open(&options, root, NULL, &vector), SALTS_OK);
      check_equal(turbo_cli_vector_close(vector), SALTS_OK); vector = NULL;
      atomic_store(&reply.mode, mode);
      check_equal(turbo_cli_run(&options, input, output, diagnostics, &config, NULL), 1);
      check_equal(chat.calls, 0); check_equal(atomic_load(&reply.calls), 2);
      char diagnostic[1024]; rewind(diagnostics);
      size_t size = fread(diagnostic, 1, sizeof(diagnostic) - 1, diagnostics); diagnostic[size] = 0;
      check_null(strstr(diagnostic, "PRIVATE_PROVIDER_BODY"));
      check_null(strstr(diagnostic, "fixture-only-key"));
    }
  }

  it("refuses unmanaged vector and knowledge databases without adding CLI metadata") {
    turbo_vector_store_t *store = turbo_vector_store_sqlite_open(path); check_not_null(store);
    turbo_vector_store_destroy(store);
    check_equal(turbo_cli_vector_open(&options, NULL, NULL, &vector), SALTS_EPROTO); check_null(vector);
    check_not_equal(vector_sql(path, "SELECT * FROM cli_vector_config"), SQLITE_OK);
    turbo_agent_knowledge_store_t *knowledge = turbo_agent_knowledge_store_sqlite_open(session); check_not_null(knowledge);
    turbo_agent_knowledge_store_close(knowledge); options.vector_db = session;
    check_equal(turbo_cli_vector_open(&options, NULL, NULL, &vector), SALTS_EPROTO); check_null(vector);
    check_not_equal(vector_sql(session, "SELECT * FROM cli_vector_config"), SQLITE_OK);
    check_equal(atomic_load(&reply.calls), 0);
  }

  it("refuses a session database and preserves its resumability") {
    options.vector_db = NULL; options.embedding_url = NULL; options.embedding_model = NULL; options.session_db = session;
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &config, NULL), 0);
    options.vector_db = session; options.embedding_url = url; options.embedding_model = "fixture-embedding";
    check_equal(turbo_cli_vector_open(&options, NULL, NULL, &vector), SALTS_EPROTO); check_null(vector);
    options.vector_db = NULL; options.embedding_url = NULL; options.embedding_model = NULL; options.resume = 1;
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &config, NULL), 0);
    check_equal(chat.calls, 2); check_equal(atomic_load(&reply.calls), 0);
  }

  it("checks cancellation before import and before query without submitting HTTP") {
    atomic_int interrupted = 1;
    check_equal(turbo_cli_vector_open(&options, root, &interrupted, &vector), SALTS_ECANCELED); check_null(vector);
    interrupted = 0;
    check_equal(turbo_cli_vector_open(&options, NULL, &interrupted, &vector), SALTS_OK);
    state = turbo_agent_state_create(); check_not_null(state); interrupted = 1;
    check_equal(turbo_cli_vector_prepare(vector, "automobile", state), SALTS_ECANCELED);
    check_equal(atomic_load(&reply.calls), 0);
  }

  for (int importing = 0; importing <= 1; ++importing) {
    it("drains in-flight SIGINT during %s and preserves the saved session and index",
        importing ? "multi-chunk replacement" : "query embedding") {
      cmeta_threadpool_config_t workers = {.num_threads = 1, .queue_capacity = 1};
      check_equal(vector_write(root, "car car_evidence_marker"), 0);
      options.index_dir = root; options.session_db = session;
      check_equal(turbo_cli_run(&options, input, output, diagnostics, &config, NULL), 0);
      check_equal(chat.calls, 1); check_equal(atomic_load(&reply.calls), 2);
      long output_before_cancel = ftell(output); check_greater(output_before_cancel, 0L);
      options.resume = 1; options.index_dir = importing ? root : NULL;
      if (importing) {
        text = tstr_new_len(NULL, 4096); check_not_null(text); memset(text, 'a', tstr_len(text));
        check_equal(vector_write(root, text), 0);
      }
      atomic_store(&reply.defer_at, importing ? 4 : 3);
      atomic_store(&reply.defer_state, VECTOR_DEFER_PENDING);
      check_true(atomic_is_lock_free(&vector_signal_interrupted));
      previous_signal_handler = signal(SIGINT, vector_signal_handler);
      check_true(previous_signal_handler != SIG_ERR); signal_installed = 1;
      worker = cmeta_threadpool_create_with_config(&workers); check_not_null(worker);
      job.options = &options; job.config = &config;
      job.input = input; job.output = output; job.diagnostics = diagnostics;
      check_equal(cmeta_threadpool_submit(worker, vector_job_run, &job), SALTS_OK);
      uint64_t deadline = cmeta_monotonic_ms() + VECTOR_CANCEL_WAIT_MS;
      while (!atomic_load_explicit(&reply.ready, memory_order_acquire) &&
          !atomic_load_explicit(&job.done, memory_order_acquire) && cmeta_monotonic_ms() < deadline)
        cmeta_thread_yield();
      check_true(atomic_load_explicit(&reply.ready, memory_order_acquire));
      check_equal(reply.defer_status, SALTS_OK);
      check_false(atomic_load_explicit(&job.done, memory_order_acquire));
      check_equal(raise(SIGINT), 0);
      deadline = cmeta_monotonic_ms() + VECTOR_CANCEL_WAIT_MS;
      while (!atomic_load_explicit(&job.done, memory_order_acquire) && cmeta_monotonic_ms() < deadline)
        cmeta_thread_yield();
      /* The server still owns an unreplied deferred request: completion must
       * come from client cancellation, not a server response or idle timeout. */
      check_true(atomic_load_explicit(&job.done, memory_order_acquire));
      check_equal(cmeta_threadpool_wait_status(worker), SALTS_OK);
      check_equal(job.result, 130); check_equal(chat.calls, 1);
      check_equal(ftell(output), output_before_cancel);
      check_equal(atomic_load(&reply.calls), importing ? 4 : 3);
      embedding_release(&reply);

      /* A new CLI run must acquire both locks and resume the earlier completed
       * turn, retrieving the old document after a cancelled replacement. */
      options.index_dir = NULL; chat = (vector_chat_t){0};
      check_equal(turbo_cli_run(&options, input, output, diagnostics, &config, NULL), 0);
      check_equal(chat.calls, 1); check_true(chat.evidence);
      check_equal(atomic_load(&reply.calls), importing ? 5 : 4);
    }
  }

  it("bounds query bytes before making an embedding request") {
    check_equal(turbo_cli_vector_open(&options, NULL, NULL, &vector), SALTS_OK);
    text = tstr_new_len(NULL, 8193); check_not_null(text); memset(text, 'a', tstr_len(text));
    state = turbo_agent_state_create(); check_not_null(state);
    check_equal(turbo_cli_vector_prepare(vector, text, state), SALTS_ENOSPC);
    check_equal(atomic_load(&reply.calls), 0);
  }

  it("rejects too many persisted chunks before SDK scan or embedding") {
    check_equal(turbo_cli_vector_open(&options, NULL, NULL, &vector), SALTS_OK);
    check_equal(turbo_cli_vector_close(vector), SALTS_OK); vector = NULL;
    check_equal(vector_sql(path, "UPDATE cli_vector_config SET dimensions=2;"
        "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM n WHERE i<8193) "
        "INSERT INTO vector_documents(id,uri,text,embedding_json,dimensions,insert_order) "
        "SELECT CAST(i AS TEXT),'source','text','[1,0]',2,i FROM n"), SQLITE_OK);
    check_equal(turbo_cli_vector_open(&options, NULL, NULL, &vector), SALTS_ENOSPC); check_null(vector);
    check_equal(atomic_load(&reply.calls), 0);
  }

  it("counts stored text with nullable titles toward the byte budget") {
    check_equal(turbo_cli_vector_open(&options, NULL, NULL, &vector), SALTS_OK);
    check_equal(turbo_cli_vector_close(vector), SALTS_OK); vector = NULL;
    check_equal(vector_sql(path, "UPDATE cli_vector_config SET dimensions=2;"
        "INSERT INTO vector_documents(id,uri,title,text,embedding_json,dimensions,insert_order) "
        "VALUES('large','source',NULL,CAST(zeroblob(67108865) AS TEXT),'[1,0]',2,1)"), SQLITE_OK);
    check_equal(turbo_cli_vector_open(&options, NULL, NULL, &vector), SALTS_ENOSPC); check_null(vector);
    check_equal(atomic_load(&reply.calls), 0);
  }

  const char *invalid_stored_vectors[] = {
    "'[1]'", "'[1,0,0]'", "'{}'", "'[]'", "'[1,\"bad\"]'", "'[0,0]'",
    "'[1e999,0]'", "'[1e308,0]'", "'[0.5,0]'", "'[1,0]' || char(0) || 'trailing'"
  };
  for (size_t i = 0; i < sizeof(invalid_stored_vectors) / sizeof(invalid_stored_vectors[0]); ++i) {
    it("rejects invalid persisted embedding case %zu before requesting any model", i) {
      check_equal(vector_write(root, "car car_evidence_marker"), 0);
      check_equal(turbo_cli_vector_open(&options, root, NULL, &vector), SALTS_OK);
      check_equal(turbo_cli_vector_close(vector), SALTS_OK); vector = NULL;
      text = tstr_format("UPDATE vector_documents SET embedding_json={}", invalid_stored_vectors[i]);
      check_not_null(text); check_equal(vector_sql(path, text), SQLITE_OK);
      check_equal(turbo_cli_run(&options, input, output, diagnostics, &config, NULL), 1);
      check_equal(atomic_load(&reply.calls), 1); check_equal(chat.calls, 0);
    }
  }

  it("accepts persisted normalized non-axis-aligned vectors") {
    check_equal(vector_write(root, "car car_evidence_marker"), 0);
    check_equal(turbo_cli_vector_open(&options, root, NULL, &vector), SALTS_OK);
    check_equal(turbo_cli_vector_close(vector), SALTS_OK); vector = NULL;
    check_equal(vector_sql(path, "UPDATE vector_documents SET embedding_json='[0.6,-0.8]'"), SQLITE_OK);
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &config, NULL), 0);
    check_equal(atomic_load(&reply.calls), 2); check_true(chat.evidence);
  }

  for (int mode = 11; mode <= 13; ++mode) {
    it("normalizes response case %d and reopens its serialized vectors", mode) {
      atomic_store(&reply.mode, mode);
      check_equal(vector_write(root, "car car_evidence_marker"), 0);
      check_equal(turbo_cli_vector_open(&options, root, NULL, &vector), SALTS_OK);
      check_equal(turbo_cli_vector_close(vector), SALTS_OK); vector = NULL;
      check_equal(turbo_cli_run(&options, input, output, diagnostics, &config, NULL), 0);
      check_equal(atomic_load(&reply.calls), 2); check_true(chat.evidence);
    }
  }

  const char *boundary_characters[] = {"é", "中", "😀"};
  for (size_t character = 0; character < 3; ++character) {
    for (size_t prefix = 1; prefix < strlen(boundary_characters[character]); ++prefix) {
      it("imports a final %zu-byte UTF-8 character split after byte %zu",
          strlen(boundary_characters[character]), prefix) {
        const char *tail = boundary_characters[character];
        text = tstr_new_len(NULL, 2048 - prefix); check_not_null(text);
        memset(text, 'a', tstr_len(text));
        tstr next = tstr_cat(text, tail); check_not_null(next); text = next;
        check_equal(vector_write(root, text), 0);
        check_equal(turbo_cli_vector_open(&options, root, NULL, &vector), SALTS_OK);
        check_equal(atomic_load(&reply.calls), 2);
        check_equal(turbo_cli_vector_close(vector), SALTS_OK); vector = NULL;
        check_equal(turbo_cli_vector_open(&options, NULL, NULL, &vector), SALTS_OK);
        state = turbo_agent_state_create(); check_not_null(state);
        check_equal(turbo_cli_vector_prepare(vector, "automobile", state), SALTS_OK);
        const json_value_t *layers = turbo_agent_state_memory_layers(state);
        check_equal(json_array_size(layers), 3u);
        int found = 0;
        for (size_t i = 0; i < 2; ++i) {
          const char *snippet = json_get_string(json_array_get(layers, i), "text");
          check_not_null(snippet); check_true(vstr_utf8_valid(vstr_from_cstr(snippet)));
          if (strstr(snippet, tail)) found = 1;
        }
        check_true(found);
      }
    }
  }

  it("embeds and retrieves valid UTF-8 when byte chunks divide a codepoint") {
    text = tstr_new_len(NULL, 2047); check_not_null(text); memset(text, 'a', tstr_len(text));
    tstr next = tstr_cat(text, "中文 marker"); check_not_null(next); text = next;
    check_equal(vector_write(root, text), 0);
    check_equal(turbo_cli_vector_open(&options, root, NULL, &vector), SALTS_OK);
    check_equal(atomic_load(&reply.calls), 2);
    state = turbo_agent_state_create(); check_not_null(state);
    check_equal(turbo_cli_vector_prepare(vector, "automobile", state), SALTS_OK);
    const json_value_t *layers = turbo_agent_state_memory_layers(state);
    check_equal(json_array_size(layers), 3u);
    int found = 0;
    for (size_t i = 0; i < json_array_size(layers); ++i) {
      const char *snippet = json_get_string(json_array_get(layers, i), "text");
      check_true(vstr_utf8_valid(vstr_from_cstr(snippet)));
      if (strstr(snippet, "中文")) found = 1;
    }
    check_true(found);
  }

  it("reimports Unicode source identities without leaving old trailing chunks") {
    text = tstr_format("{}/资料.md", root); check_not_null(text);
    char document[4097]; memset(document, 'a', sizeof(document) - 1); document[4096] = 0;
    cmeta_fs_buf_t bytes = {document, sizeof(document) - 1};
    check_equal(cmeta_fs_write_file(text, &bytes), 0);
    check_equal(turbo_cli_vector_open(&options, root, NULL, &vector), SALTS_OK);
    check_equal(turbo_cli_vector_close(vector), SALTS_OK); vector = NULL;
    bytes = (cmeta_fs_buf_t){"replacement", 11};
    check_equal(cmeta_fs_write_file(text, &bytes), 0);
    check_equal(turbo_cli_vector_open(&options, root, NULL, &vector), SALTS_OK);
    state = turbo_agent_state_create(); check_not_null(state);
    check_equal(turbo_cli_vector_prepare(vector, "automobile", state), SALTS_OK);
    check_equal(json_array_size(turbo_agent_state_memory_layers(state)), 2u);
    check_equal(json_get_string(json_array_get(turbo_agent_state_memory_layers(state), 0), "text"), "replacement");
  }
}
