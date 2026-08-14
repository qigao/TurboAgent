#include "turbo_agent_workspace.h"

#include "turbo_agent_lifecycle_internal.h"

#include <turbo_error.h>
#include <turbo_fs.h>
#include <turbo_parser.h>
#include <turbo_str.h>
#include <turbo_vec.h>

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TURBO_AGENT_WORKSPACE_DEFAULT_AGENTS_FILE "AGENTS.md"
#define TURBO_AGENT_WORKSPACE_DEFAULT_SKILLS_DIR "skills"
#define TURBO_AGENT_WORKSPACE_DEFAULT_MAX_INSTRUCTION_BYTES (256u * 1024u)
#define TURBO_AGENT_WORKSPACE_DEFAULT_MAX_SKILL_BYTES (128u * 1024u)
#define TURBO_AGENT_WORKSPACE_DEFAULT_MAX_SKILLS 128u
#define TURBO_AGENT_WORKSPACE_DEFAULT_MAX_SELECTED_SKILLS 16u
#define TURBO_AGENT_WORKSPACE_DEFAULT_MAX_PROJECTED_TOOLS 64u
#define TURBO_AGENT_WORKSPACE_DEFAULT_MAX_SCAN_DEPTH 4u
#define TURBO_AGENT_WORKSPACE_ERROR_BYTES 256u

typedef struct turbo_agent_skill_s {
  tstr_t name;
  tstr_t description;
  tstr_t path;
  tstr_t body;
  turbo_vec_t triggers;
  turbo_vec_t tools;
  turbo_vec_t capabilities;
} turbo_agent_skill_t;

typedef struct turbo_agent_workspace_tool_capability_entry_s {
  tstr_t tool_name;
  turbo_agent_policy_capability_t capability;
} turbo_agent_workspace_tool_capability_entry_t;

struct turbo_agent_workspace_s {
  tstr_t workspace_root;
  tstr_t working_directory;
  tstr_t skills_directory;
  tstr_t agents_filename;
  turbo_vec_t always_tools;
  turbo_vec_t tool_capabilities;
  turbo_agent_policy_t policy;
  size_t max_instruction_bytes;
  size_t max_skill_bytes;
  size_t max_skills;
  size_t max_selected_skills;
  size_t max_projected_tools;
  size_t max_scan_depth;
  int skills_directory_explicit;
  tstr_t agent_instructions;
  turbo_vec_t skills;
  turbo_agent_workspace_status_t last_status;
  char last_error[TURBO_AGENT_WORKSPACE_ERROR_BYTES];
};

struct turbo_agent_workspace_selection_s {
  tstr_t instructions;
  turbo_tool_registry_t *tools;
  turbo_vec_t skill_names;
  turbo_vec_t tool_names;
};

static char *turbo_agent_workspace_strdup(const char *text) {
  size_t len;
  char *copy;
  if (!text) return NULL;
  len = strlen(text);
  if (len == SIZE_MAX) return NULL;
  copy = (char *)malloc(len + 1);
  if (!copy) return NULL;
  memcpy(copy, text, len + 1);
  return copy;
}

static void turbo_agent_workspace_set_error(turbo_agent_workspace_t *workspace,
                                            turbo_agent_workspace_status_t status,
                                            const char *operation, const char *detail) {
  if (!workspace) return;
  workspace->last_status = status;
  if (!operation) operation = "workspace";
  if (!detail) detail = "failed";
  snprintf(workspace->last_error, sizeof(workspace->last_error), "%s: %s", operation, detail);
}

static int turbo_agent_workspace_vec_init(turbo_vec_t *vec, size_t elem_size) {
  return turbo_vec_init(vec, elem_size) == TURBO_OK ? 0 : -1;
}

static void turbo_agent_workspace_string_vec_destroy(turbo_vec_t *vec) {
  size_t index;
  if (!vec) return;
  for (index = 0; index < turbo_vec_size(vec); ++index) {
    char **value = (char **)turbo_vec_at(vec, index);
    if (value) free(*value);
  }
  turbo_vec_destroy(vec);
}

static int turbo_agent_workspace_string_vec_contains(const turbo_vec_t *vec, const char *text) {
  size_t index;
  if (!vec || !text) return 0;
  for (index = 0; index < turbo_vec_size(vec); ++index) {
    char *const *value = (char *const *)turbo_vec_at((turbo_vec_t *)vec, index);
    if (value && *value && strcmp(*value, text) == 0) return 1;
  }
  return 0;
}

static int turbo_agent_workspace_string_vec_push(turbo_vec_t *vec, const char *text, int unique) {
  char *copy;
  if (!vec || !text || text[0] == '\0') return -1;
  if (unique && turbo_agent_workspace_string_vec_contains(vec, text)) return 0;
  copy = turbo_agent_workspace_strdup(text);
  if (!copy) return -1;
  if (turbo_vec_push(vec, &copy) != TURBO_OK) {
    free(copy);
    return -1;
  }
  return 0;
}

static void turbo_agent_skill_destroy(turbo_agent_skill_t *skill) {
  if (!skill) return;
  tstr_free(skill->name);
  tstr_free(skill->description);
  tstr_free(skill->path);
  tstr_free(skill->body);
  turbo_agent_workspace_string_vec_destroy(&skill->triggers);
  turbo_agent_workspace_string_vec_destroy(&skill->tools);
  turbo_agent_workspace_string_vec_destroy(&skill->capabilities);
  memset(skill, 0, sizeof(*skill));
}

static void turbo_agent_workspace_skill_vec_destroy(turbo_vec_t *skills) {
  size_t index;
  if (!skills) return;
  for (index = 0; index < turbo_vec_size(skills); ++index) {
    turbo_agent_skill_t *skill = (turbo_agent_skill_t *)turbo_vec_at(skills, index);
    turbo_agent_skill_destroy(skill);
  }
  turbo_vec_destroy(skills);
}

static void turbo_agent_workspace_tool_capability_vec_destroy(turbo_vec_t *entries) {
  size_t index;
  if (!entries) return;
  for (index = 0; index < turbo_vec_size(entries); ++index) {
    turbo_agent_workspace_tool_capability_entry_t *entry =
        (turbo_agent_workspace_tool_capability_entry_t *)turbo_vec_at(entries, index);
    if (entry) tstr_free(entry->tool_name);
  }
  turbo_vec_destroy(entries);
}

static int turbo_agent_workspace_ascii_equal_ci(const char *left, const char *right) {
  if (!left || !right) return 0;
  while (*left && *right) {
    if (tolower((unsigned char)*left) != tolower((unsigned char)*right)) return 0;
    ++left;
    ++right;
  }
  return *left == '\0' && *right == '\0';
}

static int turbo_agent_workspace_ascii_contains_ci(const char *text, const char *needle) {
  size_t needle_len;
  size_t index;
  if (!text || !needle) return 0;
  needle_len = strlen(needle);
  if (needle_len == 0) return 1;
  for (index = 0; text[index] != '\0'; ++index) {
    size_t offset = 0;
    while (offset < needle_len && text[index + offset] != '\0' &&
           tolower((unsigned char)text[index + offset]) == tolower((unsigned char)needle[offset])) {
      ++offset;
    }
    if (offset == needle_len) return 1;
  }
  return 0;
}

static int turbo_agent_workspace_relative_path_valid(const char *path, int allow_empty) {
  const char *component;
  const char *cursor;
  if (!path || path[0] == '\0') return allow_empty;
  if (turbo_fs_path_is_absolute(path) || strchr(path, ':')) return 0;
  component = path;
  for (cursor = path;; ++cursor) {
    if (*cursor == '/' || *cursor == '\\' || *cursor == '\0') {
      size_t len = (size_t)(cursor - component);
      if (len == 0 || (len == 1 && component[0] == '.') ||
          (len == 2 && component[0] == '.' && component[1] == '.'))
        return 0;
      if (*cursor == '\0') break;
      component = cursor + 1;
    }
  }
  return 1;
}

static int turbo_agent_workspace_filename_valid(const char *name) {
  return name && name[0] != '\0' && !strchr(name, '/') && !strchr(name, '\\') &&
         !strchr(name, ':') && strcmp(name, ".") != 0 && strcmp(name, "..") != 0;
}

