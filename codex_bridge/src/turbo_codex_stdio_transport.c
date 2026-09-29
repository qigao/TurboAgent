#include "turbo_codex_bridge.h"

#include <stdlib.h>
#include <string.h>

#include <tstr.h>
#include <salts/clock.h>
#include <salts/thread.h>

#if defined(_WIN32)
#include <windows.h>
#elif !defined(__ANDROID__)
#include <errno.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

enum {
  TURBO_CODEX_STDIO_DEFAULT_MAX_LINE_BYTES = 4 * 1024 * 1024,
  TURBO_CODEX_STDIO_READ_CHUNK_BYTES = 8192,
  TURBO_CODEX_STDIO_CLOSE_TIMEOUT_MS = 2000,
  TURBO_CODEX_STDIO_POLL_INTERVAL_MS = 5
};

typedef struct turbo_codex_stdio_s {
  size_t max_line_bytes;
  tstr input;
  int closed;
#if defined(_WIN32)
  HANDLE process;
  HANDLE input_write;
  HANDLE output_read;
#elif !defined(__ANDROID__)
  pid_t process;
  int input_write;
  int output_read;
#endif
} turbo_codex_stdio_t;

void turbo_codex_stdio_transport_config_init(turbo_codex_stdio_transport_config_t *config) {
  if (!config) return;
  memset(config, 0, sizeof(*config));
  config->struct_size = sizeof(*config);
  config->abi_version = TURBO_CODEX_STDIO_TRANSPORT_ABI_VERSION;
  config->codex_executable = "codex";
  config->max_line_bytes = TURBO_CODEX_STDIO_DEFAULT_MAX_LINE_BYTES;
}

static int turbo_codex_stdio_take_line(turbo_codex_stdio_t *stdio_transport, char **out_line) {
  char *newline;
  size_t line_length;
  size_t consumed;
  size_t remaining;
  char *line;
  if (!stdio_transport || !out_line) return SALTS_EINVAL;
  *out_line = NULL;
  if (!stdio_transport->input) return SALTS_ENOENT;
  newline = (char *)memchr(stdio_transport->input, '\n', tstr_len(stdio_transport->input));
  if (!newline) return SALTS_ENOENT;
  line_length = (size_t)(newline - stdio_transport->input);
  if (line_length && stdio_transport->input[line_length - 1] == '\r') --line_length;
  if (line_length > stdio_transport->max_line_bytes) return SALTS_EMSGSIZE;
  line = (char *)malloc(line_length + 1);
  if (!line) return SALTS_ENOMEM;
  memcpy(line, stdio_transport->input, line_length);
  line[line_length] = '\0';
  consumed = (size_t)(newline - stdio_transport->input) + 1;
  remaining = tstr_len(stdio_transport->input) - consumed;
  memmove(stdio_transport->input, stdio_transport->input + consumed, remaining);
  stdio_transport->input[remaining] = '\0';
  if (tstr_set_len_checked(stdio_transport->input, remaining) != SALTS_OK) {
    free(line);
    return SALTS_EIO;
  }
  *out_line = line;
  return SALTS_OK;
}

static int turbo_codex_stdio_append(turbo_codex_stdio_t *stdio_transport,
                                    const uint8_t *data, size_t length) {
  tstr appended;
  if (!stdio_transport || (!data && length)) return SALTS_EINVAL;
  if (length > stdio_transport->max_line_bytes + 1 ||
      tstr_len(stdio_transport->input) > stdio_transport->max_line_bytes + 1 - length) {
    return SALTS_EMSGSIZE;
  }
  appended = tstr_cat_len(stdio_transport->input, (const char *)data, length);
  if (!appended) return SALTS_ENOMEM;
  stdio_transport->input = appended;
  if (tstr_len(stdio_transport->input) > stdio_transport->max_line_bytes &&
      !memchr(stdio_transport->input, '\n', tstr_len(stdio_transport->input))) {
    return SALTS_EMSGSIZE;
  }
  return SALTS_OK;
}

#if defined(_WIN32)

static int turbo_codex_windows_command_safe(const char *text) {
  return text && text[0] && !strpbrk(text, "\"\r\n&|<>^%!");
}

