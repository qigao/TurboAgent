#ifndef TURBO_CODING_TOOLS_H
#define TURBO_CODING_TOOLS_H

#include <stddef.h>

#include <turbo_agent_api.h>

#include "turbo_tool_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TURBO_CODING_TOOLS_CONFIG_ABI_VERSION 1u

typedef struct turbo_coding_tools_config_s {
  size_t struct_size;
  unsigned int abi_version;
  /** Absolute workspace root, admitted as a retained directory capability. */
  const char *workspace_root;
  size_t max_read_bytes;       /**< File content bytes, before JSON escaping. */
  size_t max_list_entries;     /**< List entries; also bounds search traversal/matches. */
  size_t max_result_bytes;     /**< Content/name payload bytes, excluding JSON framing. */
} turbo_coding_tools_config_t;

/** Fill bounded defaults; workspace_root remains unset. */
CXX_C_API void turbo_coding_tools_config_init(turbo_coding_tools_config_t *config);

/**
 * Register `fs.read`, `fs.list`, and `fs.search` atomically as parallel-safe,
 * read-only tools. Search takes required path/pattern strings and returns
 * bounded literal line matches; see coding_tools/README.md for limits/skips.
 *
 * Paths must be relative to workspace_root. Absolute paths, dot segments,
 * drive/stream separators, and symlink/reparse components are rejected.
 * fs.list and fs.search additionally accept "." for the admitted root itself.
 * Invalid/missing roots fail registration. Callers must quiesce invocations
 * before destroying the registry and its retained root capabilities.
 * Context-aware execution borrows the caller's cancellation token/deadline and
 * checks between I/O operations. Cancellation/deadline returns CANCELLED or
 * DEADLINE_EXCEEDED with NULL output; blocking OS calls are not interrupted.
 * fs.read/fs.list legacy callbacks remain available without execution control.
 */
CXX_C_API turbo_tool_status_t turbo_coding_tools_add_read_only(
    turbo_tool_registry_t *registry, const turbo_coding_tools_config_t *config);

#define TURBO_CODING_WORKSPACE_CONFIG_ABI_VERSION 1u

typedef struct turbo_coding_workspace_config_s {
  size_t struct_size;
  unsigned int abi_version;
  turbo_coding_tools_config_t files;
  size_t max_write_bytes;
  size_t max_command_bytes;
  size_t max_process_output_bytes;
  uint64_t process_timeout_ms;
} turbo_coding_workspace_config_t;

/** Initialize bounded defaults; files.workspace_root must be set by the caller. */
CXX_C_API void turbo_coding_workspace_config_init(turbo_coding_workspace_config_t *config);

/**
 * Register fs.read, fs.list, fs.search, fs.write, fs.edit, shell.exec atomically.
 * Configuration is copied. The registry owns admitted filesystem roots until
 * destruction; registration/destruction require no in-flight calls.
 * Writes/edits require the Agent "patch" capability; shell.exec requires "shell".
 * These three tools are exclusive and non-idempotent. Direct registry execution
 * is a trusted host API; it does not enforce Agent policy.
 *
 * Files use workspace-relative paths and reject symlinks/reparse components.
 * Edits replace exactly one nonempty old_text occurrence. Writes publish via
 * atomic rename, without crash-durability or external-writer CAS guarantees.
 * Edits check cancellation/deadline between read chunks and after matching;
 * interrupted reads discard partial text and preserve the target file.
 * shell.exec uses the platform shell in the workspace with host privileges and
 * inherited environment; cwd is not a sandbox. Output and execution time are
 * bounded. Cancellation terminates/reaps the owned process tree.
 * After in-flight cancellation/deadline, an optional execution-context detail
 * sink borrows a result with partial UTF-8 output and process status; copy it
 * during the callback to retain it. Normal output stays NULL on non-OK status.
 * Pre-spawn cancellation has no process detail. Termination does not undo effects.
 *
 * Returns OK, INVALID_ARGUMENT, DUPLICATE, OUT_OF_MEMORY, or ERROR; failed
 * registration removes only tools added by this call. Existing tools survive.
 * Example: init, set files.workspace_root, then add_workspace(registry, &config).
 */
CXX_C_API turbo_tool_status_t turbo_coding_tools_add_workspace(
    turbo_tool_registry_t *registry, const turbo_coding_workspace_config_t *config);

#ifdef __cplusplus
}
#endif

#endif