static tstr_t turbo_agent_workspace_path_join(const char *base, const char *relative) {
  tstr_t result;
  if (!base) return NULL;
  result = tstr_dup(base);
  if (!result) return NULL;
  if (relative && relative[0] != '\0') {
    size_t len = tstr_len(result);
    if (len > 0 && result[len - 1] != '/' && result[len - 1] != '\\') {
      tstr_t grown = tstr_cat(result, "/");
      if (!grown) {
        tstr_free(result);
        return NULL;
      }
      result = grown;
    }
    {
      tstr_t grown = tstr_cat(result, relative);
      if (!grown) {
        tstr_free(result);
        return NULL;
      }
      result = grown;
    }
  }
  return result;
}

static turbo_agent_workspace_status_t
turbo_agent_workspace_append_bounded(turbo_agent_workspace_t *workspace, tstr_t *target,
                                     const char *text, size_t len, size_t limit,
                                     const char *operation) {
  tstr_t grown;
  size_t current;
  if (!target || (!text && len > 0)) return TURBO_AGENT_WORKSPACE_INVALID_ARGUMENT;
  current = *target ? tstr_len(*target) : 0;
  if (len > limit || current > limit - len) {
    turbo_agent_workspace_set_error(workspace, TURBO_AGENT_WORKSPACE_LIMIT_EXCEEDED, operation,
                                    "configured byte limit exceeded");
    return TURBO_AGENT_WORKSPACE_LIMIT_EXCEEDED;
  }
  if (len == 0) return TURBO_AGENT_WORKSPACE_OK;
  if (!*target) {
    *target = tstr_new();
    if (!*target) {
      turbo_agent_workspace_set_error(workspace, TURBO_AGENT_WORKSPACE_OUT_OF_MEMORY, operation,
                                      "allocation failed");
      return TURBO_AGENT_WORKSPACE_OUT_OF_MEMORY;
    }
  }
  grown = tstr_cat_len(*target, text, len);
  if (!grown) {
    turbo_agent_workspace_set_error(workspace, TURBO_AGENT_WORKSPACE_OUT_OF_MEMORY, operation,
                                    "allocation failed");
    return TURBO_AGENT_WORKSPACE_OUT_OF_MEMORY;
  }
  *target = grown;
  return TURBO_AGENT_WORKSPACE_OK;
}

static turbo_agent_workspace_status_t
turbo_agent_workspace_read_file(turbo_agent_workspace_t *workspace, const char *path, size_t limit,
                                tstr_t *out_text) {
  turbo_fs_stat_t metadata;
  turbo_fs_buf_t buffer = {0};
  int rc;
  if (!workspace || !path || !out_text) return TURBO_AGENT_WORKSPACE_INVALID_ARGUMENT;
  *out_text = NULL;
  rc = turbo_fs_lstat(path, &metadata);
  if (rc != 0) {
    turbo_agent_workspace_set_error(workspace, TURBO_AGENT_WORKSPACE_IO_ERROR, "read", path);
    return TURBO_AGENT_WORKSPACE_IO_ERROR;
  }
  if (!metadata.is_file || metadata.is_symlink || metadata.size > (uint64_t)limit) {
    turbo_agent_workspace_set_error(workspace,
                                    metadata.size > (uint64_t)limit
                                        ? TURBO_AGENT_WORKSPACE_LIMIT_EXCEEDED
                                        : TURBO_AGENT_WORKSPACE_IO_ERROR,
                                    "read", path);
    return workspace->last_status;
  }
  if (turbo_fs_read_file(path, &buffer) != 0 || buffer.len > limit) {
    turbo_fs_buf_free(&buffer);
    turbo_agent_workspace_set_error(workspace, TURBO_AGENT_WORKSPACE_IO_ERROR, "read", path);
    return TURBO_AGENT_WORKSPACE_IO_ERROR;
  }
  *out_text = tstr_dup_len(buffer.base, buffer.len);
  turbo_fs_buf_free(&buffer);
  if (!*out_text) {
    turbo_agent_workspace_set_error(workspace, TURBO_AGENT_WORKSPACE_OUT_OF_MEMORY, "read", path);
    return TURBO_AGENT_WORKSPACE_OUT_OF_MEMORY;
  }
  return TURBO_AGENT_WORKSPACE_OK;
}

static turbo_agent_workspace_status_t
turbo_agent_workspace_append_agents_file(turbo_agent_workspace_t *workspace, tstr_t *instructions,
                                         const char *directory, const char *scope) {
  tstr_t path = turbo_agent_workspace_path_join(directory, workspace->agents_filename);
  tstr_t content = NULL;
  turbo_fs_stat_t metadata;
  turbo_agent_workspace_status_t status;
  if (!path) return TURBO_AGENT_WORKSPACE_OUT_OF_MEMORY;
  if (turbo_fs_access(path, TURBO_FS_ACCESS_EXISTS) != 0) {
    tstr_free(path);
    return TURBO_AGENT_WORKSPACE_OK;
  }
  if (turbo_fs_lstat(path, &metadata) != 0) {
    tstr_free(path);
    return TURBO_AGENT_WORKSPACE_OK;
  }
  if (!metadata.is_file || metadata.is_symlink) {
    turbo_agent_workspace_set_error(workspace, TURBO_AGENT_WORKSPACE_IO_ERROR, "agents instruction",
                                    path);
    tstr_free(path);
    return TURBO_AGENT_WORKSPACE_IO_ERROR;
  }
  status =
      turbo_agent_workspace_read_file(workspace, path, workspace->max_instruction_bytes, &content);
  if (status == TURBO_AGENT_WORKSPACE_OK) {
    static const char prefix[] = "\n\n# Workspace instructions: ";
    status = turbo_agent_workspace_append_bounded(
        workspace, instructions, prefix, sizeof(prefix) - 1, workspace->max_instruction_bytes,
        "agents instructions");
    if (status == TURBO_AGENT_WORKSPACE_OK)
      status = turbo_agent_workspace_append_bounded(workspace, instructions, scope, strlen(scope),
                                                    workspace->max_instruction_bytes,
                                                    "agents instructions");
    if (status == TURBO_AGENT_WORKSPACE_OK)
      status = turbo_agent_workspace_append_bounded(workspace, instructions, "\n", 1,
                                                    workspace->max_instruction_bytes,
                                                    "agents instructions");
    if (status == TURBO_AGENT_WORKSPACE_OK)
      status = turbo_agent_workspace_append_bounded(
          workspace, instructions, content, tstr_len(content), workspace->max_instruction_bytes,
          "agents instructions");
  }
  tstr_free(content);
  tstr_free(path);
  return status;
}

static turbo_agent_workspace_status_t
turbo_agent_workspace_load_agents(turbo_agent_workspace_t *workspace, tstr_t *out_instructions) {
  tstr_t current;
  const char *cursor;
  const char *component;
  turbo_agent_workspace_status_t status;
  *out_instructions = tstr_new();
  if (!*out_instructions) return TURBO_AGENT_WORKSPACE_OUT_OF_MEMORY;
  current = tstr_dup(workspace->workspace_root);
  if (!current) return TURBO_AGENT_WORKSPACE_OUT_OF_MEMORY;
  status = turbo_agent_workspace_append_agents_file(workspace, out_instructions, current, ".");
  if (status != TURBO_AGENT_WORKSPACE_OK) {
    tstr_free(current);
    return status;
  }
  if (!workspace->working_directory || workspace->working_directory[0] == '\0') {
    tstr_free(current);
    return TURBO_AGENT_WORKSPACE_OK;
  }
  component = workspace->working_directory;
  for (cursor = workspace->working_directory;; ++cursor) {
    if (*cursor == '/' || *cursor == '\\' || *cursor == '\0') {
      size_t len = (size_t)(cursor - component);
      turbo_fs_stat_t metadata;
      tstr_t next = tstr_dup_len(component, len);
      tstr_t scope;
      tstr_t grown;
      if (!next) {
        tstr_free(current);
        return TURBO_AGENT_WORKSPACE_OUT_OF_MEMORY;
      }
      grown = turbo_agent_workspace_path_join(current, next);
      tstr_free(next);
      if (!grown) {
        tstr_free(current);
        return TURBO_AGENT_WORKSPACE_OUT_OF_MEMORY;
      }
      tstr_free(current);
      current = grown;
      if (turbo_fs_lstat(current, &metadata) != 0 || !metadata.is_directory ||
          metadata.is_symlink) {
        turbo_agent_workspace_set_error(workspace, TURBO_AGENT_WORKSPACE_IO_ERROR,
                                        "working directory", current);
        tstr_free(current);
        return TURBO_AGENT_WORKSPACE_IO_ERROR;
      }
      scope = tstr_dup_len(workspace->working_directory,
                           (size_t)(cursor - workspace->working_directory));
      if (!scope) {
        tstr_free(current);
        return TURBO_AGENT_WORKSPACE_OUT_OF_MEMORY;
      }
      status =
          turbo_agent_workspace_append_agents_file(workspace, out_instructions, current, scope);
      tstr_free(scope);
      if (status != TURBO_AGENT_WORKSPACE_OK) {
        tstr_free(current);
        return status;
      }
      if (*cursor == '\0') break;
      component = cursor + 1;
    }
  }
  tstr_free(current);
  return TURBO_AGENT_WORKSPACE_OK;
}

