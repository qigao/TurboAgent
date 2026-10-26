#include <turbo_provider.h>

int main(void) {
  turbo_provider_request_t request = {0};
  json_value_t *input = json_create_array();
  json_value_t *tools = json_create_array();
  char *wire = NULL;
  int rc;
  request.format = TURBO_PROVIDER_CHAT;
  request.model = "installed-no-network";
  request.input = input;
  request.tools = tools;
  rc = (!input || !tools) ? -1 : turbo_provider_build_request(&request, &wire);
  json_serialize_free(wire);
  json_free(input);
  json_free(tools);
  return rc != 0;
}
