#include <stdint.h>

#include "turbo_wasm3_guest_abi.h"

TURBONET_IMPORT("redis_client_open")
extern int32_t turbonet_redis_client_open(const char *host, uint32_t host_len,
                                          uint32_t port,
                                          uint32_t *out_client);

TURBONET_IMPORT("redis_client_close")
extern int32_t turbonet_redis_client_close(uint32_t client_handle);

TURBONET_IMPORT("redis_command")
extern int32_t turbonet_redis_command(uint32_t client_handle, uint32_t argc,
                                      const char *const *argv,
                                      const uint32_t *argv_lens,
                                      uint32_t *out_reply);

TURBONET_IMPORT("redis_reply_close")
extern int32_t turbonet_redis_reply_close(uint32_t reply_handle);

TURBONET_IMPORT("redis_reply_type")
extern int32_t turbonet_redis_reply_type(uint32_t reply_handle,
                                         int32_t *out_type);

TURBONET_IMPORT("redis_reply_i64")
extern int32_t turbonet_redis_reply_i64(uint32_t reply_handle,
                                        int64_t *out_value);

TURBONET_IMPORT("redis_reply_text")
extern int32_t turbonet_redis_reply_text(uint32_t reply_handle, char *buffer,
                                         uint32_t buffer_size,
                                         uint32_t *out_written);

TURBONET_IMPORT("redis_reply_array_len")
extern int32_t turbonet_redis_reply_array_len(uint32_t reply_handle,
                                              uint32_t *out_len);

TURBONET_IMPORT("redis_reply_array_at")
extern int32_t turbonet_redis_reply_array_at(uint32_t reply_handle,
                                             uint32_t index,
                                             uint32_t *out_child_reply);

enum {
  TURBO_WASM3_REDIS_REPLY_STRING = 0,
  TURBO_WASM3_REDIS_REPLY_INTEGER = 2,
  TURBO_WASM3_REDIS_REPLY_BULK_STRING = 3,
  TURBO_WASM3_REDIS_REPLY_ARRAY = 4,
  TURBO_WASM3_REDIS_REPLY_NULL = 5,
};

static uint32_t cstrlen(const char *s) {
  uint32_t len = 0;
  while (s[len] != '\0') {
    ++len;
  }
  return len;
}

static int cstreq(const char *a, const char *b) {
  uint32_t i = 0;
  while (a[i] != '\0' && b[i] != '\0') {
    if (a[i] != b[i]) {
      return 0;
    }
    ++i;
  }
  return a[i] == b[i];
}

static int32_t run_command(uint32_t client, uint32_t argc, const char *const *argv,
                           const uint32_t *argv_lens, uint32_t *out_reply) {
  return turbonet_redis_command(client, argc, argv, argv_lens, out_reply);
}

TURBONET_EXPORT("run_redis_demo")
int32_t run_redis_demo(uint32_t port) {
  static const char host[] = "127.0.0.1";
  static const char ping0[] = "PING";
  static const char incr0[] = "INCR";
  static const char incr1[] = "counter";
  static const char lrange0[] = "LRANGE";
  static const char lrange1[] = "letters";
  static const char lrange2[] = "0";
  static const char lrange3[] = "-1";
  static const char get0[] = "GET";
  static const char get1[] = "missing";
  static const char *const ping_argv[] = {ping0};
  static const uint32_t ping_lens[] = {4};
  static const char *const incr_argv[] = {incr0, incr1};
  static const uint32_t incr_lens[] = {4, 7};
  static const char *const lrange_argv[] = {lrange0, lrange1, lrange2, lrange3};
  static const uint32_t lrange_lens[] = {6, 7, 1, 2};
  static const char *const get_argv[] = {get0, get1};
  static const uint32_t get_lens[] = {3, 7};
  uint32_t client = 0;
  uint32_t reply = 0;
  uint32_t child = 0;
  uint32_t written = 0;
  uint32_t array_len = 0;
  int32_t type = -1;
  int64_t value = 0;
  char text[16];
  int32_t rc = 0;

  rc = turbonet_redis_client_open(host, cstrlen(host), port, &client);
  if (rc != 0) {
    return 10 + rc;
  }

  rc = run_command(client, 1, ping_argv, ping_lens, &reply);
  if (rc != 0) {
    return 20 + rc;
  }
  rc = turbonet_redis_reply_type(reply, &type);
  if (rc != 0 || type != TURBO_WASM3_REDIS_REPLY_STRING) {
    return 30 + rc;
  }
  rc = turbonet_redis_reply_text(reply, text, sizeof(text), &written);
  if (written < sizeof(text)) {
    text[written] = '\0';
  }
  if (rc != 0 || written != 4 || !cstreq(text, "PONG")) {
    return 40 + rc;
  }
  rc = turbonet_redis_reply_close(reply);
  if (rc != 0) {
    return 50 + rc;
  }

  rc = run_command(client, 2, incr_argv, incr_lens, &reply);
  if (rc != 0) {
    return 60 + rc;
  }
  rc = turbonet_redis_reply_type(reply, &type);
  if (rc != 0 || type != TURBO_WASM3_REDIS_REPLY_INTEGER) {
    return 70 + rc;
  }
  rc = turbonet_redis_reply_i64(reply, &value);
  if (rc != 0 || value != 41) {
    return 80 + rc;
  }
  rc = turbonet_redis_reply_close(reply);
  if (rc != 0) {
    return 90 + rc;
  }

  rc = run_command(client, 4, lrange_argv, lrange_lens, &reply);
  if (rc != 0) {
    return 100 + rc;
  }
  rc = turbonet_redis_reply_type(reply, &type);
  if (rc != 0 || type != TURBO_WASM3_REDIS_REPLY_ARRAY) {
    return 110 + rc;
  }
  rc = turbonet_redis_reply_array_len(reply, &array_len);
  if (rc != 0 || array_len != 2) {
    return 120 + rc;
  }
  rc = turbonet_redis_reply_array_at(reply, 1, &child);
  if (rc != 0) {
    return 130 + rc;
  }
  rc = turbonet_redis_reply_type(child, &type);
  if (rc != 0 || type != TURBO_WASM3_REDIS_REPLY_BULK_STRING) {
    return 140 + rc;
  }
  rc = turbonet_redis_reply_text(child, text, sizeof(text), &written);
  if (written < sizeof(text)) {
    text[written] = '\0';
  }
  if (rc != 0 || written != 2 || !cstreq(text, "bb")) {
    return 150 + rc;
  }
  rc = turbonet_redis_reply_close(child);
  if (rc != 0) {
    return 160 + rc;
  }
  rc = turbonet_redis_reply_close(reply);
  if (rc != 0) {
    return 170 + rc;
  }

  rc = run_command(client, 2, get_argv, get_lens, &reply);
  if (rc != 0) {
    return 180 + rc;
  }
  rc = turbonet_redis_reply_type(reply, &type);
  if (rc != 0 || type != TURBO_WASM3_REDIS_REPLY_NULL) {
    return 190 + rc;
  }
  rc = turbonet_redis_reply_close(reply);
  if (rc != 0) {
    return 200 + rc;
  }

  rc = turbonet_redis_client_close(client);
  if (rc != 0) {
    return 210 + rc;
  }

  return 23;
}