static int turbo_agent_workspace_markdown_candidate(const char *name, size_t depth) {
  size_t len;
  if (!name) return 0;
  if (turbo_agent_workspace_ascii_equal_ci(name, "SKILL.md")) return 1;
  if (depth != 0 || turbo_agent_workspace_ascii_equal_ci(name, "README.md")) return 0;
  len = strlen(name);
  return len > 3 && turbo_agent_workspace_ascii_equal_ci(name + len - 3, ".md");
}

static turbo_agent_workspace_status_t
turbo_agent_workspace_scan_skill_paths(turbo_agent_workspace_t *workspace, const char *directory,
                                       size_t depth, turbo_vec_t *paths) {
  turbo_fs_dir_t *dir = NULL;
  turbo_fs_dirent_t entry;
  int read_status;
  if (depth > workspace->max_scan_depth) {
    turbo_agent_workspace_set_error(workspace, TURBO_AGENT_WORKSPACE_LIMIT_EXCEEDED, "skill scan",
                                    "directory depth exceeded");
    return TURBO_AGENT_WORKSPACE_LIMIT_EXCEEDED;
  }
  if (turbo_fs_opendir(directory, &dir) != 0) {
    turbo_agent_workspace_set_error(workspace, TURBO_AGENT_WORKSPACE_IO_ERROR, "skill scan",
                                    directory);
    return TURBO_AGENT_WORKSPACE_IO_ERROR;
  }
  while ((read_status = turbo_fs_readdir(dir, &entry)) > 0) {
    tstr_t path = turbo_agent_workspace_path_join(directory, entry.name);
    turbo_fs_stat_t metadata;
    turbo_agent_workspace_status_t status = TURBO_AGENT_WORKSPACE_OK;
    if (!path) {
      turbo_fs_closedir(dir);
      return TURBO_AGENT_WORKSPACE_OUT_OF_MEMORY;
    }
    if (turbo_fs_lstat(path, &metadata) != 0 || metadata.is_symlink) {
      turbo_agent_workspace_set_error(workspace, TURBO_AGENT_WORKSPACE_IO_ERROR, "skill scan",
                                      path);
      tstr_free(path);
      turbo_fs_closedir(dir);
      return TURBO_AGENT_WORKSPACE_IO_ERROR;
    }
    if (metadata.is_directory) {
      status = turbo_agent_workspace_scan_skill_paths(workspace, path, depth + 1, paths);
    } else if (metadata.is_file && turbo_agent_workspace_markdown_candidate(entry.name, depth)) {
      if (turbo_vec_size(paths) >= workspace->max_skills) {
        turbo_agent_workspace_set_error(workspace, TURBO_AGENT_WORKSPACE_LIMIT_EXCEEDED,
                                        "skill scan", "skill count exceeded");
        status = TURBO_AGENT_WORKSPACE_LIMIT_EXCEEDED;
      } else if (turbo_agent_workspace_string_vec_push(paths, path, 1) != 0) {
        status = TURBO_AGENT_WORKSPACE_OUT_OF_MEMORY;
      }
    }
    tstr_free(path);
    if (status != TURBO_AGENT_WORKSPACE_OK) {
      turbo_fs_closedir(dir);
      return status;
    }
  }
  turbo_fs_closedir(dir);
  if (read_status < 0) {
    turbo_agent_workspace_set_error(workspace, TURBO_AGENT_WORKSPACE_IO_ERROR, "skill scan",
                                    directory);
    return TURBO_AGENT_WORKSPACE_IO_ERROR;
  }
  return TURBO_AGENT_WORKSPACE_OK;
}

static int turbo_agent_workspace_path_compare(const void *left, const void *right) {
  const char *const *left_path = (const char *const *)left;
  const char *const *right_path = (const char *const *)right;
  return strcmp(*left_path, *right_path);
}

static tstr_t turbo_agent_workspace_skill_fallback_name(const char *path) {
  char basename[TURBO_FS_MAX_PATH];
  char dirname[TURBO_FS_MAX_PATH];
  size_t len;
  if (turbo_fs_path_basename(path, basename, sizeof(basename)) != 0) return NULL;
  if (turbo_agent_workspace_ascii_equal_ci(basename, "SKILL.md")) {
    if (turbo_fs_path_dirname(path, dirname, sizeof(dirname)) != 0 ||
        turbo_fs_path_basename(dirname, basename, sizeof(basename)) != 0)
      return NULL;
  } else {
    len = strlen(basename);
    if (len > 3 && turbo_agent_workspace_ascii_equal_ci(basename + len - 3, ".md"))
      basename[len - 3] = '\0';
  }
  return tstr_dup(basename);
}

static int turbo_agent_workspace_frontmatter(const char *text, size_t len, size_t *yaml_begin,
                                             size_t *yaml_len, size_t *body_begin) {
  size_t pos;
  if (!text || len < 4 || strncmp(text, "---", 3) != 0 || (text[3] != '\n' && text[3] != '\r'))
    return 0;
  pos = text[3] == '\r' && len > 4 && text[4] == '\n' ? 5 : 4;
  *yaml_begin = pos;
  while (pos < len) {
    size_t line_start = pos;
    size_t line_end;
    while (pos < len && text[pos] != '\n' && text[pos] != '\r')
      ++pos;
    line_end = pos;
    if (line_end - line_start == 3 && strncmp(text + line_start, "---", 3) == 0) {
      *yaml_len = line_start - *yaml_begin;
      if (pos < len && text[pos] == '\r') ++pos;
      if (pos < len && text[pos] == '\n') ++pos;
      *body_begin = pos;
      return 1;
    }
    if (pos < len && text[pos] == '\r') ++pos;
    if (pos < len && text[pos] == '\n') ++pos;
  }
  return -1;
}

static int turbo_agent_workspace_copy_string_array(const json_value_t *metadata, const char *key,
                                                   turbo_vec_t *out) {
  json_value_t *value = turbo_json_object_get(metadata, key);
  size_t index;
  if (!value) return 0;
  if (turbo_json_type(value) == TURBO_JSON_STRING)
    return turbo_agent_workspace_string_vec_push(out, turbo_json_string(value), 1);
  if (turbo_json_type(value) != TURBO_JSON_ARRAY) return -1;
  for (index = 0; index < turbo_json_array_size(value); ++index) {
    json_value_t *item = turbo_json_array_get(value, index);
    if (!item || turbo_json_type(item) != TURBO_JSON_STRING || !turbo_json_string(item)[0] ||
        turbo_agent_workspace_string_vec_push(out, turbo_json_string(item), 1) != 0)
      return -1;
  }
  return 0;
}