static int turbo_codex_windows_is_executable(const char *path) {
  size_t length = path ? strlen(path) : 0;
  return length >= 4 && !_stricmp(path + length - 4, ".exe");
}

static void turbo_codex_stdio_close_impl(void *user_data) {
  turbo_codex_stdio_t *stdio_transport = (turbo_codex_stdio_t *)user_data;
  if (!stdio_transport || stdio_transport->closed) return;
  stdio_transport->closed = 1;
  if (stdio_transport->input_write) {
    CloseHandle(stdio_transport->input_write);
    stdio_transport->input_write = NULL;
  }
  if (stdio_transport->process) {
    DWORD wait_status =
        WaitForSingleObject(stdio_transport->process, TURBO_CODEX_STDIO_CLOSE_TIMEOUT_MS);
    if (wait_status == WAIT_TIMEOUT) {
      TerminateProcess(stdio_transport->process, 1);
      (void)WaitForSingleObject(stdio_transport->process,
                                TURBO_CODEX_STDIO_CLOSE_TIMEOUT_MS);
    }
    CloseHandle(stdio_transport->process);
    stdio_transport->process = NULL;
  }
  if (stdio_transport->output_read) {
    CloseHandle(stdio_transport->output_read);
    stdio_transport->output_read = NULL;
  }
}

static void turbo_codex_stdio_free_impl(void *user_data) {
  turbo_codex_stdio_t *stdio_transport = (turbo_codex_stdio_t *)user_data;
  if (!stdio_transport) return;
  turbo_codex_stdio_close_impl(stdio_transport);
  tstr_free(stdio_transport->input);
  free(stdio_transport);
}

static int turbo_codex_stdio_write_impl(const uint8_t *frame, size_t frame_size,
                                        void *user_data) {
  turbo_codex_stdio_t *stdio_transport = (turbo_codex_stdio_t *)user_data;
  size_t offset = 0;
  if (!stdio_transport || stdio_transport->closed || !frame || !frame_size ||
      frame[frame_size - 1] != '\n') {
    return SALTS_EINVAL;
  }
  while (offset < frame_size) {
    DWORD written = 0;
    DWORD request = (DWORD)((frame_size - offset) > MAXDWORD ? MAXDWORD : frame_size - offset);
    if (!WriteFile(stdio_transport->input_write, frame + offset, request, &written, NULL) ||
        !written) {
      return SALTS_ESHUTDOWN;
    }
    offset += written;
  }
  return SALTS_OK;
}

static int turbo_codex_stdio_read_impl(uint64_t timeout_ms, char **out_line, void *user_data) {
  turbo_codex_stdio_t *stdio_transport = (turbo_codex_stdio_t *)user_data;
  uint64_t deadline = UINT64_MAX;
  uint8_t chunk[TURBO_CODEX_STDIO_READ_CHUNK_BYTES];
  int rc;
  if (!stdio_transport || !out_line || stdio_transport->closed) return SALTS_EINVAL;
  *out_line = NULL;
  rc = turbo_codex_stdio_take_line(stdio_transport, out_line);
  if (rc != SALTS_ENOENT) return rc;
  if (timeout_ms != UINT64_MAX) {
    uint64_t now = salts_monotonic_ms();
    deadline = timeout_ms > UINT64_MAX - now ? UINT64_MAX : now + timeout_ms;
  }
  for (;;) {
    DWORD available = 0;
    if (!PeekNamedPipe(stdio_transport->output_read, NULL, 0, NULL, &available, NULL)) {
      return GetLastError() == ERROR_BROKEN_PIPE ? SALTS_ESHUTDOWN : SALTS_EIO;
    }
    if (available) {
      DWORD read_size = 0;
      DWORD request = available > sizeof(chunk) ? (DWORD)sizeof(chunk) : available;
      if (!ReadFile(stdio_transport->output_read, chunk, request, &read_size, NULL) || !read_size) {
        return SALTS_ESHUTDOWN;
      }
      rc = turbo_codex_stdio_append(stdio_transport, chunk, read_size);
      if (rc != SALTS_OK) return rc;
      rc = turbo_codex_stdio_take_line(stdio_transport, out_line);
      if (rc != SALTS_ENOENT) return rc;
      continue;
    }
    if (WaitForSingleObject(stdio_transport->process, 0) == WAIT_OBJECT_0) {
      return tstr_len(stdio_transport->input) ? SALTS_EPROTO : SALTS_ESHUTDOWN;
    }
    if (timeout_ms == 0 || (deadline != UINT64_MAX && salts_monotonic_ms() >= deadline)) {
      return SALTS_ETIMEDOUT;
    }
    salts_sleep_ms(TURBO_CODEX_STDIO_POLL_INTERVAL_MS);
  }
}

