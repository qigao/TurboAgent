/* Formal subprocess fixture: simulate abrupt termination after the CLI has
 * committed its active marker, without running exit handlers or store close. */
#include "turbo_cli.h"
#include <stdlib.h>
#include <string.h>

static int fixture_model(const char *request, char **out, void *data) {
  const char response[] = "{\"id\":\"fixture\",\"output\":[{\"type\":\"message\","
      "\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"saved_answer\"}]}]}";
  (void)request;
  if (!strcmp((const char *)data, "crash")) _Exit(23);
  *out = malloc(sizeof(response));
  if (!*out) return -1;
  memcpy(*out, response, sizeof(response));
  return 0;
}

int main(int argc, char **argv) {
  turbo_cli_options_t options;
  turbo_agent_config_t config = {0};
  if (argc != 4 || (strcmp(argv[3], "crash") && strcmp(argv[3], "complete"))) return 2;
  options = (turbo_cli_options_t){argv[1], "initial_task", NULL, 1, argv[2], 0};
  config.model = "test-model"; config.transport_fn = fixture_model;
  config.transport_user_data = argv[3];
  return turbo_cli_run(&options, stdin, stdout, stderr, &config, NULL);
}