static turbo_agent_workspace_status_t
turbo_agent_workspace_parse_skill(turbo_agent_workspace_t *workspace, const char *path,
                                  turbo_agent_skill_t *out_skill) {
  tstr_t file = NULL;
  size_t yaml_begin = 0;
  size_t yaml_len = 0;
  size_t body_begin = 0;
  int frontmatter;
  turbo_agent_workspace_status_t status;
  memset(out_skill, 0, sizeof(*out_skill));
  if (turbo_agent_workspace_vec_init(&out_skill->triggers, sizeof(char *)) != 0 ||
      turbo_agent_workspace_vec_init(&out_skill->tools, sizeof(char *)) != 0 ||
      turbo_agent_workspace_vec_init(&out_skill->capabilities, sizeof(char *)) != 0) {
    turbo_agent_skill_destroy(out_skill);
    return TURBO_AGENT_WORKSPACE_OUT_OF_MEMORY;
  }
  status = turbo_agent_workspace_read_file(workspace, path, workspace->max_skill_bytes, &file);
  if (status != TURBO_AGENT_WORKSPACE_OK) goto cleanup;
  out_skill->path = tstr_dup(path);
  out_skill->name = turbo_agent_workspace_skill_fallback_name(path);
  if (!out_skill->path || !out_skill->name) {
    status = TURBO_AGENT_WORKSPACE_OUT_OF_MEMORY;
    goto cleanup;
  }
  frontmatter =
      turbo_agent_workspace_frontmatter(file, tstr_len(file), &yaml_begin, &yaml_len, &body_begin);
  if (frontmatter < 0) {
    turbo_agent_workspace_set_error(workspace, TURBO_AGENT_WORKSPACE_PARSE_ERROR,
                                    "skill frontmatter", path);
    status = TURBO_AGENT_WORKSPACE_PARSE_ERROR;
    goto cleanup;
  }
  if (frontmatter > 0) {
    turbo_yaml_doc_t *yaml = NULL;
    json_value_t *metadata = NULL;
    const char *name;
    const char *description;
    if (turbo_parse_yaml((const uint8_t *)file + yaml_begin, yaml_len, &yaml) != 0 || !yaml) {
      turbo_agent_workspace_set_error(workspace, TURBO_AGENT_WORKSPACE_PARSE_ERROR,
                                      "skill frontmatter", path);
      status = TURBO_AGENT_WORKSPACE_PARSE_ERROR;
      goto cleanup;
    }
    metadata = turbo_yaml_to_json(yaml);
    turbo_free_yaml(&yaml);
    if (!metadata || turbo_json_type(metadata) != TURBO_JSON_OBJECT) {
      turbo_free_json(&metadata);
      turbo_agent_workspace_set_error(workspace, TURBO_AGENT_WORKSPACE_PARSE_ERROR,
                                      "skill frontmatter", path);
      status = TURBO_AGENT_WORKSPACE_PARSE_ERROR;
      goto cleanup;
    }
    name = turbo_json_get_string(metadata, "name");
    description = turbo_json_get_string(metadata, "description");
    if (!name || !name[0] || !description || !description[0] ||
        turbo_agent_workspace_copy_string_array(metadata, "triggers", &out_skill->triggers) != 0 ||
        turbo_agent_workspace_copy_string_array(metadata, "tools", &out_skill->tools) != 0 ||
        turbo_agent_workspace_copy_string_array(metadata, "capabilities",
                                                &out_skill->capabilities) != 0) {
      turbo_free_json(&metadata);
      turbo_agent_workspace_set_error(workspace, TURBO_AGENT_WORKSPACE_PARSE_ERROR,
                                      "skill metadata", path);
      status = TURBO_AGENT_WORKSPACE_PARSE_ERROR;
      goto cleanup;
    }
    tstr_free(out_skill->name);
    out_skill->name = tstr_dup(name);
    out_skill->description = tstr_dup(description);
    turbo_free_json(&metadata);
    if (!out_skill->name || !out_skill->description) {
      status = TURBO_AGENT_WORKSPACE_OUT_OF_MEMORY;
      goto cleanup;
    }
  } else {
    body_begin = 0;
    out_skill->description = tstr_dup(out_skill->name);
    if (!out_skill->description) {
      status = TURBO_AGENT_WORKSPACE_OUT_OF_MEMORY;
      goto cleanup;
    }
  }
  out_skill->body = tstr_dup_len(file + body_begin, tstr_len(file) - body_begin);
  if (!out_skill->body ||
      turbo_agent_workspace_string_vec_push(&out_skill->triggers, out_skill->name, 1) != 0) {
    status = TURBO_AGENT_WORKSPACE_OUT_OF_MEMORY;
    goto cleanup;
  }
  status = TURBO_AGENT_WORKSPACE_OK;
cleanup:
  tstr_free(file);
  if (status != TURBO_AGENT_WORKSPACE_OK) turbo_agent_skill_destroy(out_skill);
  return status;
}

static turbo_agent_workspace_status_t
turbo_agent_workspace_load_skills(turbo_agent_workspace_t *workspace, turbo_vec_t *out_skills) {
  turbo_vec_t paths;
  tstr_t root;
  turbo_fs_stat_t metadata;
  turbo_agent_workspace_status_t status;
  size_t index;
  memset(&paths, 0, sizeof(paths));
  memset(out_skills, 0, sizeof(*out_skills));
  if (turbo_agent_workspace_vec_init(&paths, sizeof(char *)) != 0) {
    return TURBO_AGENT_WORKSPACE_OUT_OF_MEMORY;
  }
  if (turbo_agent_workspace_vec_init(out_skills, sizeof(turbo_agent_skill_t)) != 0) {
    turbo_agent_workspace_string_vec_destroy(&paths);
    return TURBO_AGENT_WORKSPACE_OUT_OF_MEMORY;
  }
  root = turbo_agent_workspace_path_join(workspace->workspace_root, workspace->skills_directory);
  if (!root) {
    status = TURBO_AGENT_WORKSPACE_OUT_OF_MEMORY;
    goto cleanup;
  }
  if (turbo_fs_access(root, TURBO_FS_ACCESS_EXISTS) != 0) {
    if (!workspace->skills_directory_explicit) {
      status = TURBO_AGENT_WORKSPACE_OK;
      goto cleanup;
    }
    turbo_agent_workspace_set_error(workspace, TURBO_AGENT_WORKSPACE_IO_ERROR, "skills directory",
                                    root);
    status = TURBO_AGENT_WORKSPACE_IO_ERROR;
    goto cleanup;
  }
  if (turbo_fs_lstat(root, &metadata) != 0) {
    turbo_agent_workspace_set_error(workspace, TURBO_AGENT_WORKSPACE_IO_ERROR, "skills directory",
                                    root);
    status = TURBO_AGENT_WORKSPACE_IO_ERROR;
    goto cleanup;
  }
  if (!metadata.is_directory || metadata.is_symlink) {
    turbo_agent_workspace_set_error(workspace, TURBO_AGENT_WORKSPACE_IO_ERROR, "skills directory",
                                    root);
    status = TURBO_AGENT_WORKSPACE_IO_ERROR;
    goto cleanup;
  }
  status = turbo_agent_workspace_scan_skill_paths(workspace, root, 0, &paths);
  if (status != TURBO_AGENT_WORKSPACE_OK) goto cleanup;
  if (turbo_vec_size(&paths) > 1)
    qsort(paths.data, turbo_vec_size(&paths), sizeof(char *), turbo_agent_workspace_path_compare);
  for (index = 0; index < turbo_vec_size(&paths); ++index) {
    char *const *path = (char *const *)turbo_vec_at(&paths, index);
    turbo_agent_skill_t skill;
    size_t previous;
    status = turbo_agent_workspace_parse_skill(workspace, *path, &skill);
    if (status != TURBO_AGENT_WORKSPACE_OK) goto cleanup;
    for (previous = 0; previous < turbo_vec_size(out_skills); ++previous) {
      turbo_agent_skill_t *existing = (turbo_agent_skill_t *)turbo_vec_at(out_skills, previous);
      if (turbo_agent_workspace_ascii_equal_ci(existing->name, skill.name)) {
        turbo_agent_workspace_set_error(workspace, TURBO_AGENT_WORKSPACE_PARSE_ERROR,
                                        "duplicate skill", skill.name);
        turbo_agent_skill_destroy(&skill);
        status = TURBO_AGENT_WORKSPACE_PARSE_ERROR;
        goto cleanup;
      }
    }
    if (turbo_vec_push(out_skills, &skill) != TURBO_OK) {
      turbo_agent_skill_destroy(&skill);
      status = TURBO_AGENT_WORKSPACE_OUT_OF_MEMORY;
      goto cleanup;
    }
  }
cleanup:
  tstr_free(root);
  turbo_agent_workspace_string_vec_destroy(&paths);
  if (status != TURBO_AGENT_WORKSPACE_OK) turbo_agent_workspace_skill_vec_destroy(out_skills);
  return status;
}

