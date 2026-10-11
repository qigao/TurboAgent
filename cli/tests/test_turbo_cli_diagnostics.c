#include "tinytest.h"
#include "turbo_agent_http_test_support.h"
#include "turbo_cli.h"
#include <fmt.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static const char secret_body[] = "{\"error\":{\"message\":\"TEST_SECRET_NEVER_PRINT\"}}\n\033[31m";
static const char success_body[] =
    "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"completed\"},"
    "\"finish_reason\":\"stop\"}]}";

typedef struct cli_reply_s {
  atomic_int status;
  atomic_int requests;
  atomic_int success_first;
} cli_reply_t;

static int cli_http_reply(void *user, const chttp_server_request_view *request,
                          chttp_server_response *response) {
  cli_reply_t *reply = user;
  const int call = atomic_fetch_add(&reply->requests, 1);
  const int first_success = call == 0 && atomic_load(&reply->success_first);
  const int status = first_success ? 200 : atomic_load(&reply->status);
  const char *body = first_success ? success_body : secret_body;
  (void)request;
  return chttp_server_reply(response, (unsigned)status, "application/json", body, strlen(body));
}

static int cli_private_failure(const char *request, char **out, void *user) {
  (void)request;
  (void)user;
  *out = malloc(sizeof(secret_body));
  if (*out) memcpy(*out, secret_body, sizeof(secret_body));
  return -1;
}

static void cli_read_diagnostic(FILE *stream, char *text, size_t capacity) {
  size_t count;
  rewind(stream);
  count = fread(text, 1, capacity - 1, stream);
  text[count] = '\0';
}

spec("CLI failure diagnostics") {
  static chttp_server server;
  static int server_started;
  static cli_reply_t reply;
  static char *root;
  static tstr url;
  static FILE *input, *output, *diagnostics;
  static turbo_cli_options_t options;
  static turbo_agent_config_t config;

  before_each() {
    uint16_t port = 0;
    chttp_server_config server_config = agent_http_test_server_config();
    atomic_init(&reply.status, 401);
    atomic_init(&reply.requests, 0);
    atomic_init(&reply.success_first, 0);
    check_equal(chttp_server_init(&server, &server_config), SALTS_OK);
    check_equal(chttp_server_route(&server, CHTTP_METHOD_POST, "/v1/chat/completions",
                                   cli_http_reply, &reply),
                SALTS_OK);
    check_equal(chttp_server_start(&server), SALTS_OK);
    server_started = 1;
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    url = tstr_format("http://127.0.0.1:{}/v1", (unsigned)port);
    root = tt_make_temp_dir("turbo_cli_diagnostic");
    input = tmpfile();
    output = tmpfile();
    diagnostics = tmpfile();
    check_not_null(url);
    check_not_null(root);
    check_not_null(input);
    check_not_null(output);
    check_not_null(diagnostics);
    options = (turbo_cli_options_t){.workspace = root, .prompt = "hello", .read_only = 1};
    config = (turbo_agent_config_t){.model = "test-model",
                                    .api_key = "test-only-key",
                                    .base_url = url,
                                    .api_mode = TURBO_AGENT_API_CHAT_COMPLETIONS};
  }

  after_each() {
    if (input) fclose(input);
    if (output) fclose(output);
    if (diagnostics) fclose(diagnostics);
    input = output = diagnostics = NULL;
    tstr_free(url);
    url = NULL;
    if (root) {
      tt_remove_tree(root);
      free(root);
      root = NULL;
    }
    if (server_started) {
      check_equal(chttp_server_stop(&server, AGENT_HTTP_TEST_TIMEOUT_MS), SALTS_OK);
      server_started = 0;
    }
    if (server.impl) check_equal(chttp_server_destroy(&server), SALTS_OK);
  }

  const struct {
    int status;
    const char *field;
    const char *hint;
  } cases[] = {{401, "http_status=401", "OPENAI_API_KEY"},
               {403, "http_status=403", "credential permissions"},
               {404, "http_status=404", "OPENAI_ENDPOINT_PATH"},
               {429, "http_status=429", "rate limit or quota"},
               {503, "http_status=503", "server error"}};
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    it("reports HTTP %d without leaking provider text or retrying", cases[i].status) {
      char text[2048];
      atomic_store(&reply.status, cases[i].status);
      check_equal(turbo_cli_run(&options, input, output, diagnostics, &config, NULL), 1);
      check_equal(atomic_load(&reply.requests), 1);
      cli_read_diagnostic(diagnostics, text, sizeof(text));
      check_contains(text, "Agent run failed or was interrupted.");
      check_contains(text, "phase=transport");
      check_contains(text, cases[i].field);
      check_contains(text, cases[i].hint);
      check_null(strstr(text, "TEST_SECRET_NEVER_PRINT"));
      check_null(strchr(text, '\033'));
      rewind(output);
      check_equal(fgetc(output), EOF);
    }
  }

  it("distinguishes malformed model responses from transport failures") {
    char text[2048];
    atomic_store(&reply.status, 200);
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &config, NULL), 1);
    cli_read_diagnostic(diagnostics, text, sizeof(text));
    check_contains(text, "phase=parse_response");
    check_contains(text, "http_status=200");
    check_contains(text, "could not be parsed");
    check_null(strstr(text, "TEST_SECRET_NEVER_PRINT"));
    check_null(strchr(text, '\033'));
  }

  it("reports a local endpoint error without submitting an HTTP request") {
    char text[2048];
    config.base_url = "file:///invalid-model-endpoint";
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &config, NULL), 1);
    cli_read_diagnostic(diagnostics, text, sizeof(text));
    check_contains(text, "phase=transport");
    check_contains(text, "http_status=0");
    check_contains(text, "configuration and connectivity");
    check_equal(atomic_load(&reply.requests), 0);
  }

  it("keeps legacy callback failure bodies out of terminal output") {
    char text[2048];
    config.transport_fn = cli_private_failure;
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &config, NULL), 1);
    cli_read_diagnostic(diagnostics, text, sizeof(text));
    check_contains(text, "phase=transport");
    check_contains(text, "http_status=0");
    check_null(strstr(text, "TEST_SECRET_NEVER_PRINT"));
    check_null(strchr(text, '\033'));
    check_equal(atomic_load(&reply.requests), 0);
  }

  it("preserves an earlier answer when the next interactive turn fails") {
    char text[2048];
    options.prompt = NULL;
    atomic_store(&reply.success_first, 1);
    fputs("first\nsecond\n", input);
    rewind(input);
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &config, NULL), 1);
    check_equal(atomic_load(&reply.requests), 2);
    cli_read_diagnostic(output, text, sizeof(text));
    check_equal(text, "completed\n");
    cli_read_diagnostic(diagnostics, text, sizeof(text));
    check_contains(text, "http_status=401");
    check_null(strstr(text, "TEST_SECRET_NEVER_PRINT"));
  }
}
