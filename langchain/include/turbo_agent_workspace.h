#ifndef TURBO_AGENT_WORKSPACE_H
#define TURBO_AGENT_WORKSPACE_H

#include <platform.h>

#include "turbo_agent.h"
#include "turbo_agent_policy.h"
#include "turbo_tool_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TURBO_AGENT_WORKSPACE_CONFIG_ABI_VERSION 1u

typedef struct turbo_agent_workspace_s turbo_agent_workspace_t;
typedef struct turbo_agent_workspace_selection_s turbo_agent_workspace_selection_t;

typedef struct turbo_agent_workspace_tool_capability_s {
  const char *tool_name;
  turbo_agent_policy_capability_t capability;
} turbo_agent_workspace_tool_capability_t;

typedef enum turbo_agent_workspace_status_e {
  TURBO_AGENT_WORKSPACE_OK = 0,
  TURBO_AGENT_WORKSPACE_INVALID_ARGUMENT = -1,
  TURBO_AGENT_WORKSPACE_IO_ERROR = -2,
  TURBO_AGENT_WORKSPACE_PARSE_ERROR = -3,
  TURBO_AGENT_WORKSPACE_LIMIT_EXCEEDED = -4,
  TURBO_AGENT_WORKSPACE_TOOL_NOT_FOUND = -5,
  TURBO_AGENT_WORKSPACE_CAPABILITY_DENIED = -6,
  TURBO_AGENT_WORKSPACE_OUT_OF_MEMORY = -7
} turbo_agent_workspace_status_t;

/**
 * Workspace instruction and skill discovery configuration.
 *
 * `workspace_root` must be an existing absolute directory.
 * `working_directory` and `skills_directory` are workspace-relative paths;
 * absolute paths, `.`/`..` components, drive/stream separators, and symlink
 * traversal are rejected. NULL working_directory means the workspace root.
 * NULL or `skills` means the default catalog directory; a missing default
 * directory is an empty catalog. A missing non-default directory is an error.
 *
 * The policy and string/array values are copied during create. The workspace
 * object is single-threaded; selections are immutable after prepare.
 */
typedef struct turbo_agent_workspace_config_s {
  size_t struct_size;
  unsigned int abi_version;
  const char *workspace_root;
  const char *working_directory;
  const char *skills_directory;
  const char *agents_filename;
  const char *const *always_tools;
  size_t always_tool_count;
  const turbo_agent_workspace_tool_capability_t *tool_capabilities;
  size_t tool_capability_count;
  const turbo_agent_policy_t *policy;
  size_t max_instruction_bytes;
  size_t max_skill_bytes;
  size_t max_skills;
  size_t max_selected_skills;
  size_t max_projected_tools;
  size_t max_scan_depth;
} turbo_agent_workspace_config_t;

/** Populate bounded, compatibility-preserving defaults. */
CXX_C_API void turbo_agent_workspace_config_init(turbo_agent_workspace_config_t *config);

/** Discover hierarchical AGENTS.md files and build the skill catalog. */
CXX_C_API turbo_agent_workspace_status_t turbo_agent_workspace_create(
    const turbo_agent_workspace_config_t *config, turbo_agent_workspace_t **out_workspace);

CXX_C_API void turbo_agent_workspace_destroy(turbo_agent_workspace_t *workspace);

/** Re-read AGENTS.md and skill files atomically; old state survives failure. */
CXX_C_API turbo_agent_workspace_status_t
turbo_agent_workspace_refresh(turbo_agent_workspace_t *workspace);

CXX_C_API size_t turbo_agent_workspace_skill_count(const turbo_agent_workspace_t *workspace);

/** Last status and diagnostic produced by create/refresh/prepare. */
CXX_C_API turbo_agent_workspace_status_t
turbo_agent_workspace_last_status(const turbo_agent_workspace_t *workspace);
CXX_C_API const char *turbo_agent_workspace_last_error(const turbo_agent_workspace_t *workspace);

/**
 * Select task-relevant skills, validate their capabilities and tools, compose
 * effective instructions, and build a non-owning tool registry projection.
 *
 * Modern skills use YAML frontmatter fields `name`, `description`, `triggers`,
 * `tools`, and `capabilities`. TurboParser is the only YAML/JSON value source.
 * Direct `skills/*.md` files without frontmatter remain instruction-only and
 * are selected by filename/name. Skill selection never executes code.
 *
 * The source registry and its callback dependencies must outlive the returned
 * selection and any agent created from it.
 */
CXX_C_API turbo_agent_workspace_status_t turbo_agent_workspace_prepare(
    turbo_agent_workspace_t *workspace, const char *task,
    const turbo_tool_registry_t *source_registry, const char *base_instructions,
    turbo_agent_workspace_selection_t **out_selection);

CXX_C_API void
turbo_agent_workspace_selection_destroy(turbo_agent_workspace_selection_t *selection);

CXX_C_API const char *
turbo_agent_workspace_selection_instructions(const turbo_agent_workspace_selection_t *selection);
CXX_C_API const turbo_tool_registry_t *
turbo_agent_workspace_selection_tools(const turbo_agent_workspace_selection_t *selection);
CXX_C_API size_t
turbo_agent_workspace_selection_skill_count(const turbo_agent_workspace_selection_t *selection);
CXX_C_API const char *
turbo_agent_workspace_selection_skill_name(const turbo_agent_workspace_selection_t *selection,
                                           size_t index);
CXX_C_API size_t
turbo_agent_workspace_selection_tool_count(const turbo_agent_workspace_selection_t *selection);
CXX_C_API const char *
turbo_agent_workspace_selection_tool_name(const turbo_agent_workspace_selection_t *selection,
                                          size_t index);

/**
 * Prepare one task and create an agent using its effective instructions and
 * projected tools. The returned agent owns the immutable selection; the source
 * registry supplied in `config->tool_registry` remains borrowed and must
 * outlive the agent.
 */
CXX_C_API turbo_agent_t *turbo_agent_create_for_workspace(const turbo_agent_config_t *config,
                                                          turbo_agent_workspace_t *workspace,
                                                          const char *task);

#ifdef __cplusplus
}
#endif

#endif