static int turbo_agent_workspace_skill_matches(const turbo_agent_skill_t *skill, const char *task) {
  size_t index;
  tstr_t marker;
  tstr_t reference;
  char basename[TURBO_FS_MAX_PATH];
  if (!skill || !task || task[0] == '\0') return 0;
  marker = tstr_dup("$");
  if (!marker) return 0;
  marker = tstr_cat(marker, skill->name);
  if (marker && turbo_agent_workspace_ascii_contains_ci(task, marker)) {
    tstr_free(marker);
    return 1;
  }
  tstr_free(marker);
  if (turbo_fs_path_basename(skill->path, basename, sizeof(basename)) == 0) {
    reference = tstr_dup("#skills/");
    if (!reference) return 0;
    reference = tstr_cat(reference, basename);
    if (reference && turbo_agent_workspace_ascii_contains_ci(task, reference)) {
      tstr_free(reference);
      return 1;
    }
    tstr_free(reference);
  }
  for (index = 0; index < turbo_vec_size(&skill->triggers); ++index) {
    char *const *trigger = (char *const *)turbo_vec_at((turbo_vec_t *)&skill->triggers, index);
    tstr_t normalized;
    size_t pos;
    if (!trigger || !*trigger || strlen(*trigger) < 3) continue;
    if (turbo_agent_workspace_ascii_contains_ci(task, *trigger)) return 1;
    normalized = tstr_dup(*trigger);
    if (!normalized) continue;
    for (pos = 0; pos < tstr_len(normalized); ++pos) {
      if (normalized[pos] == '_' || normalized[pos] == '-') normalized[pos] = ' ';
    }
    if (turbo_agent_workspace_ascii_contains_ci(task, normalized)) {
      tstr_free(normalized);
      return 1;
    }
    tstr_free(normalized);
  }
  return 0;
}

static int
turbo_agent_workspace_capability_from_text(const char *text,
                                           turbo_agent_policy_capability_t *out_capability) {
  if (turbo_agent_workspace_ascii_equal_ci(text, "custom_tools"))
    *out_capability = TURBO_AGENT_POLICY_CAPABILITY_CUSTOM_TOOLS;
  else if (turbo_agent_workspace_ascii_equal_ci(text, "runtime_tools") ||
           turbo_agent_workspace_ascii_equal_ci(text, "wasm"))
    *out_capability = TURBO_AGENT_POLICY_CAPABILITY_RUNTIME_TOOLS;
  else if (turbo_agent_workspace_ascii_equal_ci(text, "delegate"))
    *out_capability = TURBO_AGENT_POLICY_CAPABILITY_DELEGATE;
  else if (turbo_agent_workspace_ascii_equal_ci(text, "network"))
    *out_capability = TURBO_AGENT_POLICY_CAPABILITY_NETWORK;
  else if (turbo_agent_workspace_ascii_equal_ci(text, "shell"))
    *out_capability = TURBO_AGENT_POLICY_CAPABILITY_SHELL;
  else if (turbo_agent_workspace_ascii_equal_ci(text, "patch"))
    *out_capability = TURBO_AGENT_POLICY_CAPABILITY_PATCH;
  else if (turbo_agent_workspace_ascii_equal_ci(text, "outside_workspace"))
    *out_capability = TURBO_AGENT_POLICY_CAPABILITY_OUTSIDE_WORKSPACE;
  else return -1;
  return 0;
}

static turbo_agent_workspace_status_t
turbo_agent_workspace_check_capability(turbo_agent_workspace_t *workspace,
                                       turbo_agent_policy_capability_t capability,
                                       const char *detail);

static turbo_agent_workspace_status_t
turbo_agent_workspace_check_tool_capabilities(turbo_agent_workspace_t *workspace,
                                              const turbo_tool_registry_t *source_registry,
                                              const char *name) {
  const char *const *required_capabilities = NULL;
  size_t required_capability_count = 0;
  size_t index;
  int matched = 0;
  for (index = 0; index < turbo_vec_size(&workspace->tool_capabilities); ++index) {
    const turbo_agent_workspace_tool_capability_entry_t *entry =
        (const turbo_agent_workspace_tool_capability_entry_t *)turbo_vec_at_const(
            &workspace->tool_capabilities, index);
    turbo_agent_workspace_status_t status;
    if (!entry || strcmp(entry->tool_name, name) != 0) continue;
    matched = 1;
    status = turbo_agent_workspace_check_capability(workspace, entry->capability, name);
    if (status != TURBO_AGENT_WORKSPACE_OK) return status;
  }
  if (source_registry &&
      turbo_tool_registry_get_required_capabilities(source_registry, name, &required_capabilities,
                                                    &required_capability_count) == TURBO_TOOL_OK &&
      required_capability_count > 0) {
    for (index = 0; index < required_capability_count; ++index) {
      turbo_agent_policy_capability_t capability;
      if (turbo_agent_policy_capability_from_name(required_capabilities[index], &capability) != 0) {
        turbo_agent_workspace_set_error(workspace, TURBO_AGENT_WORKSPACE_PARSE_ERROR,
                                        "unknown tool capability", name);
        return TURBO_AGENT_WORKSPACE_PARSE_ERROR;
      }
      if (turbo_agent_workspace_check_capability(workspace, capability, name) !=
          TURBO_AGENT_WORKSPACE_OK) {
        return TURBO_AGENT_WORKSPACE_CAPABILITY_DENIED;
      }
    }
    return TURBO_AGENT_WORKSPACE_OK;
  }
  return matched ? TURBO_AGENT_WORKSPACE_OK
                 : turbo_agent_workspace_check_capability(
                       workspace, TURBO_AGENT_POLICY_CAPABILITY_CUSTOM_TOOLS, name);
}

static turbo_agent_workspace_status_t
turbo_agent_workspace_check_capability(turbo_agent_workspace_t *workspace,
                                       turbo_agent_policy_capability_t capability,
                                       const char *detail) {
  const char *reason = NULL;
  if (turbo_agent_policy_allows_capability(&workspace->policy, capability, &reason))
    return TURBO_AGENT_WORKSPACE_OK;
  turbo_agent_workspace_set_error(workspace, TURBO_AGENT_WORKSPACE_CAPABILITY_DENIED,
                                  reason ? reason : "capability denied", detail);
  return TURBO_AGENT_WORKSPACE_CAPABILITY_DENIED;
}

static turbo_agent_workspace_status_t
turbo_agent_workspace_add_selected_tool(turbo_agent_workspace_t *workspace,
                                        const turbo_tool_registry_t *source_registry,
                                        turbo_vec_t *tool_names, const char *tool_name) {
  turbo_agent_workspace_status_t status;
  if (turbo_agent_workspace_string_vec_contains(tool_names, tool_name))
    return TURBO_AGENT_WORKSPACE_OK;
  if (turbo_vec_size(tool_names) >= workspace->max_projected_tools) {
    turbo_agent_workspace_set_error(workspace, TURBO_AGENT_WORKSPACE_LIMIT_EXCEEDED,
                                    "tool projection", "tool count exceeded");
    return TURBO_AGENT_WORKSPACE_LIMIT_EXCEEDED;
  }
  status = turbo_agent_workspace_check_tool_capabilities(workspace, source_registry, tool_name);
  if (status != TURBO_AGENT_WORKSPACE_OK) return status;
  if (turbo_agent_workspace_string_vec_push(tool_names, tool_name, 1) != 0) {
    turbo_agent_workspace_set_error(workspace, TURBO_AGENT_WORKSPACE_OUT_OF_MEMORY,
                                    "tool projection", "allocation failed");
    return TURBO_AGENT_WORKSPACE_OUT_OF_MEMORY;
  }
  return TURBO_AGENT_WORKSPACE_OK;
}

