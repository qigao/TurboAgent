#include "CoroNet.h"
#include "CoroNet/turbo_coro.h"
#include "tinytest.h"
#include "turbo_fs.h"
#include "turbo_wasm3.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <io.h>
#include <process.h>
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#define TURBO_WASM3_DUP _dup
#define TURBO_WASM3_DUP2 _dup2
#define TURBO_WASM3_CLOSE _close
#define TURBO_WASM3_FILENO _fileno
#define TURBO_WASM3_GETPID _getpid
#define CLOSESOCK closesocket
typedef SOCKET native_sock_t;
typedef HANDLE native_thread_t;
typedef unsigned(__stdcall *native_thread_entry_t)(void *);
#define THREAD_RET unsigned __stdcall
#define THREAD_RETURN(value) return (value)
#define THREAD_CALL __stdcall
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>
#define TURBO_WASM3_DUP dup
#define TURBO_WASM3_DUP2 dup2
#define TURBO_WASM3_CLOSE close
#define TURBO_WASM3_FILENO fileno
#define TURBO_WASM3_GETPID getpid
#define CLOSESOCK close
typedef int native_sock_t;
typedef pthread_t native_thread_t;
typedef void *(*native_thread_entry_t)(void *);
#define THREAD_RET void *
#define THREAD_RETURN(value) return (value)
#define THREAD_CALL
#endif

#ifndef WASM3_TEST_WASM_PATH
#error "WASM3_TEST_WASM_PATH must be defined by CMake"
#endif

#ifdef WASM3_DB_TEST_WASM_PATH
#define WASM3_DB_GUEST_AVAILABLE 1
#endif

#ifdef WASM3_PARSER_TEST_WASM_PATH
#define WASM3_PARSER_GUEST_AVAILABLE 1
#endif

#ifdef WASM3_REDIS_TEST_WASM_PATH
#define WASM3_REDIS_GUEST_AVAILABLE 1
#endif

static char g_fixture_a[TURBO_FS_MAX_PATH];
static char g_fixture_b[TURBO_FS_MAX_PATH];
static unsigned long g_fixture_counter = 0;

typedef struct wasm3_socket_bridge_state_s {
  coro_context_t *ctx;
  m3_wasi_context_t *wasi;
  turbo_wasm3_socket_registry_t *registry;
  int finished;
  int stage;
  int task_rc;
  int lookup_matches;
  int unregister_rc;
  uint32_t wasi_fd;
  uint32_t sent_len;
  uint32_t recv_len;
  uint16_t send_err;
  uint16_t recv_err;
  uint16_t recv_after_unregister_err;
  uint16_t shutdown_err;
  uint16_t recv_flags;
  char recv_buf[16];
} wasm3_socket_bridge_state_t;

typedef struct wasm3_http_mock_state_s {
  int ready;
  int failed;
  int fail_step;
  int fail_code;
  int port;
  const char *expected_path;
  const char *expected_header_a;
  const char *expected_header_b;
  const char *response_body_a;
  const char *response_body_b;
  int response_chunked;
  int response_sse;
  char request[4096];
} wasm3_http_mock_state_t;

typedef struct wasm3_proxy_mock_state_s {
  int ready;
  int failed;
  int fail_step;
  int fail_code;
  int port;
  int auth_required;
  int saw_http_request;
  const char *expected_path;
  const char *expected_header_a;
  const char *expected_header_b;
  const char *response_body_a;
  const char *response_body_b;
  int response_chunked;
  int response_sse;
  char request[4096];
} wasm3_proxy_mock_state_t;

typedef struct wasm3_redis_mock_state_s {
  int ready;
  int failed;
  int fail_step;
  int fail_code;
  int port;
  char requests[4][128];
} wasm3_redis_mock_state_t;

static void ensure_native_sockets_ready(void) {
#ifdef _WIN32
  static int initialized = 0;
  static WSADATA wsa_data;

  if (!initialized) {
    check_int_eq(WSAStartup(MAKEWORD(2, 2), &wsa_data), 0);
    initialized = 1;
  }
#endif
}

static int native_thread_create(native_thread_t *thread,
                                native_thread_entry_t entry, void *arg) {
#ifdef _WIN32
  uintptr_t h = _beginthreadex(NULL, 0, entry, arg, 0, NULL);
  if (h == 0) {
    return -1;
  }
  *thread = (HANDLE)h;
  return 0;
#else
  return pthread_create(thread, NULL, entry, arg);
#endif
}

static void native_thread_join(native_thread_t *thread) {
#ifdef _WIN32
  WaitForSingleObject(*thread, INFINITE);
  CloseHandle(*thread);
  *thread = NULL;
#else
  pthread_join(*thread, NULL);
#endif
}

static void native_sleep_ms(unsigned ms) {
#ifdef _WIN32
  Sleep(ms);
#else
  usleep(ms * 1000);
#endif
}

static int recv_exact_sock(native_sock_t sock, unsigned char *buf, size_t want) {
  size_t filled = 0;

  while (filled < want) {
    int n = recv(sock, (char *)buf + filled, (int)(want - filled), 0);
    if (n <= 0) {
      return -1;
    }
    filled += (size_t)n;
  }

  return 0;
}

static int send_all_sock(native_sock_t sock, const unsigned char *buf, size_t len) {
  size_t sent = 0;

  while (sent < len) {
    int n = send(sock, (const char *)buf + sent, (int)(len - sent), 0);
    if (n <= 0) {
      return -1;
    }
    sent += (size_t)n;
  }

  return 0;
}

static int recv_http_headers(native_sock_t sock, char *buf, size_t cap) {
  size_t len = 0;

  while (len + 1 < cap) {
    int n = recv(sock, buf + len, (int)(cap - len - 1), 0);
    if (n <= 0) {
      return -1;
    }
    len += (size_t)n;
    buf[len] = '\0';
    if (strstr(buf, "\r\n\r\n") != NULL) {
      return 0;
    }
  }

  return -1;
}

static int recv_line_sock(native_sock_t sock, char *buf, size_t cap,
                          size_t *out_len) {
  size_t len = 0;

  if (!buf || cap < 3 || !out_len) {
    return -1;
  }

  while (len + 1 < cap) {
    int n = recv(sock, buf + len, 1, 0);
    if (n <= 0) {
      return -1;
    }
    len += (size_t)n;
    if (len >= 2 && buf[len - 2] == '\r' && buf[len - 1] == '\n') {
      buf[len] = '\0';
      *out_len = len;
      return 0;
    }
  }

  return -1;
}

static int recv_redis_command(native_sock_t sock, char *buf, size_t cap) {
  char line[64];
  size_t line_len = 0;
  size_t total = 0;
  long argc = 0;
  long arg_len = 0;
  int i;

  if (!buf || cap < 8) {
    return -1;
  }

  if (recv_line_sock(sock, line, sizeof(line), &line_len) != 0 || line[0] != '*') {
    return -1;
  }
  argc = strtol(line + 1, NULL, 10);
  if (argc <= 0) {
    return -1;
  }

  if (total + line_len >= cap) {
    return -1;
  }
  memcpy(buf + total, line, line_len);
  total += line_len;

  for (i = 0; i < argc; ++i) {
    if (recv_line_sock(sock, line, sizeof(line), &line_len) != 0 ||
        line[0] != '$') {
      return -1;
    }
    arg_len = strtol(line + 1, NULL, 10);
    if (arg_len < 0) {
      return -1;
    }

    if (total + line_len + (size_t)arg_len + 2 >= cap) {
      return -1;
    }
    memcpy(buf + total, line, line_len);
    total += line_len;

    if (recv_exact_sock(sock, (unsigned char *)(buf + total),
                        (size_t)arg_len + 2) != 0) {
      return -1;
    }
    total += (size_t)arg_len + 2;
  }

  buf[total] = '\0';
  return 0;
}