static int turbo_codex_stdio_start(const turbo_codex_stdio_transport_config_t *config,
                                   turbo_codex_stdio_t *stdio_transport) {
  SECURITY_ATTRIBUTES attributes;
  STARTUPINFOA startup;
  PROCESS_INFORMATION process;
  HANDLE child_input_read = NULL;
  HANDLE child_output_write = NULL;
  tstr command = NULL;
  BOOL created;
  int use_shell;
  int rc = SALTS_EIO;
  if (!turbo_codex_windows_command_safe(config->codex_executable)) return SALTS_EINVAL;
  memset(&attributes, 0, sizeof(attributes));
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;
  memset(&startup, 0, sizeof(startup));
  startup.cb = sizeof(startup);
  memset(&process, 0, sizeof(process));
  if (!CreatePipe(&child_input_read, &stdio_transport->input_write, &attributes, 0) ||
      !SetHandleInformation(stdio_transport->input_write, HANDLE_FLAG_INHERIT, 0) ||
      !CreatePipe(&stdio_transport->output_read, &child_output_write, &attributes, 0) ||
      !SetHandleInformation(stdio_transport->output_read, HANDLE_FLAG_INHERIT, 0)) {
    goto cleanup;
  }
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = child_input_read;
  startup.hStdOutput = child_output_write;
  startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  use_shell = !turbo_codex_windows_is_executable(config->codex_executable);
  command = tstr_dup(use_shell ? "cmd.exe /d /s /c \"\"" : "\"");
  if (command) command = tstr_cat(command, config->codex_executable);
  if (command) command = tstr_cat(command, use_shell ? "\" app-server --stdio\"" : "\" app-server --stdio");
  if (!command) {
    rc = SALTS_ENOMEM;
    goto cleanup;
  }
  created = CreateProcessA(use_shell ? NULL : config->codex_executable, command, NULL, NULL, TRUE,
                           CREATE_NO_WINDOW, NULL, config->working_directory, &startup, &process);
  if (!created) goto cleanup;
  CloseHandle(process.hThread);
  stdio_transport->process = process.hProcess;
  rc = SALTS_OK;

cleanup:
  if (child_input_read) CloseHandle(child_input_read);
  if (child_output_write) CloseHandle(child_output_write);
  tstr_free(command);
  if (rc != SALTS_OK) turbo_codex_stdio_close_impl(stdio_transport);
  return rc;
}

#elif !defined(__ANDROID__)

static void turbo_codex_stdio_close_impl(void *user_data) {
  turbo_codex_stdio_t *stdio_transport = (turbo_codex_stdio_t *)user_data;
  uint64_t deadline;
  int status;
  if (!stdio_transport || stdio_transport->closed) return;
  stdio_transport->closed = 1;
  if (stdio_transport->input_write >= 0) {
    close(stdio_transport->input_write);
    stdio_transport->input_write = -1;
  }
  if (stdio_transport->process > 0) {
    deadline = salts_monotonic_ms() + TURBO_CODEX_STDIO_CLOSE_TIMEOUT_MS;
    while (waitpid(stdio_transport->process, &status, WNOHANG) == 0 &&
           salts_monotonic_ms() < deadline) {
      salts_sleep_ms(TURBO_CODEX_STDIO_POLL_INTERVAL_MS);
    }
    if (waitpid(stdio_transport->process, &status, WNOHANG) == 0) {
      kill(stdio_transport->process, SIGTERM);
      deadline = salts_monotonic_ms() + TURBO_CODEX_STDIO_CLOSE_TIMEOUT_MS;
      while (waitpid(stdio_transport->process, &status, WNOHANG) == 0 &&
             salts_monotonic_ms() < deadline) {
        salts_sleep_ms(TURBO_CODEX_STDIO_POLL_INTERVAL_MS);
      }
      if (waitpid(stdio_transport->process, &status, WNOHANG) == 0) {
        kill(stdio_transport->process, SIGKILL);
        (void)waitpid(stdio_transport->process, &status, 0);
      }
    }
    stdio_transport->process = -1;
  }
  if (stdio_transport->output_read >= 0) {
    close(stdio_transport->output_read);
    stdio_transport->output_read = -1;
  }
}