static turbo_agent_workspace_status_t
turbo_agent_workspace_bind_projected_capabilities(turbo_agent_workspace_t *workspace,
                                                  turbo_agent_workspace_selection_t *selection) {
  size_t mapping_index;
  size_t tool_index;
  for (mapping_index = 0; mapping_index < turbo_vec_size(&workspace->tool_capabilities);
       ++mapping_index) {
    const turbo_agent_workspace_tool_capability_entry_t *entry =
        (const turbo_agent_workspace_tool_capability_entry_t *)turbo_vec_at_const(
            &workspace->tool_capabilities, mapping_index);
    const char *capability_name;
    turbo_tool_status_t tool_status;
    if (!entry ||
        !turbo_agent_workspace_string_vec_contains(&selection->tool_names, entry->tool_name)) {
      continue;
    }
    capability_name = turbo_agent_policy_capability_name(entry->capability);
    if (!capability_name) {
      turbo_agent_workspace_set_error(workspace, TURBO_AGENT_WORKSPACE_PARSE_ERROR,
                                      "unknown capability", entry->tool_name);
      return TURBO_AGENT_WORKSPACE_PARSE_ERROR;
    }
    tool_status =
        turbo_tool_registry_require_capability(selection->tools, entry->tool_name, capability_name);
    if (tool_status != TURBO_TOOL_OK) {
      turbo_agent_workspace_set_error(workspace, TURBO_AGENT_WORKSPACE_OUT_OF_MEMORY,
                                      "tool capability metadata", entry->tool_name);
      return TURBO_AGENT_WORKSPACE_OUT_OF_MEMORY;
    }
  }
  for (tool_index = 0; tool_index < turbo_vec_size(&selection->tool_names); ++tool_index) {
    char *const *tool_name = (char *const *)turbo_vec_at(&selection->tool_names, tool_index);
    const char *reason = NULL;
    if (turbo_agent_policy_check_tool(&workspace->policy, selection->tools, *tool_name, &reason) ==
        TURBO_AGENT_POLICY_ALLOW) {
      continue;
    }
    turbo_agent_workspace_set_error(workspace,
                                    reason && strcmp(reason, "unknown_tool_capability") == 0
                                        ? TURBO_AGENT_WORKSPACE_PARSE_ERROR
                                        : TURBO_AGENT_WORKSPACE_CAPABILITY_DENIED,
                                    reason ? reason : "capability denied", *tool_name);
    return workspace->last_status;
  }
  return TURBO_AGENT_WORKSPACE_OK;
}

void turbo_agent_workspace_config_init(turbo_agent_workspace_config_t *config) {
  if (!config) return;
  memset(config, 0, sizeof(*config));
  config->struct_size = sizeof(*config);
  config->abi_version = TURBO_AGENT_WORKSPACE_CONFIG_ABI_VERSION;
  config->skills_directory = TURBO_AGENT_WORKSPACE_DEFAULT_SKILLS_DIR;
  config->agents_filename = TURBO_AGENT_WORKSPACE_DEFAULT_AGENTS_FILE;
  config->max_instruction_bytes = TURBO_AGENT_WORKSPACE_DEFAULT_MAX_INSTRUCTION_BYTES;
  config->max_skill_bytes = TURBO_AGENT_WORKSPACE_DEFAULT_MAX_SKILL_BYTES;
  config->max_skills = TURBO_AGENT_WORKSPACE_DEFAULT_MAX_SKILLS;
  config->max_selected_skills = TURBO_AGENT_WORKSPACE_DEFAULT_MAX_SELECTED_SKILLS;
  config->max_projected_tools = TURBO_AGENT_WORKSPACE_DEFAULT_MAX_PROJECTED_TOOLS;
  config->max_scan_depth = TURBO_AGENT_WORKSPACE_DEFAULT_MAX_SCAN_DEPTH;
}

turbo_agent_workspace_status_t
turbo_agent_workspace_create(const turbo_agent_workspace_config_t *config,
                             turbo_agent_workspace_t **out_workspace) {
  turbo_agent_workspace_t *workspace;
  turbo_fs_stat_t root_metadata;
  const char *skills_directory;
  const char *agents_filename;
  size_t index;
  if (!out_workspace) return TURBO_AGENT_WORKSPACE_INVALID_ARGUMENT;
  *out_workspace = NULL;
  skills_directory = config && config->skills_directory ? config->skills_directory
                                                        : TURBO_AGENT_WORKSPACE_DEFAULT_SKILLS_DIR;
  agents_filename = config && config->agents_filename ? config->agents_filename
                                                      : TURBO_AGENT_WORKSPACE_DEFAULT_AGENTS_FILE;
  if (!config || config->struct_size < sizeof(*config) ||
      config->abi_version != TURBO_AGENT_WORKSPACE_CONFIG_ABI_VERSION || !config->workspace_root ||
      !turbo_fs_path_is_absolute(config->workspace_root) ||
      !turbo_agent_workspace_relative_path_valid(config->working_directory, 1) ||
      !turbo_agent_workspace_relative_path_valid(skills_directory, 0) ||
      !turbo_agent_workspace_filename_valid(agents_filename) ||
      config->max_instruction_bytes == 0 || config->max_skill_bytes == 0 ||
      config->max_skills == 0 || config->max_selected_skills == 0 ||
      config->max_projected_tools == 0 || config->max_scan_depth == 0 ||
      config->always_tool_count > config->max_projected_tools ||
      (config->always_tool_count > 0 && !config->always_tools) ||
      (config->tool_capability_count > 0 && !config->tool_capabilities) ||
      turbo_fs_lstat(config->workspace_root, &root_metadata) != 0 || !root_metadata.is_directory ||
      root_metadata.is_symlink) {
    return TURBO_AGENT_WORKSPACE_INVALID_ARGUMENT;
  }
  workspace = (turbo_agent_workspace_t *)calloc(1, sizeof(*workspace));
  if (!workspace) return TURBO_AGENT_WORKSPACE_OUT_OF_MEMORY;
  workspace->workspace_root = tstr_dup(config->workspace_root);
  workspace->working_directory =
      tstr_dup(config->working_directory ? config->working_directory : "");
  workspace->skills_directory = tstr_dup(skills_directory);
  workspace->agents_filename = tstr_dup(agents_filename);
  workspace->policy = config->policy ? *config->policy : turbo_agent_policy_default();
  workspace->policy.workspace_root = workspace->workspace_root;
  workspace->max_instruction_bytes = config->max_instruction_bytes;
  workspace->max_skill_bytes = config->max_skill_bytes;
  workspace->max_skills = config->max_skills;
  workspace->max_selected_skills = config->max_selected_skills;
  workspace->max_projected_tools = config->max_projected_tools;
  workspace->max_scan_depth = config->max_scan_depth;
  workspace->skills_directory_explicit =
      config->skills_directory != NULL &&
      strcmp(config->skills_directory, TURBO_AGENT_WORKSPACE_DEFAULT_SKILLS_DIR) != 0;
  if (!workspace->workspace_root || !workspace->working_directory || !workspace->skills_directory ||
      !workspace->agents_filename ||
      turbo_agent_workspace_vec_init(&workspace->always_tools, sizeof(char *)) != 0 ||
      turbo_agent_workspace_vec_init(&workspace->tool_capabilities,
                                     sizeof(turbo_agent_workspace_tool_capability_entry_t)) != 0 ||
      turbo_agent_workspace_vec_init(&workspace->skills, sizeof(turbo_agent_skill_t)) != 0) {
    turbo_agent_workspace_destroy(workspace);
    return TURBO_AGENT_WORKSPACE_OUT_OF_MEMORY;
  }
  for (index = 0; index < config->always_tool_count; ++index) {
    if (!config->always_tools[index] || !config->always_tools[index][0] ||
        turbo_agent_workspace_string_vec_push(&workspace->always_tools, config->always_tools[index],
                                              1) != 0) {
      turbo_agent_workspace_destroy(workspace);
      return TURBO_AGENT_WORKSPACE_INVALID_ARGUMENT;
    }
  }
  for (index = 0; index < config->tool_capability_count; ++index) {
    const turbo_agent_workspace_tool_capability_t *source = &config->tool_capabilities[index];
    turbo_agent_workspace_tool_capability_entry_t entry;
    size_t previous;
    memset(&entry, 0, sizeof(entry));
    if (!source->tool_name || !source->tool_name[0] ||
        source->capability < TURBO_AGENT_POLICY_CAPABILITY_CUSTOM_TOOLS ||
        source->capability > TURBO_AGENT_POLICY_CAPABILITY_OUTSIDE_WORKSPACE) {
      turbo_agent_workspace_destroy(workspace);
      return TURBO_AGENT_WORKSPACE_INVALID_ARGUMENT;
    }
    for (previous = 0; previous < turbo_vec_size(&workspace->tool_capabilities); ++previous) {
      const turbo_agent_workspace_tool_capability_entry_t *existing =
          (const turbo_agent_workspace_tool_capability_entry_t *)turbo_vec_at_const(
              &workspace->tool_capabilities, previous);
      if (existing && strcmp(existing->tool_name, source->tool_name) == 0 &&
          existing->capability == source->capability) {
        turbo_agent_workspace_destroy(workspace);
        return TURBO_AGENT_WORKSPACE_INVALID_ARGUMENT;
      }
    }
    entry.tool_name = tstr_dup(source->tool_name);
    entry.capability = source->capability;
    if (!entry.tool_name || turbo_vec_push(&workspace->tool_capabilities, &entry) != TURBO_OK) {
      tstr_free(entry.tool_name);
      turbo_agent_workspace_destroy(workspace);
      return TURBO_AGENT_WORKSPACE_OUT_OF_MEMORY;
    }
  }
  if (turbo_agent_workspace_refresh(workspace) != TURBO_AGENT_WORKSPACE_OK) {
    turbo_agent_workspace_status_t status = workspace->last_status;
    turbo_agent_workspace_destroy(workspace);
    return status;
  }
  *out_workspace = workspace;
  return TURBO_AGENT_WORKSPACE_OK;
}