static THREAD_RET THREAD_CALL wasm3_http_mock_thread(void *arg) {
  wasm3_http_mock_state_t *state = (wasm3_http_mock_state_t *)arg;
  native_sock_t listener;
  native_sock_t client;
  struct sockaddr_in addr;
  struct sockaddr_in bound_addr;
#ifdef _WIN32
  int bound_len;
#else
  socklen_t bound_len;
#endif
  const char *body_a;
  const char *body_b;
  char header[512];
  int rc = 0;

  listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listener == (native_sock_t)-1) {
    state->fail_step = 1;
#ifdef _WIN32
    state->fail_code = WSAGetLastError();
#else
    state->fail_code = errno;
#endif
    state->failed = 1;
    THREAD_RETURN(0);
  }

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons((uint16_t)state->port);

  if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
      listen(listener, 1) != 0) {
    state->fail_step = 2;
#ifdef _WIN32
    state->fail_code = WSAGetLastError();
#else
    state->fail_code = errno;
#endif
    CLOSESOCK(listener);
    state->failed = 1;
    THREAD_RETURN(0);
  }

  bound_len = (int)sizeof(bound_addr);
  if (getsockname(listener, (struct sockaddr *)&bound_addr, &bound_len) != 0) {
    state->fail_step = 3;
#ifdef _WIN32
    state->fail_code = WSAGetLastError();
#else
    state->fail_code = errno;
#endif
    CLOSESOCK(listener);
    state->failed = 1;
    THREAD_RETURN(0);
  }

  state->port = ntohs(bound_addr.sin_port);
  state->ready = 1;
  client = accept(listener, NULL, NULL);
  CLOSESOCK(listener);
  if (client == (native_sock_t)-1) {
    state->fail_step = 4;
    state->failed = 1;
    THREAD_RETURN(0);
  }

  if (recv_http_headers(client, state->request, sizeof(state->request)) != 0) {
    state->fail_step = 5;
    state->failed = 1;
    CLOSESOCK(client);
    THREAD_RETURN(0);
  }

  if (state->expected_path && strstr(state->request, state->expected_path) == NULL) {
    state->fail_step = 6;
    state->failed = 1;
  }
  if (state->expected_header_a &&
      strstr(state->request, state->expected_header_a) == NULL) {
    state->fail_step = state->fail_step ? state->fail_step : 7;
    state->failed = 1;
  }
  if (state->expected_header_b &&
      strstr(state->request, state->expected_header_b) == NULL) {
    state->fail_step = state->fail_step ? state->fail_step : 8;
    state->failed = 1;
  }

  body_a = state->response_body_a ? state->response_body_a : "";
  body_b = state->response_body_b ? state->response_body_b : "";

  if (state->response_chunked) {
    static const char prelude[] =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Content-Type: text/plain\r\n"
        "Connection: close\r\n"
        "\r\n";
    rc = send_all_sock(client, (const unsigned char *)prelude, strlen(prelude));
    if (rc == 0) {
      snprintf(header, sizeof(header), "%x\r\n", (unsigned)strlen(body_a));
      rc = send_all_sock(client, (const unsigned char *)header, strlen(header));
    }
    if (rc == 0) {
      rc = send_all_sock(client, (const unsigned char *)body_a, strlen(body_a));
    }
    if (rc == 0) {
      rc = send_all_sock(client, (const unsigned char *)"\r\n", 2);
    }
    if (rc == 0) {
      native_sleep_ms(20);
      snprintf(header, sizeof(header), "%x\r\n", (unsigned)strlen(body_b));
      rc = send_all_sock(client, (const unsigned char *)header, strlen(header));
    }
    if (rc == 0) {
      rc = send_all_sock(client, (const unsigned char *)body_b, strlen(body_b));
    }
    if (rc == 0) {
      rc = send_all_sock(client, (const unsigned char *)"\r\n0\r\n\r\n", 7);
    }
  } else if (state->response_sse) {
    static const char prelude[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n"
        "\r\n";
    rc = send_all_sock(client, (const unsigned char *)prelude, strlen(prelude));
    if (rc == 0) {
      rc = send_all_sock(client, (const unsigned char *)body_a, strlen(body_a));
    }
    if (rc == 0) {
      native_sleep_ms(20);
      rc = send_all_sock(client, (const unsigned char *)body_b, strlen(body_b));
    }
  } else {
    snprintf(header, sizeof(header),
             "HTTP/1.1 200 OK\r\n"
             "Content-Length: %u\r\n"
             "Content-Type: text/plain\r\n"
             "Connection: close\r\n"
             "\r\n",
             (unsigned)strlen(body_a));
    rc = send_all_sock(client, (const unsigned char *)header, strlen(header));
    if (rc == 0) {
      rc = send_all_sock(client, (const unsigned char *)body_a, strlen(body_a));
    }
  }

  if (rc != 0) {
    state->fail_step = 9;
    state->failed = 1;
  }

  CLOSESOCK(client);
  THREAD_RETURN(0);
}

static THREAD_RET THREAD_CALL wasm3_redis_mock_thread(void *arg) {
  static const char *expected_requests[] = {
      "*1\r\n$4\r\nPING\r\n",
      "*2\r\n$4\r\nINCR\r\n$7\r\ncounter\r\n",
      "*4\r\n$6\r\nLRANGE\r\n$7\r\nletters\r\n$1\r\n0\r\n$2\r\n-1\r\n",
      "*2\r\n$3\r\nGET\r\n$7\r\nmissing\r\n",
  };
  static const char *responses[] = {
      "+PONG\r\n",
      ":41\r\n",
      "*2\r\n$1\r\na\r\n$2\r\nbb\r\n",
      "$-1\r\n",
  };
  wasm3_redis_mock_state_t *state = (wasm3_redis_mock_state_t *)arg;
  native_sock_t listener;
  native_sock_t client;
  struct sockaddr_in addr;
  struct sockaddr_in bound_addr;
#ifdef _WIN32
  int bound_len;
#else
  socklen_t bound_len;
#endif
  int i;

  listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listener == (native_sock_t)-1) {
    state->fail_step = 1;
    state->failed = 1;
    THREAD_RETURN(0);
  }

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons((uint16_t)state->port);

  if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
      listen(listener, 1) != 0) {
    state->fail_step = 2;
    CLOSESOCK(listener);
    state->failed = 1;
    THREAD_RETURN(0);
  }

  bound_len = (int)sizeof(bound_addr);
  if (getsockname(listener, (struct sockaddr *)&bound_addr, &bound_len) != 0) {
    state->fail_step = 3;
    CLOSESOCK(listener);
    state->failed = 1;
    THREAD_RETURN(0);
  }

  state->port = ntohs(bound_addr.sin_port);
  state->ready = 1;
  client = accept(listener, NULL, NULL);
  CLOSESOCK(listener);
  if (client == (native_sock_t)-1) {
    state->fail_step = 4;
    state->failed = 1;
    THREAD_RETURN(0);
  }

  for (i = 0; i < 4; ++i) {
    if (recv_redis_command(client, state->requests[i],
                           sizeof(state->requests[i])) != 0) {
      state->fail_step = 5 + i;
      state->failed = 1;
      CLOSESOCK(client);
      THREAD_RETURN(0);
    }
    if (strcmp(state->requests[i], expected_requests[i]) != 0) {
      state->fail_step = 10 + i;
      state->failed = 1;
      CLOSESOCK(client);
      THREAD_RETURN(0);
    }
    if (send_all_sock(client, (const unsigned char *)responses[i],
                      strlen(responses[i])) != 0) {
      state->fail_step = 15 + i;
      state->failed = 1;
      CLOSESOCK(client);
      THREAD_RETURN(0);
    }
  }

  CLOSESOCK(client);
  THREAD_RETURN(0);
}

static THREAD_RET THREAD_CALL wasm3_http_proxy_thread(void *arg) {
  wasm3_proxy_mock_state_t *state = (wasm3_proxy_mock_state_t *)arg;
  native_sock_t listener;
  native_sock_t client;
  struct sockaddr_in addr;
  struct sockaddr_in bound_addr;
#ifdef _WIN32
  int bound_len;
#else
  socklen_t bound_len;
#endif
  unsigned char head[4];
  unsigned char auth_req[512];
  unsigned char connect_req[512];
  unsigned char resp[32];
  size_t host_len;
  int rc = 0;
  const char *body_a;
  const char *body_b;
  char header[512];

  listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listener == (native_sock_t)-1) {
    state->fail_step = 1;
    state->failed = 1;
    THREAD_RETURN(0);
  }

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons((uint16_t)state->port);

  if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
      listen(listener, 1) != 0) {
    state->fail_step = 2;
    CLOSESOCK(listener);
    state->failed = 1;
    THREAD_RETURN(0);
  }

  bound_len = (int)sizeof(bound_addr);
  if (getsockname(listener, (struct sockaddr *)&bound_addr, &bound_len) != 0) {
    state->fail_step = 3;
    CLOSESOCK(listener);
    state->failed = 1;
    THREAD_RETURN(0);
  }

  state->port = ntohs(bound_addr.sin_port);
  state->ready = 1;
  client = accept(listener, NULL, NULL);
  CLOSESOCK(listener);
  if (client == (native_sock_t)-1) {
    state->fail_step = 4;
    state->failed = 1;
    THREAD_RETURN(0);
  }

  rc = recv_exact_sock(client, head, 2);
  if (rc != 0 || head[0] != 0x05) {
    state->fail_step = 5;
    state->failed = 1;
    CLOSESOCK(client);
    THREAD_RETURN(0);
  }
  rc = recv_exact_sock(client, head + 2, head[1]);
  if (rc != 0) {
    state->fail_step = 6;
    state->failed = 1;
    CLOSESOCK(client);
    THREAD_RETURN(0);
  }

  resp[0] = 0x05;
  resp[1] = state->auth_required ? 0x02 : 0x00;
  if (send_all_sock(client, resp, 2) != 0) {
    state->fail_step = 7;
    state->failed = 1;
    CLOSESOCK(client);
    THREAD_RETURN(0);
  }

  if (state->auth_required) {
    rc = recv_exact_sock(client, auth_req, 2);
    if (rc != 0 || auth_req[0] != 0x01) {
      state->fail_step = 8;
      state->failed = 1;
      CLOSESOCK(client);
      THREAD_RETURN(0);
    }
    rc = recv_exact_sock(client, auth_req + 2, auth_req[1] + 1);
    if (rc != 0) {
      state->fail_step = 9;
      state->failed = 1;
      CLOSESOCK(client);
      THREAD_RETURN(0);
    }
    rc = recv_exact_sock(client, auth_req + 3 + auth_req[1],
                         auth_req[2 + auth_req[1]]);
    if (rc != 0) {
      state->fail_step = 10;
      state->failed = 1;
      CLOSESOCK(client);
      THREAD_RETURN(0);
    }
    resp[0] = 0x01;
    resp[1] = 0x00;
    if (send_all_sock(client, resp, 2) != 0) {
      state->fail_step = 11;
      state->failed = 1;
      CLOSESOCK(client);
      THREAD_RETURN(0);
    }
  }

  rc = recv_exact_sock(client, connect_req, 4);
  if (rc != 0 || connect_req[0] != 0x05 || connect_req[1] != 0x01) {
    state->fail_step = 12;
    state->failed = 1;
    CLOSESOCK(client);
    THREAD_RETURN(0);
  }

  switch (connect_req[3]) {
  case 0x01:
    rc = recv_exact_sock(client, connect_req + 4, 6);
    break;
  case 0x03:
    rc = recv_exact_sock(client, connect_req + 4, 1);
    if (rc == 0) {
      host_len = connect_req[4];
      rc = recv_exact_sock(client, connect_req + 5, host_len + 2);
    }
    break;
  case 0x04:
    rc = recv_exact_sock(client, connect_req + 4, 18);
    break;
  default:
    rc = -1;
    break;
  }
  if (rc != 0) {
    state->fail_step = 13;
    state->failed = 1;
    CLOSESOCK(client);
    THREAD_RETURN(0);
  }

  resp[0] = 0x05;
  resp[1] = 0x00;
  resp[2] = 0x00;
  resp[3] = 0x01;
  resp[4] = 127;
  resp[5] = 0;
  resp[6] = 0;
  resp[7] = 1;
  resp[8] = 0x1F;
  resp[9] = 0x90;
  if (send_all_sock(client, resp, 10) != 0) {
    state->fail_step = 14;
    state->failed = 1;
    CLOSESOCK(client);
    THREAD_RETURN(0);
  }

  if (recv_http_headers(client, state->request, sizeof(state->request)) != 0) {
    state->fail_step = 15;
    state->failed = 1;
    CLOSESOCK(client);
    THREAD_RETURN(0);
  }
  if (state->expected_path &&
      strstr(state->request, state->expected_path) == NULL) {
    state->fail_step = 16;
    state->failed = 1;
  }
  if (state->expected_header_a &&
      strstr(state->request, state->expected_header_a) == NULL) {
    state->fail_step = state->fail_step ? state->fail_step : 17;
    state->failed = 1;
  }
  if (state->expected_header_b &&
      strstr(state->request, state->expected_header_b) == NULL) {
    state->fail_step = state->fail_step ? state->fail_step : 18;
    state->failed = 1;
  }

  state->saw_http_request = 1;
  body_a = state->response_body_a ? state->response_body_a : "HELLO";
  body_b = state->response_body_b ? state->response_body_b : "";
  if (state->response_chunked) {
    static const char prelude[] =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Content-Type: text/plain\r\n"
        "Connection: close\r\n"
        "\r\n";
    rc = send_all_sock(client, (const unsigned char *)prelude, strlen(prelude));
    if (rc == 0) {
      snprintf(header, sizeof(header), "%x\r\n", (unsigned)strlen(body_a));
      rc = send_all_sock(client, (const unsigned char *)header, strlen(header));
    }
    if (rc == 0) {
      rc = send_all_sock(client, (const unsigned char *)body_a, strlen(body_a));
    }
    if (rc == 0) {
      rc = send_all_sock(client, (const unsigned char *)"\r\n", 2);
    }
    if (rc == 0) {
      native_sleep_ms(20);
      snprintf(header, sizeof(header), "%x\r\n", (unsigned)strlen(body_b));
      rc = send_all_sock(client, (const unsigned char *)header, strlen(header));
    }
    if (rc == 0) {
      rc = send_all_sock(client, (const unsigned char *)body_b, strlen(body_b));
    }
    if (rc == 0) {
      rc = send_all_sock(client, (const unsigned char *)"\r\n0\r\n\r\n", 7);
    }
  } else if (state->response_sse) {
    static const char prelude[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n"
        "\r\n";
    rc = send_all_sock(client, (const unsigned char *)prelude, strlen(prelude));
    if (rc == 0) {
      rc = send_all_sock(client, (const unsigned char *)body_a, strlen(body_a));
    }
    if (rc == 0) {
      native_sleep_ms(20);
      rc = send_all_sock(client, (const unsigned char *)body_b, strlen(body_b));
    }
  } else {
    snprintf(header, sizeof(header),
             "HTTP/1.1 200 OK\r\n"
             "Content-Length: %u\r\n"
             "Content-Type: text/plain\r\n"
             "Connection: close\r\n"
             "\r\n",
             (unsigned)strlen(body_a));
    rc = send_all_sock(client, (const unsigned char *)header, strlen(header));
    if (rc == 0) {
      rc = send_all_sock(client, (const unsigned char *)body_a, strlen(body_a));
    }
  }

  if (rc != 0) {
    state->fail_step = state->fail_step ? state->fail_step : 19;
    state->failed = 1;
  }

  CLOSESOCK(client);
  THREAD_RETURN(0);
}

