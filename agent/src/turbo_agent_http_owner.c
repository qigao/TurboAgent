#include "turbo_agent_http_owner_internal.h"
#include <salts/clock.h>
#include <stdlib.h>
#include <string.h>
#include <tlog.h>

enum { HTTP_OWNER_POLL_MS = 50, HTTP_OWNER_DRAIN_MS = 5000 };

struct turbo_agent_http_owner_s {
  chttp_client_config config;
  chttp_async_client client;
  chttp_response response;
  chttp_error error;
  int done;
  int close_status;
};

static void http_owner_complete(void *user, chttp_request request,
                                const chttp_response_view *response, const chttp_error *error) {
  turbo_agent_http_owner_t *owner = user;
  (void)request;
  owner->done = 1;
  if (error) owner->error = *error;
  if (owner->error.status != SALTS_OK) return;
  if (!response || (response->body_size && !response->body)) {
    owner->error = (chttp_error){.status = SALTS_EPROTO, .stage = "response_copy"};
    return;
  }
  /* Only the body and status are consumed by model transport. Other response
   * fields remain zero; this is not a copy of the callback's borrowed views. */
  owner->response.status_code = response->status_code;
  if (response->body_size > owner->config.max_response_body_bytes) {
    owner->error = (chttp_error){.status = SALTS_EMSGSIZE, .stage = "response_copy"};
    return;
  }
  if (response->body_size) {
    owner->response.body = malloc(response->body_size);
    if (!owner->response.body) {
      owner->error = (chttp_error){.status = SALTS_ENOMEM, .stage = "response_copy"};
      return;
    }
    memcpy(owner->response.body, response->body, response->body_size);
    owner->response.body_size = response->body_size;
  }
}

int turbo_agent_http_owner_create(const chttp_client_config *config,
                                  turbo_agent_http_owner_t **out_owner) {
  turbo_agent_http_owner_t *owner;
  if (!config || !out_owner) return SALTS_EINVAL;
  *out_owner = NULL;
  owner = calloc(1, sizeof(*owner));
  if (!owner) return SALTS_ENOMEM;
  owner->config = *config;
  *out_owner = owner;
  return SALTS_OK;
}

void turbo_agent_http_owner_destroy(turbo_agent_http_owner_t *owner) {
  if (!owner) return;
  if (owner->client.impl) {
    /* An abnormal backend drain failure cannot revoke SDK borrows. Never
     * poll this owner on the control thread or free its callback storage. */
    TLOG_ERRORF("Agent HTTP owner retained after cleanup failure, status={}", owner->close_status);
    return;
  }
  chttp_response_destroy(&owner->response);
  free(owner);
}

int turbo_agent_http_owner_post(turbo_agent_http_owner_t *owner, const chttp_options *options,
                                const turbo_cancel_token_t *token, chttp_response *out_response,
                                chttp_error *out_error, int *out_cancelled) {
  chttp_request request = {0};
  chttp_request_options submit;
  uint64_t started_ms;
  int status, cancelled = 0, admitted = 0;
  if (!owner || !options || !out_response || !out_error || !out_cancelled) return SALTS_EINVAL;
  *out_cancelled = 0;
  *out_error = (chttp_error){0};
  if (owner->client.impl || owner->close_status != SALTS_OK) {
    *out_error = (chttp_error){.status = SALTS_ESHUTDOWN, .stage = "client_cleanup_failed"};
    return SALTS_ESHUTDOWN;
  }
  status = token ? turbo_cancel_token_check(token) : SALTS_OK;
  if (status != SALTS_OK) {
    *out_cancelled = 1;
    *out_error = (chttp_error){.status = status, .stage = "cancel_before_submit"};
    return status;
  }
  owner->done = 0;
  owner->error = (chttp_error){0};
  started_ms = cmeta_monotonic_ms();
  status = chttp_async_client_init(&owner->client, &owner->config);
  if (status != SALTS_OK) {
    *out_error = (chttp_error){.status = status, .stage = "client_init"};
    return status;
  }
  submit = (chttp_request_options){.connection_uri = options->connection_uri,
                                   .authority = options->authority,
                                   .target = options->target,
                                   .method = CHTTP_METHOD_POST,
                                   .headers = options->headers,
                                   .header_count = options->header_count,
                                   .body = options->body,
                                   .body_size = options->body_size,
                                   .on_complete = http_owner_complete,
                                   .user = owner,
                                   .tls = options->tls,
                                   .protocol = options->protocol};
  status = token ? turbo_cancel_token_check(token) : SALTS_OK;
  if (status == SALTS_OK) {
    status = chttp_async_client_submit(&owner->client, &submit, &request);
    admitted = status == SALTS_OK;
    if (!admitted) *out_error = (chttp_error){.status = status, .stage = "request_submit"};
  } else {
    cancelled = 1;
    *out_error = (chttp_error){.status = status, .stage = "cancel_before_submit"};
  }
  while (status == SALTS_OK) {
    uint64_t elapsed;
    uint32_t wait_ms = HTTP_OWNER_POLL_MS;
    size_t completions = 0;
    status = token ? turbo_cancel_token_check(token) : SALTS_OK;
    if (status != SALTS_OK) {
      cancelled = 1;
      *out_error = (chttp_error){.status = status, .stage = "request_cancel"};
      break;
    }
    if (owner->done) {
      *out_error = owner->error;
      status = owner->error.status;
      break;
    }
    elapsed = cmeta_monotonic_ms() - started_ms;
    if (options->timeout_ms != UINT32_MAX) {
      if (elapsed >= options->timeout_ms) {
        status = SALTS_ETIMEDOUT;
        *out_error = (chttp_error){.status = status, .stage = "request_timeout"};
        break;
      }
      if (options->timeout_ms - elapsed < wait_ms)
        wait_ms = (uint32_t)(options->timeout_ms - elapsed);
    }
    status = chttp_async_client_poll(&owner->client, wait_ms, &completions);
    if (status != SALTS_OK) *out_error = (chttp_error){.status = status, .stage = "request_poll"};
  }
  if (admitted && !owner->done) {
    const int cancel_status = chttp_async_request_cancel(&owner->client, request);
    if (cancel_status != SALTS_OK && cancel_status != SALTS_EALREADY &&
        cancel_status != SALTS_ENOENT) {
      status = cancel_status;
      cancelled = 0;
      *out_error = (chttp_error){.status = status, .stage = "request_cancel"};
    }
  }
  owner->close_status = chttp_async_client_stop(&owner->client, HTTP_OWNER_DRAIN_MS);
  if (owner->close_status != SALTS_OK) {
    status = owner->close_status;
    cancelled = 0;
    *out_error = (chttp_error){.status = status, .stage = "client_stop"};
  } else {
    owner->close_status = chttp_async_client_destroy(&owner->client);
    if (owner->close_status != SALTS_OK) {
      status = owner->close_status;
      cancelled = 0;
      *out_error = (chttp_error){.status = status, .stage = "client_destroy"};
    }
  }
  *out_response = owner->response;
  owner->response = (chttp_response){0};
  *out_cancelled = cancelled;
  return status;
}