void turbo_agent_workspace_destroy(turbo_agent_workspace_t *workspace) {
  if (!workspace) return;
  tstr_free(workspace->workspace_root);
  tstr_free(workspace->working_directory);
  tstr_free(workspace->skills_directory);
  tstr_free(workspace->agents_filename);
  tstr_free(workspace->agent_instructions);
  turbo_agent_workspace_string_vec_destroy(&workspace->always_tools);
  turbo_agent_workspace_tool_capability_vec_destroy(&workspace->tool_capabilities);
  turbo_agent_workspace_skill_vec_destroy(&workspace->skills);
  free(workspace);
}

turbo_agent_workspace_status_t turbo_agent_workspace_refresh(turbo_agent_workspace_t *workspace) {
  tstr_t instructions = NULL;
  turbo_vec_t skills;
  turbo_agent_workspace_status_t status;
  memset(&skills, 0, sizeof(skills));
  if (!workspace) return TURBO_AGENT_WORKSPACE_INVALID_ARGUMENT;
  status = turbo_agent_workspace_load_agents(workspace, &instructions);
  if (status != TURBO_AGENT_WORKSPACE_OK) {
    tstr_free(instructions);
    return status;
  }
  status = turbo_agent_workspace_load_skills(workspace, &skills);
  if (status != TURBO_AGENT_WORKSPACE_OK) {
    tstr_free(instructions);
    return status;
  }
  tstr_free(workspace->agent_instructions);
  turbo_agent_workspace_skill_vec_destroy(&workspace->skills);
  workspace->agent_instructions = instructions;
  workspace->skills = skills;
  workspace->last_status = TURBO_AGENT_WORKSPACE_OK;
  workspace->last_error[0] = '\0';
  return TURBO_AGENT_WORKSPACE_OK;
}

size_t turbo_agent_workspace_skill_count(const turbo_agent_workspace_t *workspace) {
  return workspace ? turbo_vec_size(&workspace->skills) : 0;
}

turbo_agent_workspace_status_t
turbo_agent_workspace_last_status(const turbo_agent_workspace_t *workspace) {
  return workspace ? workspace->last_status : TURBO_AGENT_WORKSPACE_INVALID_ARGUMENT;
}

const char *turbo_agent_workspace_last_error(const turbo_agent_workspace_t *workspace) {
  return workspace && workspace->last_error[0] ? workspace->last_error : NULL;
}