static void wait_mock_ready(int *ready, int *failed) {
  int i;

  for (i = 0; i < 200 && !*ready && !*failed; ++i) {
    native_sleep_ms(10);
  }
}

static void run_context_until(coro_context_t *ctx, int *done, uint64_t timeout_ms) {
  uint64_t deadline;

  if (!ctx || !done) {
    return;
  }

  deadline = turbo_monotonic_ms() + timeout_ms;
  while (!*done && turbo_monotonic_ms() < deadline) {
    coro_context_run(ctx, TURBO_RUN_ONCE);
  }
}

static void drain_context_destroy(coro_context_t *ctx) {
  uint64_t deadline;

  if (!ctx) {
    return;
  }

  deadline = turbo_monotonic_ms() + 1000;
  while (coro_context_alive(ctx) && turbo_monotonic_ms() < deadline) {
    coro_context_run(ctx, TURBO_RUN_ONCE);
  }

  coro_context_destroy(ctx);
}

static void wasm3_socket_echo_handler(coro_socket_t *client, void *arg) {
  char *data = NULL;
  size_t len = 0;
  coro_context_t *ctx;

  (void)arg;
  ctx = coro_socket_get_context(client);

  if (coro_socket_recv(client, &data, &len) == 0 && data) {
    coro_socket_send(client, data, len);
    coro_socket_free_recv(data);
    if (ctx) {
      coro_sleep(ctx, 10);
    }
  }
}

static void wasm3_socket_bridge_task(coro_t *co, void *arg) {
  wasm3_socket_bridge_state_t *state = (wasm3_socket_bridge_state_t *)arg;
  coro_socket_t *server = NULL;
  coro_socket_t *client = NULL;
  const m3_wasi_socket_ops_t *ops = NULL;
  void *user_data = NULL;
  uint64_t recv_deadline = 0;
  uint8_t recv_after_unregister_buf[8];
  uint32_t recv_after_unregister_len = 0;
  uint16_t recv_after_unregister_flags = 0;
  unsigned short port = 19998U;

  (void)co;
  memset(recv_after_unregister_buf, 0, sizeof(recv_after_unregister_buf));

  state->stage = 1;
  server = coro_socket_create_tcpv4(state->ctx);
  if (!server) {
    state->task_rc = TURBO_ENOMEM;
    goto done;
  }

  state->stage = 2;
  state->task_rc = coro_socket_listen_on(server, "127.0.0.1", (int)port,
                                         wasm3_socket_echo_handler, NULL);
  if (state->task_rc != 0) {
    goto done;
  }

  state->stage = 3;
  coro_yield();

  state->stage = 4;
  client = coro_socket_create_tcpv4(state->ctx);
  if (!client) {
    state->task_rc = TURBO_ENOMEM;
    goto done;
  }

  coro_socket_set_timeout(client, 2000);
  state->stage = 5;
  state->task_rc = coro_socket_connect(client, "127.0.0.1", (int)port);
  if (state->task_rc != 0) {
    goto done;
  }

  state->stage = 6;
  state->task_rc =
      turbo_wasm3_socket_registry_register(state->registry, client, &state->wasi_fd);
  if (state->task_rc != 0) {
    goto done;
  }

  state->stage = 7;
  state->lookup_matches =
      turbo_wasm3_socket_registry_lookup(state->registry, state->wasi_fd) == client;

  state->stage = 8;
  ops = m3_wasi_context_get_socket_ops(state->wasi, &user_data);
  if (!ops || !ops->send || !ops->recv || !ops->shutdown ||
      user_data != state->registry) {
    state->task_rc = TURBO_EINVAL;
    goto done;
  }

  state->stage = 9;
  state->send_err =
      ops->send(user_data, state->wasi_fd, (const uint8_t *)"ping", 4, 0,
                &state->sent_len);
  state->stage = 10;
  recv_deadline = turbo_monotonic_ms() + 1000;
  do {
    state->recv_err =
        ops->recv(user_data, state->wasi_fd, (uint8_t *)state->recv_buf,
                  (uint32_t)(sizeof(state->recv_buf) - 1), 0, &state->recv_len,
                  &state->recv_flags);
    if (state->recv_err == 0) {
      break;
    }
    coro_sleep(state->ctx, 1);
  } while (turbo_monotonic_ms() < recv_deadline);
  if (state->recv_len < sizeof(state->recv_buf)) {
    state->recv_buf[state->recv_len] = '\0';
  } else {
    state->recv_buf[sizeof(state->recv_buf) - 1] = '\0';
  }

  state->stage = 11;
  state->shutdown_err = ops->shutdown(user_data, state->wasi_fd, 0);
  state->stage = 12;
  state->unregister_rc =
      turbo_wasm3_socket_registry_unregister(state->registry, state->wasi_fd);
  state->stage = 13;
  state->recv_after_unregister_err =
      ops->recv(user_data, state->wasi_fd, recv_after_unregister_buf,
                (uint32_t)sizeof(recv_after_unregister_buf), 0,
                &recv_after_unregister_len, &recv_after_unregister_flags);

done:
  if (client) {
    coro_socket_destroy(client);
  }
  if (server) {
    coro_sleep(state->ctx, 50);
    coro_socket_destroy(server);
  }
  state->finished = 1;
}

static int make_fixture_dir(char *dir, size_t dir_size, const char *content) {
  char tmpdir[TURBO_FS_MAX_PATH];
  char dirname[64];
  char file_path[TURBO_FS_MAX_PATH];
  int rc;
  int written;
  turbo_fs_buf_t file = {0};

  rc = turbo_fs_get_tmpdir(tmpdir, sizeof(tmpdir));
  if (rc != 0) {
    return rc;
  }

  written = snprintf(dirname, sizeof(dirname), "turbo_wasm3_%lu_%lu",
                     (unsigned long)TURBO_WASM3_GETPID(), ++g_fixture_counter);
  if (written < 0 || (size_t)written >= sizeof(dirname)) {
    return -1;
  }

  rc = turbo_fs_path_join(dir, dir_size, tmpdir, dirname);
  if (rc != 0) {
    return rc;
  }

  rc = turbo_fs_mkdir(dir, 0755);
  if (rc != 0) {
    return rc;
  }

  rc = turbo_fs_path_join(file_path, sizeof(file_path), dir, "0.txt");
  if (rc != 0) {
    return rc;
  }

  file = turbo_fs_buf_init((char *)content, strlen(content));
  return turbo_fs_write_file(file_path, &file);
}