static void turbo_codex_stdio_free_impl(void *user_data) {
  turbo_codex_stdio_t *stdio_transport = (turbo_codex_stdio_t *)user_data;
  if (!stdio_transport) return;
  turbo_codex_stdio_close_impl(stdio_transport);
  tstr_free(stdio_transport->input);
  free(stdio_transport);
}

static int turbo_codex_stdio_write_impl(const uint8_t *frame, size_t frame_size,
                                        void *user_data) {
  turbo_codex_stdio_t *stdio_transport = (turbo_codex_stdio_t *)user_data;
  size_t offset = 0;
  if (!stdio_transport || stdio_transport->closed || !frame || !frame_size ||
      frame[frame_size - 1] != '\n') {
    return SALTS_EINVAL;
  }
  while (offset < frame_size) {
    ssize_t written = write(stdio_transport->input_write, frame + offset, frame_size - offset);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) return SALTS_ESHUTDOWN;
    offset += (size_t)written;
  }
  return SALTS_OK;
}

static int turbo_codex_stdio_read_impl(uint64_t timeout_ms, char **out_line, void *user_data) {
  turbo_codex_stdio_t *stdio_transport = (turbo_codex_stdio_t *)user_data;
  uint8_t chunk[TURBO_CODEX_STDIO_READ_CHUNK_BYTES];
  fd_set read_set;
  struct timeval timeout;
  struct timeval *timeout_ptr;
  uint64_t remaining = timeout_ms;
  uint64_t deadline = UINT64_MAX;
  int selected;
  int rc;
  if (!stdio_transport || !out_line || stdio_transport->closed) return SALTS_EINVAL;
  *out_line = NULL;
  rc = turbo_codex_stdio_take_line(stdio_transport, out_line);
  if (rc != SALTS_ENOENT) return rc;
  if (timeout_ms != UINT64_MAX) {
    uint64_t now = salts_monotonic_ms();
    deadline = timeout_ms > UINT64_MAX - now ? UINT64_MAX : now + timeout_ms;
  }
  for (;;) {
    FD_ZERO(&read_set);
    FD_SET(stdio_transport->output_read, &read_set);
    timeout_ptr = NULL;
    if (remaining != UINT64_MAX) {
      timeout.tv_sec = (long)(remaining / 1000);
      timeout.tv_usec = (long)((remaining % 1000) * 1000);
      timeout_ptr = &timeout;
    }
    selected = select(stdio_transport->output_read + 1, &read_set, NULL, NULL, timeout_ptr);
    if (selected < 0 && errno == EINTR) {
      if (deadline != UINT64_MAX) {
        uint64_t now = salts_monotonic_ms();
        if (now >= deadline) return SALTS_ETIMEDOUT;
        remaining = deadline - now;
      }
      continue;
    }
    if (selected < 0) return SALTS_EIO;
    if (selected == 0) return SALTS_ETIMEDOUT;
    {
      ssize_t read_size = read(stdio_transport->output_read, chunk, sizeof(chunk));
      if (read_size < 0 && errno == EINTR) continue;
      if (read_size <= 0) {
        return tstr_len(stdio_transport->input) ? SALTS_EPROTO : SALTS_ESHUTDOWN;
      }
      rc = turbo_codex_stdio_append(stdio_transport, chunk, (size_t)read_size);
      if (rc != SALTS_OK) return rc;
      rc = turbo_codex_stdio_take_line(stdio_transport, out_line);
      if (rc != SALTS_ENOENT) return rc;
    }
    if (deadline != UINT64_MAX) {
      uint64_t now = salts_monotonic_ms();
      if (now >= deadline) return SALTS_ETIMEDOUT;
      remaining = deadline - now;
    }
  }
}

