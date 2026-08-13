#include "tinytest.h"
#include "turbo_langchain.h"

#include <stdlib.h>
#include <string.h>

static char *langchain_facade_strdup(const char *text) {
  char *copy;
  size_t len;

  check_not_null(text);
  len = strlen(text);
  copy = (char *)malloc(len + 1);
  check_not_null(copy);
  memcpy(copy, text, len + 1);
  return copy;
}

static int langchain_facade_transport(const char *request_json, char **out_response_json,
                                      void *user_data) {
  const char *response =
      "{\"id\":\"resp_facade\",\"output\":[{\"type\":\"message\",\"role\":\"assistant\","
      "\"content\":[{\"type\":\"output_text\",\"text\":\"facade ok\"}]}]}";

  (void)request_json;
  (void)user_data;
  check_not_null(out_response_json);
  *out_response_json = langchain_facade_strdup(response);
  return 0;
}

spec("turbo langchain facade") {
  it("should run prompt and chain through langchain-named runnable APIs") {
    turbo_langchain_chain_t *chain = turbo_langchain_chain_create("facade-chain");
    turbo_langchain_runnable_t *runnable = NULL;
    turbo_langchain_event_log_t *log = turbo_langchain_event_log_create();
    json_value_t *state = turbo_langchain_chain_state_create();
    json_value_t *input;
    json_value_t *out_state = NULL;

    check_not_null(chain);
    check_not_null(log);
    check_not_null(state);
    input = (json_value_t *)turbo_json_object_get(state, "input");
    check_not_null(input);
    check_int_eq(turbo_runtime_json_object_set(
                     input, "task", turbo_json_create_string("ship")),
                 TURBO_RUNTIME_JSON_OK);
    check_int_eq(turbo_langchain_chain_add_prompt(chain, "prompt", "user", "Please {{task}}."),
                 TURBO_CHAIN_OK);

    runnable = turbo_langchain_runnable_from_chain(chain);
    check_not_null(runnable);
    check_int_eq(turbo_langchain_runnable_log(runnable, state, log, &out_state), 0);
    check_not_null(out_state);
    check_size_eq(turbo_runtime_json_value_size(
                      turbo_json_object_get(out_state, "messages")),
                  1);
    check_size_eq(turbo_runtime_json_value_size(turbo_event_log_events_json_value(log)), 2);

    turbo_runtime_json_destroy(out_state);
    turbo_runtime_json_destroy(state);
    turbo_langchain_runnable_destroy(runnable);
    turbo_langchain_event_log_destroy(log);
    turbo_langchain_chain_destroy(chain);
  }

  it("should invoke an agent and memory through langchain-named APIs") {
    turbo_agent_session_config_t config = {0};
    turbo_agent_config_t agent_config = {0};
    turbo_langchain_agent_t *agent;
    turbo_agent_memory_store_t memory_store = turbo_langchain_memory_store_create();
    json_value_t *summary = NULL;
    json_value_t *records = NULL;
    char *text = NULL;

    agent_config.model = "gpt-5.4";
    agent_config.transport_fn = langchain_facade_transport;
    config.runtime_store = turbo_agent_runtime_store_memory_create();
    config.memory_store = memory_store;
    config.agent_config = agent_config;
    config.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_LOOP;

    agent = turbo_langchain_agent_create(&config);
    check_not_null(agent);
    check_int_eq(turbo_langchain_agent_memory_put_context(
                     agent, "project/facade", "notes", "project", "/tmp/facade.md", "remember"),
                 0);
    check_int_eq(turbo_langchain_agent_memory_query(
                     agent, "project", "context", "notes", "remember", &records),
                 0);
    check_size_eq(turbo_json_array_size(records), 1);

    check_int_eq(turbo_langchain_agent_invoke_text(agent, "hello", NULL, &text, &summary), 0);
    check_str_eq(text, "facade ok");
    check_str_eq(turbo_json_get_string(summary, "status"), "completed");

    free(text);
    turbo_free_json(&records);
    turbo_free_json(&summary);
    turbo_langchain_agent_destroy(agent);
  }
}
