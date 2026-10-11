#ifndef TURBO_AGENT_HTTP_OWNER_INTERNAL_H
#define TURBO_AGENT_HTTP_OWNER_INTERNAL_H

#include "turbo_runtime_control.h"
#include <http_client/http.h>

typedef struct turbo_agent_http_owner_s turbo_agent_http_owner_t;

int turbo_agent_http_owner_create(const chttp_client_config *config,
                                  turbo_agent_http_owner_t **out_owner);
void turbo_agent_http_owner_destroy(turbo_agent_http_owner_t *owner);

/* Buffered model POST only (body_source/body_sink must be NULL), sequential on
 * the issuing worker. out_response starts empty and receives
 * owned storage, including error responses. out_cancelled is set only after a
 * token cancellation AND successful drain/destroy; cleanup errors take priority. */
int turbo_agent_http_owner_post(turbo_agent_http_owner_t *owner, const chttp_options *options,
                                const turbo_cancel_token_t *token, chttp_response *out_response,
                                chttp_error *out_error, int *out_cancelled);

#endif