static int turbo_codex_stdio_start(const turbo_codex_stdio_transport_config_t *config,
                                   turbo_codex_stdio_t *stdio_transport) {
  int input_pipe[2] = {-1, -1};
  int output_pipe[2] = {-1, -1};
  pid_t child;
  if (pipe(input_pipe) != 0 || pipe(output_pipe) != 0) {
    if (input_pipe[0] >= 0) close(input_pipe[0]);
    if (input_pipe[1] >= 0) close(input_pipe[1]);
    if (output_pipe[0] >= 0) close(output_pipe[0]);
    if (output_pipe[1] >= 0) close(output_pipe[1]);
    return SALTS_EIO;
  }
  child = fork();
  if (child < 0) {
    close(input_pipe[0]);
    close(input_pipe[1]);
    close(output_pipe[0]);
    close(output_pipe[1]);
    return SALTS_EIO;
  }
  if (child == 0) {
    (void)dup2(input_pipe[0], STDIN_FILENO);
    (void)dup2(output_pipe[1], STDOUT_FILENO);
    close(input_pipe[0]);
    close(input_pipe[1]);
    close(output_pipe[0]);
    close(output_pipe[1]);
    if (config->working_directory && chdir(config->working_directory) != 0) _exit(126);
    execlp(config->codex_executable, config->codex_executable, "app-server", "--stdio",
           (char *)NULL);
    _exit(127);
  }
  close(input_pipe[0]);
  close(output_pipe[1]);
  stdio_transport->process = child;
  stdio_transport->input_write = input_pipe[1];
  stdio_transport->output_read = output_pipe[0];
  return SALTS_OK;
}

#endif

int turbo_codex_stdio_transport_create(const turbo_codex_stdio_transport_config_t *config,
                                       turbo_codex_transport_t *out_transport) {
#if defined(__ANDROID__)
  (void)config;
  if (out_transport) memset(out_transport, 0, sizeof(*out_transport));
  return SALTS_ENOTSUP;
#else
  turbo_codex_stdio_transport_config_t defaults;
  const turbo_codex_stdio_transport_config_t *effective = config;
  turbo_codex_stdio_t *stdio_transport;
  int rc;
  if (out_transport) memset(out_transport, 0, sizeof(*out_transport));
  if (!out_transport) return SALTS_EINVAL;
  if (!effective) {
    turbo_codex_stdio_transport_config_init(&defaults);
    effective = &defaults;
  }
  if (effective->struct_size < sizeof(*effective) ||
      effective->abi_version != TURBO_CODEX_STDIO_TRANSPORT_ABI_VERSION ||
      !effective->codex_executable || !effective->codex_executable[0] ||
      !effective->max_line_bytes) {
    return SALTS_EINVAL;
  }
  stdio_transport = (turbo_codex_stdio_t *)calloc(1, sizeof(*stdio_transport));
  if (!stdio_transport) return SALTS_ENOMEM;
  stdio_transport->max_line_bytes = effective->max_line_bytes;
  stdio_transport->input = tstr_new();
#if !defined(_WIN32)
  stdio_transport->process = -1;
  stdio_transport->input_write = -1;
  stdio_transport->output_read = -1;
#endif
  if (!stdio_transport->input) {
    free(stdio_transport);
    return SALTS_ENOMEM;
  }
  rc = turbo_codex_stdio_start(effective, stdio_transport);
  if (rc != SALTS_OK) {
    turbo_codex_stdio_free_impl(stdio_transport);
    return rc;
  }
  out_transport->struct_size = sizeof(*out_transport);
  out_transport->abi_version = TURBO_CODEX_TRANSPORT_ABI_VERSION;
  out_transport->write = turbo_codex_stdio_write_impl;
  out_transport->read = turbo_codex_stdio_read_impl;
  out_transport->close = turbo_codex_stdio_close_impl;
  out_transport->user_data = stdio_transport;
  out_transport->user_data_free = turbo_codex_stdio_free_impl;
  return SALTS_OK;
#endif
}