static void cleanup_fixture_dir(const char *dir) {
  char path[TURBO_FS_MAX_PATH];

  if (!dir || dir[0] == '\0') {
    return;
  }

  if (turbo_fs_path_join(path, sizeof(path), dir, "0.txt") == 0) {
    turbo_fs_unlink(path);
  }
  if (turbo_fs_path_join(path, sizeof(path), dir, "db.sqlite3") == 0) {
    turbo_fs_unlink(path);
  }
  if (turbo_fs_path_join(path, sizeof(path), dir, "stdout.txt") == 0) {
    turbo_fs_unlink(path);
  }
  turbo_fs_rmdir(dir);
}

static const char *configure_cat_vm(turbo_wasm3_vm_t *vm, const char *host_dir,
                                    IM3Function *start) {
  const char *argv[] = {"test.wasm", "cat", "/0.txt"};
  IM3Module module = NULL;
  M3Result result = m3Err_none;

  if (!vm || !host_dir || !start) {
    return "invalid test setup";
  }

  turbo_wasm3_vm_reset_preopens(vm);
  if (turbo_wasm3_vm_set_preopen(vm, 3, "/", host_dir) != 0) {
    return "failed to set preopen";
  }
  if (turbo_wasm3_vm_set_wasi_args(vm, 3, argv) != 0) {
    return "failed to set wasi args";
  }

  result = turbo_wasm3_vm_load_module_file(vm, WASM3_TEST_WASM_PATH, "simple",
                                           &module);
  if (result) {
    return result;
  }

  return m3_FindFunction(start, turbo_wasm3_vm_get_runtime(vm), "_start");
}

static M3Result call_start_capture(turbo_wasm3_vm_t *vm, IM3Function start,
                                   const char *capture_path, char *output,
                                   size_t output_size) {
  m3_wasi_context_t *wasi = turbo_wasm3_vm_get_wasi_context(vm);
  turbo_fs_buf_t captured = {0};
  FILE *capture = NULL;
  int saved_stdout = -1;
  int stdout_fd;
  M3Result result = m3Err_none;

  if (!vm || !start || !capture_path || !output || output_size == 0) {
    return "invalid capture request";
  }

  stdout_fd = TURBO_WASM3_FILENO(stdout);
  output[0] = '\0';

  fflush(stdout);
  saved_stdout = TURBO_WASM3_DUP(stdout_fd);
  if (saved_stdout < 0) {
    return "failed to duplicate stdout";
  }

  capture = fopen(capture_path, "wb");
  if (!capture) {
    TURBO_WASM3_CLOSE(saved_stdout);
    return "failed to open capture file";
  }

  if (TURBO_WASM3_DUP2(TURBO_WASM3_FILENO(capture), stdout_fd) < 0) {
    fclose(capture);
    TURBO_WASM3_CLOSE(saved_stdout);
    return "failed to redirect stdout";
  }

  result = m3_CallArgv(start, 0, NULL);
  fflush(stdout);

  TURBO_WASM3_DUP2(saved_stdout, stdout_fd);
  TURBO_WASM3_CLOSE(saved_stdout);
  fclose(capture);

  if (result == m3Err_trapExit && wasi && wasi->exit_code == 0) {
    result = m3Err_none;
  }
  if (result) {
    return result;
  }

  if (turbo_fs_read_file(capture_path, &captured) != 0) {
    return "failed to read capture file";
  }

  if (captured.len >= output_size) {
    turbo_fs_buf_free(&captured);
    return "capture buffer too small";
  }

  memcpy(output, captured.base, captured.len);
  output[captured.len] = '\0';
  turbo_fs_buf_free(&captured);
  return m3Err_none;
}

