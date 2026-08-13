#ifndef TURBO_CODING_TOOLS_H
#define TURBO_CODING_TOOLS_H

#include <platform.h>

#include "turbo_tool_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TURBO_CODING_TOOLS_CONFIG_ABI_VERSION 1u

typedef struct turbo_coding_tools_config_s {
  size_t struct_size;
  unsigned int abi_version;
  /** Absolute workspace root. The pack copies this string. */
  const char *workspace_root;
  size_t max_read_bytes;
  size_t max_list_entries;
  size_t max_result_bytes;
} turbo_coding_tools_config_t;

/** Fill bounded defaults; workspace_root remains unset. */
CXX_C_API void turbo_coding_tools_config_init(turbo_coding_tools_config_t *config);

/**
 * Register `fs.read` and `fs.list` as parallel-safe, read-only tools.
 *
 * Paths must be relative to workspace_root. Absolute paths, dot segments,
 * drive/stream separators, and symlink/reparse components are rejected.
 */
CXX_C_API turbo_tool_status_t turbo_coding_tools_add_read_only(
    turbo_tool_registry_t *registry, const turbo_coding_tools_config_t *config);

#ifdef __cplusplus
}
#endif

#endif
