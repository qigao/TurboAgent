/* Formal subprocess fixture. The readiness byte is emitted only after the
 * actual CLI vector owner has opened the index and completed any import. */
#include "turbo_cli_vector.h"
#include <platform.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
  turbo_cli_vector_t *vector = NULL;
  if (argc != 5 || (strcmp(argv[4], "hold") && strcmp(argv[4], "import"))) return 2;
  turbo_cli_options_t options = {.vector_db = argv[1], .embedding_url = argv[2],
      .embedding_model = "fixture-embedding"};
  int rc = turbo_cli_vector_open(&options, !strcmp(argv[4], "import") ? argv[3] : NULL,
      NULL, &vector);
  if (rc != SALTS_OK) return rc == SALTS_EBUSY ? 24 : 1;
  if (fputc('R', stdout) == EOF || fflush(stdout) != 0) {
    turbo_cli_vector_close(vector);
    return 1;
  }
  int command = fgetc(stdin);
  if (command == 'X') _Exit(23); /* Deliberately skip store and lock cleanup. */
  rc = turbo_cli_vector_close(vector);
  return command == 'C' && rc == SALTS_OK ? 0 : 1;
}