spec("Turbo wasm3 integration") {

  before_all() {
    memset(g_fixture_a, 0, sizeof(g_fixture_a));
    memset(g_fixture_b, 0, sizeof(g_fixture_b));
  }

  after_all() {
    cleanup_fixture_dir(g_fixture_a);
    cleanup_fixture_dir(g_fixture_b);
  }

  describe("basic runtime") {
    it("loads a wasm module and calls an exported function") {
      turbo_wasm3_vm_t *vm = turbo_wasm3_vm_create(64U * 1024U, NULL, 16);
      IM3Module module = NULL;
      IM3Function fib = NULL;
      uint32_t value = 0;

      check_not_null(vm);
      check_null(turbo_wasm3_vm_load_module_file(vm, WASM3_TEST_WASM_PATH, "simple",
                                                 &module));
      check_not_null(module);
      check_null(m3_FindFunction(&fib, turbo_wasm3_vm_get_runtime(vm), "fib"));
      check_not_null(fib);
      check_null(m3_CallV(fib, 10));
      check_null(m3_GetResultsV(fib, &value));
      check_int_eq((int)value, 55);

      turbo_wasm3_vm_destroy(vm);
    }
  }

  describe("host linker layer") {
    it("keeps vm host state and preserves wasi module loading") {
      turbo_wasm3_vm_t *vm = turbo_wasm3_vm_create(64U * 1024U, NULL, 16);
      IM3Module module = NULL;
      int marker = 42;

      check_not_null(vm);
      turbo_wasm3_vm_set_host_user_data(vm, &marker);
      check_ptr_eq(turbo_wasm3_vm_get_host_user_data(vm), &marker);
      check_int_eq(turbo_wasm3_vm_enable_host(vm), 0);
      check_int_eq(turbo_wasm3_vm_enable_host(vm), 0);
      check_null(turbo_wasm3_vm_load_module_file(vm, WASM3_TEST_WASM_PATH, "simple",
                                                 &module));
      check_not_null(module);

      turbo_wasm3_vm_destroy(vm);
    }
  }

  describe("database registry") {
    it("keeps default sqlite host limited to in-memory databases") {
      turbo_wasm3_vm_t *vm = turbo_wasm3_vm_create(64U * 1024U, NULL, 16);
      turbo_wasm3_db_registry_t *db = NULL;
      char db_path[TURBO_FS_MAX_PATH];
      uint32_t handle = 0;

      check_not_null(vm);
      check_int_eq(make_fixture_dir(g_fixture_a, sizeof(g_fixture_a), "dbpolicy\n"),
                   0);
      check_int_eq(
          turbo_fs_path_join(db_path, sizeof(db_path), g_fixture_a, "db.sqlite3"),
          0);

      db = turbo_wasm3_vm_get_db_registry(vm);
      check_not_null(db);
      check_int_eq(turbo_wasm3_vm_enable_sqlite_db(vm), 0);
      check_int_eq(turbo_wasm3_db_registry_open(db, ":memory:", &handle), 0);
      check_int_eq(turbo_wasm3_db_registry_close(db, handle), 0);
      handle = 0;
      check_int_eq(turbo_wasm3_db_registry_open(db, db_path, &handle),
                   TURBO_EPERM);

      turbo_wasm3_vm_destroy(vm);
    }

    it("executes sqlite statements through the shared db host layer") {
      turbo_wasm3_vm_t *vm = turbo_wasm3_vm_create(64U * 1024U, NULL, 16);
      turbo_wasm3_db_registry_t *db = NULL;
      char db_path[TURBO_FS_MAX_PATH];
      char errbuf[128];
      uint32_t handle = 0;
      uint32_t insert_stmt = 0;
      uint32_t select_stmt = 0;
      uint32_t conflict_stmt = 0;
      uint32_t errlen = 0;
      uint32_t text_len = 0;
      uint32_t blob_len = 0;
      uint64_t changes = 0;
      int32_t step_state = TURBO_WASM3_DB_STEP_DONE;
      int32_t column_type = TURBO_WASM3_DB_TYPE_NULL;
      int64_t id_value = 0;
      double score_value = 0.0;
      char name_buf[32];
      uint8_t payload[4] = {0x41, 0x42, 0x00, 0x7f};
      uint8_t payload_buf[8];

      check_not_null(vm);
      check_int_eq(make_fixture_dir(g_fixture_a, sizeof(g_fixture_a), "db\n"), 0);
      check_int_eq(
          turbo_fs_path_join(db_path, sizeof(db_path), g_fixture_a, "db.sqlite3"),
          0);

      db = turbo_wasm3_vm_get_db_registry(vm);
      check_not_null(db);
      check_int_eq(turbo_wasm3_vm_enable_sqlite_db_with_base_dir(vm, g_fixture_a),
                   0);
      check_int_eq(turbo_wasm3_vm_enable_host(vm), 0);
      check_int_eq(turbo_wasm3_db_registry_open(db, ":memory:", &handle), 0);
      check_int_eq(turbo_wasm3_db_registry_close(db, handle), 0);
      handle = 0;
      check_int_eq(turbo_wasm3_db_registry_open(db, db_path, &handle), 0);
      check_int_ne((int)handle, 0);
      check_int_eq(
          turbo_wasm3_db_registry_exec(
              db, handle,
              "create table if not exists items(id integer primary key, name text, "
              "score real, payload blob, note text);",
              &changes),
          0);
      check_int_eq((int)changes, 0);
      check_int_eq(
          turbo_wasm3_db_registry_exec(
              db, handle,
              "insert into items(name, score, payload, note) values('TurboNet', 1.5, "
              "X'0102', null);",
              &changes),
          0);
      check_int_eq((int)changes, 1);
      check_int_eq(
          turbo_wasm3_db_registry_prepare(
              db, handle,
              "insert into items(id, name, score, payload, note) values(?, ?, ?, ?, ?);",
              &insert_stmt),
          0);
      check_int_ne((int)insert_stmt, 0);
      check_int_eq(
          turbo_wasm3_db_registry_bind_int64(db, insert_stmt, 1, 2), 0);
      check_int_eq(
          turbo_wasm3_db_registry_bind_text(db, insert_stmt, 2, "academy", 7), 0);
      check_int_eq(
          turbo_wasm3_db_registry_bind_double(db, insert_stmt, 3, 4.25), 0);
      check_int_eq(
          turbo_wasm3_db_registry_bind_blob(db, insert_stmt, 4, payload,
                                            sizeof(payload)),
          0);
      check_int_eq(
          turbo_wasm3_db_registry_bind_null(db, insert_stmt, 5), 0);
      check_int_eq(
          turbo_wasm3_db_registry_step(db, insert_stmt, &step_state), 0);
      check_int_eq((int)step_state, TURBO_WASM3_DB_STEP_DONE);
      check_int_eq(
          turbo_wasm3_db_registry_finalize(db, insert_stmt), 0);
      insert_stmt = 0;
      check_int_eq(
          turbo_wasm3_db_registry_prepare(
              db, handle,
              "select id, name, score, payload, note from items where id = ?;",
              &select_stmt),
          0);
      check_int_ne((int)select_stmt, 0);
      check_int_eq(
          turbo_wasm3_db_registry_bind_int64(db, select_stmt, 1, 2), 0);
      check_int_eq(
          turbo_wasm3_db_registry_step(db, select_stmt, &step_state), 0);
      check_int_eq((int)step_state, TURBO_WASM3_DB_STEP_ROW);
      check_int_eq(
          turbo_wasm3_db_registry_column_int64(db, select_stmt, 0, &id_value), 0);
      check_int_eq((int)id_value, 2);
      check_int_eq(
          turbo_wasm3_db_registry_column_text(db, select_stmt, 1, name_buf,
                                              sizeof(name_buf), &text_len),
          0);
      check_int_eq((int)text_len, 7);
      check_str_eq(name_buf, "academy");
      check_int_eq(
          turbo_wasm3_db_registry_column_type(db, select_stmt, 2, &column_type), 0);
      check_int_eq((int)column_type, TURBO_WASM3_DB_TYPE_DOUBLE);
      check_int_eq(
          turbo_wasm3_db_registry_column_double(db, select_stmt, 2, &score_value), 0);
      check_true(score_value > 4.24 && score_value < 4.26);
      check_int_eq(
          turbo_wasm3_db_registry_column_type(db, select_stmt, 3, &column_type), 0);
      check_int_eq((int)column_type, TURBO_WASM3_DB_TYPE_BLOB);
      check_int_eq(
          turbo_wasm3_db_registry_column_blob(db, select_stmt, 3, payload_buf,
                                              sizeof(payload_buf), &blob_len),
          0);
      check_int_eq((int)blob_len, (int)sizeof(payload));
      check_mem_eq(payload_buf, payload, sizeof(payload));
      check_int_eq(
          turbo_wasm3_db_registry_column_type(db, select_stmt, 4, &column_type), 0);
      check_int_eq((int)column_type, TURBO_WASM3_DB_TYPE_NULL);
      check_int_eq(
          turbo_wasm3_db_registry_step(db, select_stmt, &step_state), 0);
      check_int_eq((int)step_state, TURBO_WASM3_DB_STEP_DONE);
      check_int_eq(
          turbo_wasm3_db_registry_reset(db, select_stmt), 0);
      check_int_eq(
          turbo_wasm3_db_registry_bind_int64(db, select_stmt, 1, 1), 0);
      check_int_eq(
          turbo_wasm3_db_registry_step(db, select_stmt, &step_state), 0);
      check_int_eq((int)step_state, TURBO_WASM3_DB_STEP_ROW);
      check_int_eq(
          turbo_wasm3_db_registry_column_text(db, select_stmt, 1, name_buf,
                                              sizeof(name_buf), &text_len),
          0);
      check_str_eq(name_buf, "TurboNet");
      check_int_eq(
          turbo_wasm3_db_registry_finalize(db, select_stmt), 0);
      select_stmt = 0;
      check_int_eq(
          turbo_wasm3_db_registry_prepare(
              db, handle,
              "insert into items(id, name, score, payload, note) values(?, ?, ?, ?, ?);",
              &conflict_stmt),
          0);
      check_int_ne(
          turbo_wasm3_db_registry_bind_int64(db, conflict_stmt, 6, 1), 0);
      check_int_eq(
          turbo_wasm3_db_registry_stmt_error(db, conflict_stmt, errbuf,
                                             sizeof(errbuf), &errlen),
          0);
      check_int_ne((int)errlen, 0);
      check_int_ne((int)errbuf[0], 0);
      check_int_eq(
          turbo_wasm3_db_registry_finalize(db, conflict_stmt), 0);
      conflict_stmt = 0;
      check_int_eq(
          turbo_wasm3_db_registry_exec(
              db, handle, "update items set name = 'patched' where id = 2;",
              &changes),
          0);
      check_int_eq((int)changes, 1);
      check_int_eq(
          turbo_wasm3_db_registry_prepare(
              db, handle, "select name from items where id = 2;", &select_stmt),
          0);
      check_int_eq(
          turbo_wasm3_db_registry_step(db, select_stmt, &step_state), 0);
      check_int_eq((int)step_state, TURBO_WASM3_DB_STEP_ROW);
      check_int_eq(
          turbo_wasm3_db_registry_column_text(db, select_stmt, 0, name_buf,
                                              sizeof(name_buf), &text_len),
          0);
      check_str_eq(name_buf, "patched");
      check_int_eq(
          turbo_wasm3_db_registry_finalize(db, select_stmt), 0);
      select_stmt = 0;
      check_int_eq(
          turbo_wasm3_db_registry_exec(
              db, handle, "delete from items where id = 2;", &changes),
          0);
      check_int_eq((int)changes, 1);
      check_int_eq(
          turbo_wasm3_db_registry_prepare(
              db, handle, "select count(*) from items where id = 2;", &select_stmt),
          0);
      check_int_eq(
          turbo_wasm3_db_registry_step(db, select_stmt, &step_state), 0);
      check_int_eq((int)step_state, TURBO_WASM3_DB_STEP_ROW);
      check_int_eq(
          turbo_wasm3_db_registry_column_int64(db, select_stmt, 0, &id_value), 0);
      check_int_eq((int)id_value, 0);
      check_int_eq(
          turbo_wasm3_db_registry_finalize(db, select_stmt), 0);
      select_stmt = 0;
      check_int_ne(turbo_wasm3_db_registry_exec(db, handle, "insert into", &changes),
                   0);
      check_int_eq(
          turbo_wasm3_db_registry_error(db, handle, errbuf, sizeof(errbuf), &errlen),
          0);
      check_int_ne((int)errlen, 0);
      check_int_ne((int)errbuf[0], 0);
      check_int_ne(
          turbo_wasm3_db_registry_prepare(db, handle, "select from", &select_stmt), 0);
      check_int_eq(
          turbo_wasm3_db_registry_error(db, handle, errbuf, sizeof(errbuf), &errlen),
          0);
      check_int_ne((int)errlen, 0);
      check_int_eq(turbo_wasm3_db_registry_close(db, handle), 0);

      turbo_wasm3_vm_destroy(vm);
    }
  }

  describe("http registry") {
    it("opens clients and captures local request errors through the shared http host layer") {
      turbo_wasm3_vm_t *vm = turbo_wasm3_vm_create(64U * 1024U, NULL, 16);
      turbo_wasm3_http_registry_t *http = NULL;
      uint32_t client_handle = 0;
      uint32_t response_handle = 0;
      uint32_t errlen = 0;
      int32_t status_code = -1;
      int32_t error_code = 0;
      char errbuf[128];

      check_not_null(vm);
      http = turbo_wasm3_vm_get_http_registry(vm);
      check_not_null(http);
      check_int_eq(turbo_wasm3_vm_enable_http_host(vm), 0);
      check_int_eq(
          turbo_wasm3_http_registry_open_client(http, "http://localhost:8080",
                                                &client_handle),
          0);
      check_int_ne((int)client_handle, 0);
      check_int_eq(
          turbo_wasm3_http_registry_set_timeout(http, client_handle, 100), 0);
      check_int_eq(
          turbo_wasm3_http_registry_request_with_headers(
              http, client_handle, TURBO_WASM3_HTTP_METHOD_GET,
              "not-a-valid-url",
              "Accept: application/json\nX-Test: 1",
              strlen("Accept: application/json\nX-Test: 1"), NULL, 0,
              &response_handle),
          0);
      check_int_ne((int)response_handle, 0);
      check_int_eq(
          turbo_wasm3_http_registry_response_status(http, response_handle,
                                                    &status_code),
          0);
      check_int_eq((int)status_code, 0);
      check_int_eq(
          turbo_wasm3_http_registry_response_error_code(http, response_handle,
                                                        &error_code),
          0);
      check_int_ne((int)error_code, 0);
      check_int_eq(
          turbo_wasm3_http_registry_response_header(http, response_handle,
                                                    "Content-Type", errbuf,
                                                    sizeof(errbuf), &errlen),
          0);
      check_int_eq((int)errlen, 0);
      check_int_eq(
          turbo_wasm3_http_registry_response_error(http, response_handle, errbuf,
                                                   sizeof(errbuf), &errlen),
          0);
      check_int_ne((int)errlen, 0);
      check_int_ne((int)errbuf[0], 0);
      check_int_eq(
          turbo_wasm3_http_registry_close_response(http, response_handle), 0);
      check_int_eq(
          turbo_wasm3_http_registry_close_client(http, client_handle), 0);

      turbo_wasm3_vm_destroy(vm);
    }

    it("applies default headers and auth through the shared http host layer") {
      wasm3_proxy_mock_state_t state = {
          .expected_path = "GET /config HTTP/1.1\r\n",
          .expected_header_a = "Host: example.com:8088\r\n",
          .expected_header_b = "Authorization: Bearer token-123\r\n",
          .response_body_a = "CONFIG",
      };
      native_thread_t thread;
      turbo_wasm3_vm_t *vm = NULL;
      turbo_wasm3_http_registry_t *http = NULL;
      uint32_t client_handle = 0;
      uint32_t response_handle = 0;
      uint32_t body_len = 0;
      int32_t status_code = 0;
      char body[32];

      ensure_native_sockets_ready();
      check_int_eq(native_thread_create(&thread, wasm3_http_proxy_thread, &state), 0);
      wait_mock_ready(&state.ready, &state.failed);
      check_int_eq(state.failed, 0);
      check_int_ne(state.port, 0);

      vm = turbo_wasm3_vm_create(64U * 1024U, NULL, 16);
      check_not_null(vm);
      http = turbo_wasm3_vm_get_http_registry(vm);
      check_not_null(http);
      check_int_eq(turbo_wasm3_http_registry_open_client(http, NULL,
                                                         &client_handle),
                   0);
      check_int_eq(turbo_wasm3_http_registry_set_proxy(
                       http, client_handle, "127.0.0.1", (uint16_t)state.port,
                       NULL, NULL),
                   0);
      check_int_eq(turbo_wasm3_http_registry_set_default_header(
                       http, client_handle, "X-TurboNet", "enabled"),
                   0);
      check_int_eq(turbo_wasm3_http_registry_set_bearer_token(
                       http, client_handle, "token-123"),
                   0);
      check_int_eq(
          turbo_wasm3_http_registry_request(
              http, client_handle, TURBO_WASM3_HTTP_METHOD_GET,
              "http://example.com:8088/config", NULL, 0, &response_handle),
          0);
      native_thread_join(&thread);
      if (state.failed) {
        printf("http config proxy failed at step %d, request=\n%s\n",
               state.fail_step, state.request);
      }
      check_int_eq(state.failed, 0);
      check_int_eq(
          turbo_wasm3_http_registry_response_status(http, response_handle,
                                                    &status_code),
          0);
      check_int_eq((int)status_code, 200);
      check_int_eq(
          turbo_wasm3_http_registry_response_body(http, response_handle, body,
                                                  sizeof(body), &body_len),
          0);
      body[body_len] = '\0';
      check_str_eq(body, "CONFIG");
      check_int_eq(
          turbo_wasm3_http_registry_close_response(http, response_handle), 0);
      check_int_eq(
          turbo_wasm3_http_registry_clear_proxy(http, client_handle), 0);
      check_int_eq(
          turbo_wasm3_http_registry_clear_auth(http, client_handle), 0);
      check_int_eq(turbo_wasm3_http_registry_remove_default_header(
                       http, client_handle, "X-TurboNet"),
                   0);
      check_int_eq(
          turbo_wasm3_http_registry_clear_default_headers(http, client_handle), 0);
      check_int_eq(
          turbo_wasm3_http_registry_close_client(http, client_handle), 0);

      turbo_wasm3_vm_destroy(vm);
    }

    it("captures streaming chunks and SSE bodies through the shared http host layer") {
      wasm3_proxy_mock_state_t stream_state = {
          .expected_path = "GET /stream HTTP/1.1\r\n",
          .expected_header_a = "Host: example.com:8088\r\n",
          .response_body_a = "first-",
          .response_body_b = "second",
          .response_chunked = 1,
      };
      wasm3_proxy_mock_state_t sse_state = {
          .expected_path = "GET /events HTTP/1.1\r\n",
          .expected_header_a = "Host: example.com:8088\r\n",
          .expected_header_b = "Accept: text/event-stream\r\n",
          .response_body_a = "data: one\n\n",
          .response_body_b = "data: two\n\n",
          .response_sse = 1,
      };
      native_thread_t stream_thread;
      native_thread_t sse_thread;
      turbo_wasm3_vm_t *vm = NULL;
      turbo_wasm3_http_registry_t *http = NULL;
      uint32_t client_handle = 0;
      uint32_t response_handle = 0;
      uint32_t chunk_count = 0;
      uint32_t chunk_len = 0;
      uint32_t body_len = 0;
      int32_t is_sse = 0;
      char body[64];
      char chunk[32];

      ensure_native_sockets_ready();
      check_int_eq(native_thread_create(&stream_thread, wasm3_http_proxy_thread,
                                        &stream_state),
                   0);
      wait_mock_ready(&stream_state.ready, &stream_state.failed);
      check_int_eq(stream_state.failed, 0);

      vm = turbo_wasm3_vm_create(64U * 1024U, NULL, 16);
      check_not_null(vm);
      http = turbo_wasm3_vm_get_http_registry(vm);
      check_not_null(http);
      check_int_eq(turbo_wasm3_http_registry_open_client(http, NULL,
                                                         &client_handle),
                   0);
      check_int_eq(turbo_wasm3_http_registry_set_proxy(
                       http, client_handle, "127.0.0.1",
                       (uint16_t)stream_state.port, NULL, NULL),
                   0);
      check_int_eq(turbo_wasm3_http_registry_stream_get(
                       http, client_handle, "http://example.com:8088/stream",
                       &response_handle),
                   0);
      native_thread_join(&stream_thread);
      if (stream_state.failed) {
        printf("http stream proxy failed at step %d, request=\n%s\n",
               stream_state.fail_step, stream_state.request);
      }
      check_int_eq(stream_state.failed, 0);
      check_int_eq(turbo_wasm3_http_registry_response_chunk_count(
                       http, response_handle, &chunk_count),
                   0);
      check_int_eq((int)chunk_count, 2);
      check_int_eq(turbo_wasm3_http_registry_response_chunk(
                       http, response_handle, 0, chunk, sizeof(chunk), &chunk_len),
                   0);
      chunk[chunk_len] = '\0';
      check_str_eq(chunk, "first-");
      check_int_eq(
          turbo_wasm3_http_registry_response_body(http, response_handle, body,
                                                  sizeof(body), &body_len),
          TURBO_ENOTSUP);
      check_int_eq(
          turbo_wasm3_http_registry_close_response(http, response_handle), 0);
      check_int_eq(
          turbo_wasm3_http_registry_clear_proxy(http, client_handle), 0);
      check_int_eq(
          turbo_wasm3_http_registry_close_client(http, client_handle), 0);

      check_int_eq(
          native_thread_create(&sse_thread, wasm3_http_proxy_thread, &sse_state), 0);
      wait_mock_ready(&sse_state.ready, &sse_state.failed);
      check_int_eq(sse_state.failed, 0);
      check_int_eq(turbo_wasm3_http_registry_open_client(http, NULL,
                                                         &client_handle),
                   0);
      check_int_eq(turbo_wasm3_http_registry_set_proxy(
                       http, client_handle, "127.0.0.1", (uint16_t)sse_state.port,
                       NULL, NULL),
                   0);
      check_int_eq(
          turbo_wasm3_http_registry_sse_get(
              http, client_handle, "http://example.com:8088/events",
              &response_handle),
          0);
      native_thread_join(&sse_thread);
      if (sse_state.failed) {
        printf("http sse proxy failed at step %d, request=\n%s\n",
               sse_state.fail_step, sse_state.request);
      }
      check_int_eq(sse_state.failed, 0);
      check_int_eq(turbo_wasm3_http_registry_response_is_sse(
                       http, response_handle, &is_sse),
                   0);
      check_int_eq((int)is_sse, 1);
      check_int_eq(turbo_wasm3_http_registry_response_chunk_count(
                       http, response_handle, &chunk_count),
                   0);
      check_int_eq((int)chunk_count, 2);
      check_int_eq(turbo_wasm3_http_registry_response_chunk(
                       http, response_handle, 0, chunk, sizeof(chunk), &chunk_len),
                   0);
      chunk[chunk_len] = '\0';
      check_str_eq(chunk, "data: one\n\n");
      check_int_eq(
          turbo_wasm3_http_registry_response_body(http, response_handle, body,
                                                  sizeof(body), &body_len),
          TURBO_ENOTSUP);
      check_int_eq(
          turbo_wasm3_http_registry_close_response(http, response_handle), 0);
      check_int_eq(
          turbo_wasm3_http_registry_clear_proxy(http, client_handle), 0);
      check_int_eq(
          turbo_wasm3_http_registry_close_client(http, client_handle), 0);

      turbo_wasm3_vm_destroy(vm);
    }

    it("routes requests through a configured socks5 proxy") {
      wasm3_proxy_mock_state_t state = {.auth_required = 1};
      native_thread_t thread;
      turbo_wasm3_vm_t *vm = NULL;
      turbo_wasm3_http_registry_t *http = NULL;
      uint32_t client_handle = 0;
      uint32_t response_handle = 0;
      uint32_t body_len = 0;
      int32_t status_code = 0;
      char body[16];

      ensure_native_sockets_ready();
      check_int_eq(
          native_thread_create(&thread, wasm3_http_proxy_thread, &state), 0);
      wait_mock_ready(&state.ready, &state.failed);
      check_int_eq(state.failed, 0);
      check_int_ne(state.port, 0);

      vm = turbo_wasm3_vm_create(64U * 1024U, NULL, 16);
      check_not_null(vm);
      http = turbo_wasm3_vm_get_http_registry(vm);
      check_not_null(http);
      check_int_eq(turbo_wasm3_http_registry_open_client(http, NULL,
                                                         &client_handle),
                   0);
      check_int_eq(turbo_wasm3_http_registry_set_timeout(http, client_handle, 2000),
                   0);
      check_int_eq(turbo_wasm3_http_registry_set_proxy(
                       http, client_handle, "127.0.0.1", (uint16_t)state.port,
                       "user", "pass"),
                   0);
      check_int_eq(
          turbo_wasm3_http_registry_request(
              http, client_handle, TURBO_WASM3_HTTP_METHOD_GET,
              "http://example.com:8088/proxy-test", NULL, 0, &response_handle),
          0);
      check_int_eq(
          turbo_wasm3_http_registry_response_status(http, response_handle,
                                                    &status_code),
          0);
      check_int_eq((int)status_code, 200);
      check_int_eq(
          turbo_wasm3_http_registry_response_body(http, response_handle, body,
                                                  sizeof(body), &body_len),
          0);
      body[body_len] = '\0';
      check_str_eq(body, "HELLO");
      check_int_eq(
          turbo_wasm3_http_registry_close_response(http, response_handle), 0);
      check_int_eq(
          turbo_wasm3_http_registry_clear_proxy(http, client_handle), 0);
      check_int_eq(
          turbo_wasm3_http_registry_close_client(http, client_handle), 0);

      turbo_wasm3_vm_destroy(vm);
      native_thread_join(&thread);
      check_int_eq(state.failed, 0);
      check_int_eq(state.saw_http_request, 1);
    }
  }

  describe("redis registry") {
    it("executes RESP commands through the shared redis host layer") {
      wasm3_redis_mock_state_t state = {0};
      native_thread_t thread;
      turbo_wasm3_vm_t *vm = NULL;
      turbo_wasm3_redis_registry_t *redis = NULL;
      static const char *const ping_argv[] = {"PING"};
      static const uint32_t ping_lens[] = {4};
      static const char *const incr_argv[] = {"INCR", "counter"};
      static const uint32_t incr_lens[] = {4, 7};
      static const char *const lrange_argv[] = {"LRANGE", "letters", "0", "-1"};
      static const uint32_t lrange_lens[] = {6, 7, 1, 2};
      static const char *const get_argv[] = {"GET", "missing"};
      static const uint32_t get_lens[] = {3, 7};
      uint32_t client_handle = 0;
      uint32_t reply_handle = 0;
      uint32_t child_handle = 0;
      uint32_t text_len = 0;
      uint32_t array_len = 0;
      int32_t type = -1;
      int64_t int_value = 0;
      char text[16];

      ensure_native_sockets_ready();
      check_int_eq(native_thread_create(&thread, wasm3_redis_mock_thread, &state), 0);
      wait_mock_ready(&state.ready, &state.failed);
      check_int_eq(state.failed, 0);
      check_int_ne(state.port, 0);

      vm = turbo_wasm3_vm_create(64U * 1024U, NULL, 16);
      check_not_null(vm);
      redis = turbo_wasm3_vm_get_redis_registry(vm);
      check_not_null(redis);
      check_int_eq(turbo_wasm3_vm_enable_redis_host(vm), 0);

      check_int_eq(turbo_wasm3_redis_registry_open_client(
                       redis, "127.0.0.1", (uint16_t)state.port, &client_handle),
                   0);
      check_int_ne((int)client_handle, 0);

      check_int_eq(turbo_wasm3_redis_registry_command(
                       redis, client_handle, 1, ping_argv, ping_lens, &reply_handle),
                   0);
      check_int_eq(turbo_wasm3_redis_registry_reply_type(redis, reply_handle, &type),
                   0);
      check_int_eq((int)type, TURBO_WASM3_REDIS_REPLY_STRING);
      check_int_eq(turbo_wasm3_redis_registry_reply_text(
                       redis, reply_handle, text, sizeof(text), &text_len),
                   0);
      text[text_len] = '\0';
      check_str_eq(text, "PONG");
      check_int_eq(turbo_wasm3_redis_registry_close_reply(redis, reply_handle), 0);
      reply_handle = 0;

      check_int_eq(turbo_wasm3_redis_registry_command(
                       redis, client_handle, 2, incr_argv, incr_lens, &reply_handle),
                   0);
      check_int_eq(turbo_wasm3_redis_registry_reply_int64(redis, reply_handle,
                                                          &int_value),
                   0);
      check_int_eq((int)int_value, 41);
      check_int_eq(turbo_wasm3_redis_registry_close_reply(redis, reply_handle), 0);
      reply_handle = 0;

      check_int_eq(turbo_wasm3_redis_registry_command(
                       redis, client_handle, 4, lrange_argv, lrange_lens,
                       &reply_handle),
                   0);
      check_int_eq(
          turbo_wasm3_redis_registry_reply_array_len(redis, reply_handle, &array_len),
          0);
      check_int_eq((int)array_len, 2);
      check_int_eq(turbo_wasm3_redis_registry_reply_array_at(
                       redis, reply_handle, 1, &child_handle),
                   0);
      check_int_eq(turbo_wasm3_redis_registry_reply_text(
                       redis, child_handle, text, sizeof(text), &text_len),
                   0);
      text[text_len] = '\0';
      check_str_eq(text, "bb");
      check_int_eq(turbo_wasm3_redis_registry_close_reply(redis, child_handle), 0);
      check_int_eq(turbo_wasm3_redis_registry_close_reply(redis, reply_handle), 0);
      child_handle = 0;
      reply_handle = 0;

      check_int_eq(turbo_wasm3_redis_registry_command(
                       redis, client_handle, 2, get_argv, get_lens, &reply_handle),
                   0);
      check_int_eq(turbo_wasm3_redis_registry_reply_type(redis, reply_handle, &type),
                   0);
      check_int_eq((int)type, TURBO_WASM3_REDIS_REPLY_NULL);
      check_int_eq(turbo_wasm3_redis_registry_close_reply(redis, reply_handle), 0);
      check_int_eq(turbo_wasm3_redis_registry_close_client(redis, client_handle), 0);

      turbo_wasm3_vm_destroy(vm);
      native_thread_join(&thread);
      check_int_eq(state.failed, 0);
    }

    it("keeps array reply access valid across reply table growth") {
      wasm3_redis_mock_state_t state = {0};
      native_thread_t thread;
      turbo_wasm3_vm_t *vm = NULL;
      turbo_wasm3_redis_registry_t *redis = NULL;
      static const char *const ping_argv[] = {"PING"};
      static const uint32_t ping_lens[] = {4};
      static const char *const incr_argv[] = {"INCR", "counter"};
      static const uint32_t incr_lens[] = {4, 7};
      static const char *const lrange_argv[] = {"LRANGE", "letters", "0", "-1"};
      static const uint32_t lrange_lens[] = {6, 7, 1, 2};
      static const char *const get_argv[] = {"GET", "missing"};
      static const uint32_t get_lens[] = {3, 7};
      uint32_t client_handle = 0;
      uint32_t reply_handles[4] = {0};
      uint32_t child_handle = 0;
      uint32_t text_len = 0;
      char text[16];

      ensure_native_sockets_ready();
      check_int_eq(native_thread_create(&thread, wasm3_redis_mock_thread, &state), 0);
      wait_mock_ready(&state.ready, &state.failed);
      check_int_eq(state.failed, 0);
      check_int_ne(state.port, 0);

      vm = turbo_wasm3_vm_create(64U * 1024U, NULL, 16);
      check_not_null(vm);
      redis = turbo_wasm3_vm_get_redis_registry(vm);
      check_not_null(redis);

      check_int_eq(turbo_wasm3_redis_registry_open_client(
                       redis, "127.0.0.1", (uint16_t)state.port, &client_handle),
                   0);
      check_int_ne((int)client_handle, 0);

      check_int_eq(turbo_wasm3_redis_registry_command(
                       redis, client_handle, 1, ping_argv, ping_lens,
                       &reply_handles[0]),
                   0);
      check_int_eq(turbo_wasm3_redis_registry_command(
                       redis, client_handle, 2, incr_argv, incr_lens,
                       &reply_handles[1]),
                   0);
      check_int_eq(turbo_wasm3_redis_registry_command(
                       redis, client_handle, 4, lrange_argv, lrange_lens,
                       &reply_handles[2]),
                   0);
      check_int_eq(turbo_wasm3_redis_registry_command(
                       redis, client_handle, 2, get_argv, get_lens,
                       &reply_handles[3]),
                   0);

      check_int_eq(turbo_wasm3_redis_registry_reply_array_at(
                       redis, reply_handles[2], 1, &child_handle),
                   0);
      check_int_eq(turbo_wasm3_redis_registry_reply_text(
                       redis, child_handle, text, sizeof(text), &text_len),
                   0);
      text[text_len] = '\0';
      check_str_eq(text, "bb");

      check_int_eq(turbo_wasm3_redis_registry_close_reply(redis, child_handle), 0);
      check_int_eq(turbo_wasm3_redis_registry_close_reply(redis, reply_handles[0]),
                   0);
      check_int_eq(turbo_wasm3_redis_registry_close_reply(redis, reply_handles[1]),
                   0);
      check_int_eq(turbo_wasm3_redis_registry_close_reply(redis, reply_handles[2]),
                   0);
      check_int_eq(turbo_wasm3_redis_registry_close_reply(redis, reply_handles[3]),
                   0);
      check_int_eq(turbo_wasm3_redis_registry_close_client(redis, client_handle), 0);

      turbo_wasm3_vm_destroy(vm);
      native_thread_join(&thread);
      check_int_eq(state.failed, 0);
    }
  }

#ifdef WASM3_DB_GUEST_AVAILABLE
  describe("guest database module") {
    it("runs sqlite imports from a wasm guest through the TurboNet host abi") {
      turbo_wasm3_vm_t *vm = turbo_wasm3_vm_create(64U * 1024U, NULL, 16);
      IM3Module module = NULL;
      IM3Function run = NULL;
      int32_t result_value = 0;

      check_not_null(vm);
      check_int_eq(turbo_wasm3_vm_enable_sqlite_db(vm), 0);
      check_int_eq(turbo_wasm3_vm_enable_host(vm), 0);
      check_null(
          turbo_wasm3_vm_load_module_file(vm, WASM3_DB_TEST_WASM_PATH, "db_demo",
                                          &module));
      check_not_null(module);
      check_null(m3_FindFunction(&run, turbo_wasm3_vm_get_runtime(vm), "run_db_demo"));
      check_not_null(run);
      check_null(m3_CallV(run));
      check_null(m3_GetResultsV(run, &result_value));
      check_int_eq((int)result_value, 7);

      turbo_wasm3_vm_destroy(vm);
    }

    it("runs CRUD sqlite imports from a wasm guest through the TurboNet host abi") {
      turbo_wasm3_vm_t *vm = turbo_wasm3_vm_create(64U * 1024U, NULL, 16);
      IM3Module module = NULL;
      IM3Function run = NULL;
      int32_t result_value = 0;

      check_not_null(vm);
      check_int_eq(turbo_wasm3_vm_enable_sqlite_db(vm), 0);
      check_int_eq(turbo_wasm3_vm_enable_host(vm), 0);
      check_null(
          turbo_wasm3_vm_load_module_file(vm, WASM3_DB_CRUD_TEST_WASM_PATH,
                                          "db_crud_demo", &module));
      check_not_null(module);
      check_null(
          m3_FindFunction(&run, turbo_wasm3_vm_get_runtime(vm), "run_db_crud_demo"));
      check_not_null(run);
      check_null(m3_CallV(run));
      check_null(m3_GetResultsV(run, &result_value));
      check_int_eq((int)result_value, 11);

      turbo_wasm3_vm_destroy(vm);
    }
  }
#endif

#ifdef WASM3_REDIS_GUEST_AVAILABLE
  describe("guest redis module") {
    it("runs redis imports from a wasm guest through the TurboNet host abi") {
      wasm3_redis_mock_state_t state = {0};
      native_thread_t thread;
      turbo_wasm3_vm_t *vm = NULL;
      IM3Module module = NULL;
      IM3Function run = NULL;
      int32_t result_value = 0;

      ensure_native_sockets_ready();
      check_int_eq(native_thread_create(&thread, wasm3_redis_mock_thread, &state), 0);
      wait_mock_ready(&state.ready, &state.failed);
      check_int_eq(state.failed, 0);
      check_int_ne(state.port, 0);

      vm = turbo_wasm3_vm_create(64U * 1024U, NULL, 16);
      check_not_null(vm);
      check_int_eq(turbo_wasm3_vm_enable_redis_host(vm), 0);
      check_null(
          turbo_wasm3_vm_load_module_file(vm, WASM3_REDIS_TEST_WASM_PATH,
                                          "redis_demo", &module));
      check_not_null(module);
      check_null(
          m3_FindFunction(&run, turbo_wasm3_vm_get_runtime(vm), "run_redis_demo"));
      check_not_null(run);
      check_null(m3_CallV(run, (int32_t)state.port));
      check_null(m3_GetResultsV(run, &result_value));
      check_int_eq((int)result_value, 23);

      turbo_wasm3_vm_destroy(vm);
      native_thread_join(&thread);
      check_int_eq(state.failed, 0);
    }
  }
#endif

#ifdef WASM3_HTTP_TEST_WASM_PATH
  describe("guest http module") {
    it("runs http imports from a wasm guest through the TurboNet host abi") {
      turbo_wasm3_vm_t *vm = turbo_wasm3_vm_create(64U * 1024U, NULL, 16);
      IM3Module module = NULL;
      IM3Function run = NULL;
      int32_t result_value = 0;

      check_not_null(vm);
      check_int_eq(turbo_wasm3_vm_enable_http_host(vm), 0);
      check_null(
          turbo_wasm3_vm_load_module_file(vm, WASM3_HTTP_TEST_WASM_PATH,
                                          "http_demo", &module));
      check_not_null(module);
      check_null(m3_FindFunction(&run, turbo_wasm3_vm_get_runtime(vm), "run_http_demo"));
      check_not_null(run);
      check_null(m3_CallV(run));
      check_null(m3_GetResultsV(run, &result_value));
      check_int_eq((int)result_value, 17);

      turbo_wasm3_vm_destroy(vm);
    }
  }
#endif

#ifdef WASM3_PARSER_GUEST_AVAILABLE
  describe("guest parser module") {
    it("runs parser imports from a wasm guest through the TurboNet host abi") {
      turbo_wasm3_vm_t *vm = turbo_wasm3_vm_create(64U * 1024U, NULL, 16);
      IM3Module module = NULL;
      IM3Function json_run = NULL;
      IM3Function csv_run = NULL;
      IM3Function xml_run = NULL;
      IM3Function ini_run = NULL;
      int32_t result_value = 0;

      check_not_null(vm);
      check_int_eq(turbo_wasm3_vm_enable_host(vm), 0);
      check_null(
          turbo_wasm3_vm_load_module_file(vm, WASM3_PARSER_TEST_WASM_PATH,
                                          "parser_demo", &module));
      check_not_null(module);

      check_null(
          m3_FindFunction(&json_run, turbo_wasm3_vm_get_runtime(vm), "run_json_demo"));
      check_not_null(json_run);
      check_null(m3_CallV(json_run));
      check_null(m3_GetResultsV(json_run, &result_value));
      check_int_eq((int)result_value, 7);

      check_null(
          m3_FindFunction(&csv_run, turbo_wasm3_vm_get_runtime(vm), "run_csv_demo"));
      check_not_null(csv_run);
      check_null(m3_CallV(csv_run));
      check_null(m3_GetResultsV(csv_run, &result_value));
      check_int_eq((int)result_value, 7);

      check_null(
          m3_FindFunction(&xml_run, turbo_wasm3_vm_get_runtime(vm), "run_xml_demo"));
      check_not_null(xml_run);
      check_null(m3_CallV(xml_run));
      check_null(m3_GetResultsV(xml_run, &result_value));
      check_int_eq((int)result_value, 7);

      check_null(
          m3_FindFunction(&ini_run, turbo_wasm3_vm_get_runtime(vm), "run_ini_demo"));
      check_not_null(ini_run);
      check_null(m3_CallV(ini_run));
      check_null(m3_GetResultsV(ini_run, &result_value));
      check_int_eq((int)result_value, 7);

      turbo_wasm3_vm_destroy(vm);
    }
  }
#endif

  describe("per-vm wasi state") {
    it("keeps preopen mappings isolated between VMs") {
      turbo_wasm3_vm_t *vm_a = NULL;
      turbo_wasm3_vm_t *vm_b = NULL;
      IM3Function start_a = NULL;
      IM3Function start_b = NULL;
      char capture_a[TURBO_FS_MAX_PATH];
      char capture_b[TURBO_FS_MAX_PATH];
      char output_a[4096];
      char output_b[4096];

      check_int_eq(make_fixture_dir(g_fixture_a, sizeof(g_fixture_a), "One\n"), 0);
      check_int_eq(make_fixture_dir(g_fixture_b, sizeof(g_fixture_b), "Two\n"), 0);
      check_int_eq(turbo_fs_path_join(capture_a, sizeof(capture_a), g_fixture_a,
                                      "stdout.txt"),
                   0);
      check_int_eq(turbo_fs_path_join(capture_b, sizeof(capture_b), g_fixture_b,
                                      "stdout.txt"),
                   0);

      vm_a = turbo_wasm3_vm_create(64U * 1024U, NULL, 16);
      vm_b = turbo_wasm3_vm_create(64U * 1024U, NULL, 16);

      check_not_null(vm_a);
      check_not_null(vm_b);
      check_null(configure_cat_vm(vm_a, g_fixture_a, &start_a));
      check_null(configure_cat_vm(vm_b, g_fixture_b, &start_b));
      check_null(call_start_capture(vm_a, start_a, capture_a, output_a,
                                    sizeof(output_a)));
      check_null(call_start_capture(vm_b, start_b, capture_b, output_b,
                                    sizeof(output_b)));
      check_str_contains(output_a, "4f 6e 65 0a");
      check_str_contains(output_b, "54 77 6f 0a");

      turbo_wasm3_vm_destroy(vm_a);
      turbo_wasm3_vm_destroy(vm_b);
    }
  }

  describe("coronet socket provider") {
    it("bridges registered CoroNet sockets through WASI socket ops") {
      wasm3_socket_bridge_state_t state;
      coro_context_t *ctx = coro_context_create(NULL);
      m3_wasi_context_t *wasi = m3_NewWasiContext();
      turbo_wasm3_socket_registry_t *registry =
          turbo_wasm3_socket_registry_create(4);

      memset(&state, 0, sizeof(state));
      state.ctx = ctx;
      state.wasi = wasi;
      state.registry = registry;
      state.unregister_rc = -1;

      check_not_null(ctx);
      check_not_null(wasi);
      check_not_null(registry);
      check_int_eq(turbo_wasm3_socket_registry_bind_wasi(registry, wasi), 0);
      check_int_eq(coro_context_spawn(ctx, wasm3_socket_bridge_task, &state), 0);

      run_context_until(ctx, &state.finished, 2000);

      check_int_eq(state.finished, 1);
      check_int_eq(state.stage, 13);
      check_int_eq(state.task_rc, 0);
      check_int_eq(state.lookup_matches, 1);
      check_int_eq((int)state.send_err, 0);
      check_int_eq((int)state.sent_len, 4);
      check_int_eq((int)state.recv_err, 0);
      check_int_eq((int)state.recv_len, 4);
      check_int_eq((int)state.recv_flags, 0);
      check_str_eq(state.recv_buf, "ping");
      check_int_ne((int)state.shutdown_err, 0);
      check_int_eq(state.unregister_rc, 0);
      check_null(turbo_wasm3_socket_registry_lookup(registry, state.wasi_fd));
      check_int_ne((int)state.recv_after_unregister_err, 0);
      check_int_eq(turbo_wasm3_socket_registry_unbind_wasi(wasi), 0);
      check_null(m3_wasi_context_get_socket_ops(wasi, NULL));

      turbo_wasm3_socket_registry_destroy(registry);
      m3_FreeWasiContext(wasi);
      drain_context_destroy(ctx);
    }
  }
}