turbo_agent_workspace_status_t
turbo_agent_workspace_prepare(turbo_agent_workspace_t *workspace, const char *task,
                              const turbo_tool_registry_t *source_registry,
                              const char *base_instructions,
                              turbo_agent_workspace_selection_t **out_selection) {
  turbo_agent_workspace_selection_t *selection;
  turbo_vec_t selected;
  turbo_agent_workspace_status_t status = TURBO_AGENT_WORKSPACE_OK;
  size_t index;
  if (!out_selection) return TURBO_AGENT_WORKSPACE_INVALID_ARGUMENT;
  *out_selection = NULL;
  if (!workspace || !task || task[0] == '\0') return TURBO_AGENT_WORKSPACE_INVALID_ARGUMENT;
  selection = (turbo_agent_workspace_selection_t *)calloc(1, sizeof(*selection));
  if (!selection) return TURBO_AGENT_WORKSPACE_OUT_OF_MEMORY;
  memset(&selected, 0, sizeof(selected));
  if (turbo_agent_workspace_vec_init(&selected, sizeof(turbo_agent_skill_t *)) != 0 ||
      turbo_agent_workspace_vec_init(&selection->skill_names, sizeof(char *)) != 0 ||
      turbo_agent_workspace_vec_init(&selection->tool_names, sizeof(char *)) != 0) {
    status = TURBO_AGENT_WORKSPACE_OUT_OF_MEMORY;
    goto cleanup;
  }
  for (index = 0; index < turbo_vec_size(&workspace->skills); ++index) {
    turbo_agent_skill_t *skill = (turbo_agent_skill_t *)turbo_vec_at(&workspace->skills, index);
    if (!turbo_agent_workspace_skill_matches(skill, task)) continue;
    if (turbo_vec_size(&selected) >= workspace->max_selected_skills) {
      turbo_agent_workspace_set_error(workspace, TURBO_AGENT_WORKSPACE_LIMIT_EXCEEDED,
                                      "skill selection", "selected skill count exceeded");
      status = TURBO_AGENT_WORKSPACE_LIMIT_EXCEEDED;
      goto cleanup;
    }
    if (turbo_vec_push(&selected, &skill) != TURBO_OK ||
        turbo_agent_workspace_string_vec_push(&selection->skill_names, skill->name, 1) != 0) {
      status = TURBO_AGENT_WORKSPACE_OUT_OF_MEMORY;
      goto cleanup;
    }
  }
  if (base_instructions && base_instructions[0]) {
    status = turbo_agent_workspace_append_bounded(
        workspace, &selection->instructions, base_instructions, strlen(base_instructions),
        workspace->max_instruction_bytes, "effective instructions");
    if (status != TURBO_AGENT_WORKSPACE_OK) goto cleanup;
  }
  if (workspace->agent_instructions && tstr_len(workspace->agent_instructions) > 0) {
    status = turbo_agent_workspace_append_bounded(
        workspace, &selection->instructions, workspace->agent_instructions,
        tstr_len(workspace->agent_instructions), workspace->max_instruction_bytes,
        "effective instructions");
    if (status != TURBO_AGENT_WORKSPACE_OK) goto cleanup;
  }
  for (index = 0; index < turbo_vec_size(&workspace->always_tools); ++index) {
    char *const *tool = (char *const *)turbo_vec_at(&workspace->always_tools, index);
    status = turbo_agent_workspace_add_selected_tool(workspace, source_registry,
                                                     &selection->tool_names, *tool);
    if (status != TURBO_AGENT_WORKSPACE_OK) goto cleanup;
  }
  for (index = 0; index < turbo_vec_size(&selected); ++index) {
    turbo_agent_skill_t **skill_ptr = (turbo_agent_skill_t **)turbo_vec_at(&selected, index);
    turbo_agent_skill_t *skill = *skill_ptr;
    size_t item;
    static const char prefix[] = "\n\n# Skill: ";
    for (item = 0; item < turbo_vec_size(&skill->capabilities); ++item) {
      char *const *text = (char *const *)turbo_vec_at(&skill->capabilities, item);
      turbo_agent_policy_capability_t capability;
      if (turbo_agent_workspace_capability_from_text(*text, &capability) != 0) {
        turbo_agent_workspace_set_error(workspace, TURBO_AGENT_WORKSPACE_PARSE_ERROR,
                                        "unknown capability", *text);
        status = TURBO_AGENT_WORKSPACE_PARSE_ERROR;
        goto cleanup;
      }
      status = turbo_agent_workspace_check_capability(workspace, capability, skill->name);
      if (status != TURBO_AGENT_WORKSPACE_OK) goto cleanup;
    }
    for (item = 0; item < turbo_vec_size(&skill->tools); ++item) {
      char *const *tool = (char *const *)turbo_vec_at(&skill->tools, item);
      status = turbo_agent_workspace_add_selected_tool(workspace, source_registry,
                                                       &selection->tool_names, *tool);
      if (status != TURBO_AGENT_WORKSPACE_OK) goto cleanup;
    }
    status = turbo_agent_workspace_append_bounded(
        workspace, &selection->instructions, prefix, sizeof(prefix) - 1,
        workspace->max_instruction_bytes, "effective instructions");
    if (status == TURBO_AGENT_WORKSPACE_OK)
      status = turbo_agent_workspace_append_bounded(
          workspace, &selection->instructions, skill->name, tstr_len(skill->name),
          workspace->max_instruction_bytes, "effective instructions");
    if (status == TURBO_AGENT_WORKSPACE_OK)
      status = turbo_agent_workspace_append_bounded(workspace, &selection->instructions, "\n", 1,
                                                    workspace->max_instruction_bytes,
                                                    "effective instructions");
    if (status == TURBO_AGENT_WORKSPACE_OK)
      status = turbo_agent_workspace_append_bounded(
          workspace, &selection->instructions, skill->body, tstr_len(skill->body),
          workspace->max_instruction_bytes, "effective instructions");
    if (status != TURBO_AGENT_WORKSPACE_OK) goto cleanup;
  }
  if (turbo_vec_size(&selection->tool_names) > 0) {
    const char **names;
    turbo_tool_status_t tool_status;
    if (!source_registry) {
      turbo_agent_workspace_set_error(workspace, TURBO_AGENT_WORKSPACE_TOOL_NOT_FOUND,
                                      "tool projection", "source registry is not configured");
      status = TURBO_AGENT_WORKSPACE_TOOL_NOT_FOUND;
      goto cleanup;
    }
    names = (const char **)calloc(turbo_vec_size(&selection->tool_names), sizeof(*names));
    if (!names) {
      status = TURBO_AGENT_WORKSPACE_OUT_OF_MEMORY;
      goto cleanup;
    }
    for (index = 0; index < turbo_vec_size(&selection->tool_names); ++index) {
      char *const *name = (char *const *)turbo_vec_at(&selection->tool_names, index);
      names[index] = *name;
    }
    tool_status = turbo_tool_registry_project(
        source_registry, names, turbo_vec_size(&selection->tool_names), &selection->tools);
    free(names);
    if (tool_status != TURBO_TOOL_OK) {
      turbo_agent_workspace_set_error(workspace,
                                      tool_status == TURBO_TOOL_NOT_FOUND
                                          ? TURBO_AGENT_WORKSPACE_TOOL_NOT_FOUND
                                          : TURBO_AGENT_WORKSPACE_OUT_OF_MEMORY,
                                      "tool projection", "declared tool is unavailable");
      status = workspace->last_status;
      goto cleanup;
    }
    status = turbo_agent_workspace_bind_projected_capabilities(workspace, selection);
    if (status != TURBO_AGENT_WORKSPACE_OK) goto cleanup;
  } else {
    selection->tools = turbo_tool_registry_create();
    if (!selection->tools) {
      status = TURBO_AGENT_WORKSPACE_OUT_OF_MEMORY;
      goto cleanup;
    }
  }
  if (!selection->instructions) {
    selection->instructions = tstr_new();
    if (!selection->instructions) {
      status = TURBO_AGENT_WORKSPACE_OUT_OF_MEMORY;
      goto cleanup;
    }
  }
  workspace->last_status = TURBO_AGENT_WORKSPACE_OK;
  workspace->last_error[0] = '\0';
  *out_selection = selection;
cleanup:
  turbo_vec_destroy(&selected);
  if (status != TURBO_AGENT_WORKSPACE_OK) {
    turbo_agent_workspace_selection_destroy(selection);
  }
  return status;
}

void turbo_agent_workspace_selection_destroy(turbo_agent_workspace_selection_t *selection) {
  if (!selection) return;
  tstr_free(selection->instructions);
  turbo_tool_registry_destroy(selection->tools);
  turbo_agent_workspace_string_vec_destroy(&selection->skill_names);
  turbo_agent_workspace_string_vec_destroy(&selection->tool_names);
  free(selection);
}

const char *
turbo_agent_workspace_selection_instructions(const turbo_agent_workspace_selection_t *selection) {
  return selection ? selection->instructions : NULL;
}

const turbo_tool_registry_t *
turbo_agent_workspace_selection_tools(const turbo_agent_workspace_selection_t *selection) {
  return selection ? selection->tools : NULL;
}

size_t
turbo_agent_workspace_selection_skill_count(const turbo_agent_workspace_selection_t *selection) {
  return selection ? turbo_vec_size(&selection->skill_names) : 0;
}

const char *
turbo_agent_workspace_selection_skill_name(const turbo_agent_workspace_selection_t *selection,
                                           size_t index) {
  char *const *name;
  if (!selection || index >= turbo_vec_size(&selection->skill_names)) return NULL;
  name = (char *const *)turbo_vec_at((turbo_vec_t *)&selection->skill_names, index);
  return name ? *name : NULL;
}

size_t
turbo_agent_workspace_selection_tool_count(const turbo_agent_workspace_selection_t *selection) {
  return selection ? turbo_vec_size(&selection->tool_names) : 0;
}

const char *
turbo_agent_workspace_selection_tool_name(const turbo_agent_workspace_selection_t *selection,
                                          size_t index) {
  char *const *name;
  if (!selection || index >= turbo_vec_size(&selection->tool_names)) return NULL;
  name = (char *const *)turbo_vec_at((turbo_vec_t *)&selection->tool_names, index);
  return name ? *name : NULL;
}

static void turbo_agent_workspace_selection_free_resource(void *resource) {
  turbo_agent_workspace_selection_destroy((turbo_agent_workspace_selection_t *)resource);
}

turbo_agent_t *turbo_agent_create_for_workspace(const turbo_agent_config_t *config,
                                                turbo_agent_workspace_t *workspace,
                                                const char *task) {
  turbo_agent_workspace_selection_t *selection = NULL;
  turbo_agent_config_t effective;
  turbo_agent_t *agent;
  if (!config || !workspace || !task) return NULL;
  if (turbo_agent_workspace_prepare(workspace, task, config->tool_registry, config->instructions,
                                    &selection) != TURBO_AGENT_WORKSPACE_OK)
    return NULL;
  effective = *config;
  effective.instructions = selection->instructions;
  effective.tool_registry = selection->tools;
  agent = turbo_agent_create(&effective);
  if (!agent || turbo_agent_set_tool_policy(agent, &workspace->policy) != 0 ||
      turbo_agent_attach_owned_resource(agent, selection,
                                        turbo_agent_workspace_selection_free_resource) != 0) {
    turbo_agent_destroy(agent);
    turbo_agent_workspace_selection_destroy(selection);
    return NULL;
  }
  return agent;
}
