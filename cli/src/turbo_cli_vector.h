#ifndef TURBO_CLI_VECTOR_H
#define TURBO_CLI_VECTOR_H
#include "turbo_cli.h"

typedef struct turbo_cli_vector_s turbo_cli_vector_t;
/* Private, single input-thread owner. Options are copied on open. HTTP work is
 * sequential and drained before close. No SDK public ABI changes. */
int turbo_cli_vector_validate(const turbo_cli_options_t *options);
int turbo_cli_vector_open(const turbo_cli_options_t *options, const char *index_dir,
    const atomic_int *interrupted, turbo_cli_vector_t **out);
int turbo_cli_vector_prepare(turbo_cli_vector_t *vector, const char *query, json_value_t *state);
int turbo_cli_vector_close(turbo_cli_vector_t *vector);
#endif
