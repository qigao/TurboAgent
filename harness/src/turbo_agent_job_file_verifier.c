#include "turbo_agent_job_file_verifier.h"
#include "turbo_agent_job.h"
#include <cmeta_crypto.h>
#include <cmeta_fs.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct turbo_agent_job_file_verifier_s {
  cmeta_fs_root_t *workspace, *verification;
  size_t manifest_bytes, files, file_bytes, total_bytes;
};
static void hex_digest(const unsigned char digest[32], char out[65]) {
  static const char hex[] = "0123456789abcdef";
  for (size_t i = 0; i < 32; ++i) { out[i * 2] = hex[digest[i] >> 4]; out[i * 2 + 1] = hex[digest[i] & 15]; }
  out[64] = '\0';
}
int turbo_agent_job_file_verifier_digest_bytes(const void *bytes, size_t size, char out[65]) {
  unsigned char digest[32];
  int rc;
  if (!out) return SALTS_EINVAL;
  out[0] = '\0';
  rc = cmeta_sha256(bytes, size, digest);
  if (rc == SALTS_OK) hex_digest(digest, out);
  return rc;
}
int turbo_agent_job_file_verifier_digest_json(const json_value_t *value, char out[65]) {
  char *serialized;
  size_t size;
  int rc;
  if (out) out[0] = '\0';
  if (!value || !out) return SALTS_EINVAL;
  serialized = json_serialize(value, &size);
  if (!serialized) return SALTS_ENOMEM;
  rc = turbo_agent_job_file_verifier_digest_bytes(serialized, size, out);
  json_serialize_free(serialized);
  return rc;
}
static int fs_status(int rc) {
  if (!rc) return SALTS_OK;
  if (rc == -ENOENT) return SALTS_ENOENT;
  if (rc == -ENOMEM) return SALTS_ENOMEM;
  if (rc == -EINVAL) return SALTS_EINVAL;
  if (rc == -EACCES || rc == -EPERM) return SALTS_EPERM;
#ifdef ELOOP
  if (rc == -ELOOP) return SALTS_EPERM;
#endif
  return SALTS_EIO;
}
static const char *text(const json_value_t *r, const char *key, size_t max) {
  const json_value_t *v = json_object_get(r, key);
  const char *s;
  if (!v || json_type(v) != JSON_STRING || !json_string_len(v) || json_string_len(v) > max) return NULL;
  s = json_string(v);
  return s && strlen(s) == json_string_len(v) ? s : NULL;
}
static int is(const json_value_t *r, const char *key, const char *expected) {
  const char *s = expected ? text(r, key, strlen(expected)) : NULL;
  return s && !strcmp(s, expected);
}
static int64_t number(const json_value_t *r, const char *key) {
  return turbo_runtime_json_value_as_int64(json_object_get(r, key), -1);
}
static int id_valid(const char *s) {
  size_t i;
  if (!s || !s[0]) return 0;
  for (i = 0; i <= 64; ++i) {
    unsigned char c = (unsigned char)s[i];
    if (!c) return 1;
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '-')) return 0;
  }
  return 0;
}
static int hash_valid(const char *s) {
  if (!s || strlen(s) != 64) return 0;
  for (size_t i = 0; i < 64; ++i)
    if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return 0;
  return 1;
}
static int path_valid(const char *s) {
  size_t start = 0, i;
  if (!s || !s[0]) return 0;
  for (i = 0; i <= 1024; ++i) {
    unsigned char c = (unsigned char)s[i];
    if (c && (c < 32 || c == 127 || c == '\\' || c == ':')) return 0;
    if (!c || c == '/') {
      size_t n = i - start;
      if (!n || (n == 1 && s[start] == '.') || (n == 2 && !memcmp(s + start, "..", 2)) ||
          s[i - 1] == '.' || s[i - 1] == ' ') return 0;
      if (!c) return 1;
      start = i + 1;
    }
  }
  return 0;
}
static int version(const json_value_t *r) {
  int64_t n;
  if (!r || json_type(r) != JSON_OBJECT) return SALTS_EPROTO;
  n = number(r, "schema_version");
  return n == 1 ? SALTS_OK : n > 0 ? SALTS_ENOTSUP : SALTS_EPROTO;
}
/* Refuse excessive nesting before calling the DOM parser. Strings/escapes are
 * scanned only for this depth bound; TurboParser remains the JSON syntax parser. */
static int bounded_json(const char *s, size_t size) {
  unsigned depth = 0;
  int quoted = 0, escaped = 0;
  for (size_t i = 0; i < size; ++i) {
    char c = s[i];
    if (!c) return 0;
    if (quoted) {
      if (escaped) escaped = 0;
      else if (c == '\\') escaped = 1;
      else if (c == '"') quoted = 0;
    } else if (c == '"') quoted = 1;
    else if (c == '{' || c == '[') { if (++depth > 8) return 0; }
    else if (c == '}' || c == ']') { if (!depth) return 0; --depth; }
  }
  return !quoted && !depth;
}
static int regular_open(cmeta_fs_root_t *root, const char *path, cmeta_fs_root_file_t **out,
    cmeta_fs_stat_t *stat) {
  int rc = fs_status(cmeta_fs_root_stat(root, path, stat));
  *out = NULL;
  if (rc != SALTS_OK) return rc;
  if (!stat->is_file || stat->is_symlink) return SALTS_EPROTO;
  rc = fs_status(cmeta_fs_root_file_open(root, path, SALTS_FS_O_RDONLY, 0, out));
  if (rc == SALTS_OK) rc = fs_status(cmeta_fs_root_file_stat(*out, stat));
  if (rc == SALTS_OK && (!stat->is_file || stat->is_symlink)) rc = SALTS_EPROTO;
  return rc;
}
static int stable_file(cmeta_fs_root_file_t *file, const cmeta_fs_stat_t *before) {
  cmeta_fs_stat_t after;
  int rc = fs_status(cmeta_fs_root_file_stat(file, &after));
  if (rc != SALTS_OK) return rc;
  return after.is_file && after.size == before->size && after.mtime == before->mtime &&
      after.ctime == before->ctime ? SALTS_OK : SALTS_EPROTO;
}
static int read_manifest(turbo_agent_job_file_verifier_t *v, const char *path,
    json_value_t **out, char hash[65]) {
  cmeta_fs_root_file_t *file = NULL;
  cmeta_fs_stat_t stat;
  char *bytes = NULL;
  size_t size = 0;
  int count, rc = regular_open(v->verification, path, &file, &stat);
  *out = NULL;
  if (rc != SALTS_OK) goto done;
  if (stat.size > v->manifest_bytes) { rc = SALTS_EFBIG; goto done; }
  bytes = malloc(v->manifest_bytes + 1);
  if (!bytes) { rc = SALTS_ENOMEM; goto done; }
  for (;;) {
    size_t available = v->manifest_bytes + 1 - size;
    count = cmeta_fs_root_file_read(file, bytes + size, available < 4096 ? available : 4096);
    if (count < 0) { rc = fs_status(count); goto done; }
    if (!count) break;
    size += (size_t)count;
    if (size > v->manifest_bytes) { rc = SALTS_EFBIG; goto done; }
  }
  rc = stable_file(file, &stat);
  if (rc != SALTS_OK) goto done;
  if (!size || !bounded_json(bytes, size)) { rc = SALTS_EPROTO; goto done; }
  rc = turbo_agent_job_file_verifier_digest_bytes(bytes, size, hash);
  if (rc != SALTS_OK) goto done;
  *out = json_parse(bytes, size);
  if (!*out) rc = SALTS_EPROTO;
done:
  if (file) { int close_rc = fs_status(cmeta_fs_root_file_close(file)); if (rc == SALTS_OK) rc = close_rc; }
  if (rc != SALTS_OK) { json_free(*out); *out = NULL; }
  free(bytes);
  return rc;
}
static int hash_file(cmeta_fs_root_t *root, const char *path, size_t expected, const char *hash) {
  cmeta_fs_root_file_t *file = NULL;
  cmeta_fs_stat_t stat;
  cmeta_sha256_stream *sha = NULL;
  unsigned char digest[32];
  char chunk[4096], actual[65];
  size_t size = 0;
  int count, rc = regular_open(root, path, &file, &stat);
  if (rc != SALTS_OK) goto done;
  if (stat.size != expected) { rc = SALTS_EPROTO; goto done; }
  rc = cmeta_sha256_stream_create(&sha);
  if (rc != SALTS_OK) goto done;
  for (;;) {
    size_t available = expected - size + 1;
    count = cmeta_fs_root_file_read(file, chunk, available < sizeof(chunk) ? available : sizeof(chunk));
    if (count < 0) { rc = fs_status(count); goto done; }
    if (!count) break;
    size += (size_t)count;
    if (size > expected) { rc = SALTS_EPROTO; goto done; }
    rc = cmeta_sha256_stream_update(sha, chunk, (size_t)count);
    if (rc != SALTS_OK) goto done;
  }
  if (size != expected) { rc = SALTS_EPROTO; goto done; }
  rc = stable_file(file, &stat);
  if (rc != SALTS_OK) goto done;
  rc = cmeta_sha256_stream_finish(sha, digest);
  if (rc == SALTS_OK) { hex_digest(digest, actual); if (strcmp(actual, hash)) rc = SALTS_EPROTO; }
done:
  cmeta_sha256_stream_destroy(sha);
  if (file) { int close_rc = fs_status(cmeta_fs_root_file_close(file)); if (rc == SALTS_OK) rc = close_rc; }
  return rc;
}
static int verify_files(turbo_agent_job_file_verifier_t *v, cmeta_fs_root_t *root, const json_value_t *files) {
  size_t total = 0, count = json_array_size(files);
  if (!files || json_type(files) != JSON_ARRAY || !count) return SALTS_EPROTO;
  if (count > v->files) return SALTS_EFBIG;
  for (size_t i = 0; i < count; ++i) {
    const json_value_t *entry = json_array_get(files, i);
    const char *path = text(entry, "path", 1024), *hash = text(entry, "sha256", 64);
    int64_t bytes = number(entry, "byte_count");
    int rc;
    if (json_type(entry) != JSON_OBJECT || json_object_size(entry) != 3 || !path_valid(path) ||
        !hash_valid(hash) || bytes < 0) return SALTS_EPROTO;
    if ((uint64_t)bytes > v->file_bytes || (uint64_t)bytes > v->total_bytes - total) return SALTS_EFBIG;
    for (size_t j = 0; j < i; ++j)
      if (is(json_array_get(files, j), "path", path)) return SALTS_EPROTO;
    total += (size_t)bytes;
    rc = hash_file(root, path, (size_t)bytes, hash);
    if (rc != SALTS_OK) return rc;
  }
  return SALTS_OK;
}
static int verify_source(const json_value_t *snapshot, void *user) {
  turbo_agent_job_file_verifier_t *v = user;
  const json_value_t *proposal = json_object_get(snapshot, "proposal");
  const char *id = text(snapshot, "id", 64), *revision = text(proposal, "source_revision", 71);
  const json_value_t *source = json_object_get(proposal, "source_json");
  json_value_t *manifest = NULL;
  char path[96], hash[65], source_hash[65];
  int rc;
  if (!v || !id_valid(id) || !revision || strncmp(revision, "sha256-", 7) || !hash_valid(revision + 7) ||
      !source || json_type(source) != JSON_STRING || !json_string_len(source) ||
      json_string_len(source) > TURBO_AGENT_JOB_MAX_SOURCE_BYTES) return SALTS_EPROTO;
  (void)snprintf(path, sizeof(path), "%s.sources.json", id);
  rc = read_manifest(v, path, &manifest, hash);
  if (rc != SALTS_OK) goto done;
  if (strcmp(hash, revision + 7)) { rc = SALTS_EPROTO; goto done; }
  rc = version(manifest);
  if (rc != SALTS_OK) goto done;
  rc = turbo_agent_job_file_verifier_digest_bytes(json_string(source), json_string_len(source), source_hash);
  if (rc != SALTS_OK) goto done;
  if (json_object_size(manifest) != 5 || !is(manifest, "kind", "job_file_sources") ||
      !is(manifest, "job_id", id) || !is(manifest, "source_json_sha256", source_hash)) { rc = SALTS_EPROTO; goto done; }
  rc = verify_files(v, v->workspace, json_object_get(manifest, "files"));
done:
  json_free(manifest); return rc;
}
static int same_reference(const json_value_t *a, const json_value_t *b) {
  const char *strings[] = {"kind", "job_id", "thread_id", "attempt_id", "attempt_record_id", "run_id", "run_thread_id", "execution_id", "plan_hash"};
  const char *numbers[] = {"schema_version", "job_revision", "proposal_version", "authority_generation"};
  if (json_object_size(a) != 13 || json_object_size(b) != 13 || !is(a, "kind", "compiled_inspect")) return 0;
  for (size_t i = 0; i < sizeof(strings) / sizeof(strings[0]); ++i)
    if (!is(a, strings[i], text(b, strings[i], 160))) return 0;
  for (size_t i = 0; i < sizeof(numbers) / sizeof(numbers[0]); ++i)
    if (number(a, numbers[i]) < 0 || number(a, numbers[i]) != number(b, numbers[i])) return 0;
  return number(a, "schema_version") == 1;
}
static int verify_evidence(const json_value_t *snapshot, const json_value_t *reference,
    const json_value_t *result, turbo_agent_job_acceptance_decision_t decision,
    const char *evidence_ref, void *user) {
  turbo_agent_job_file_verifier_t *v = user;
  const json_value_t *proposal = json_object_get(snapshot, "proposal");
  json_value_t *manifest = NULL;
  char path[96], hash[65], criteria_hash[65], result_hash[65];
  int rc;
  if (!v || !id_valid(evidence_ref) || (decision != TURBO_AGENT_JOB_ACCEPTED && decision != TURBO_AGENT_JOB_REJECTED))
    return SALTS_EINVAL;
  (void)snprintf(path, sizeof(path), "%s.evidence.json", evidence_ref);
  rc = read_manifest(v, path, &manifest, hash);
  if (rc != SALTS_OK) goto done;
  rc = version(manifest);
  if (rc != SALTS_OK) goto done;
  rc = turbo_agent_job_file_verifier_digest_json(json_object_get(snapshot, "acceptance_criteria"), criteria_hash);
  if (rc == SALTS_OK) rc = turbo_agent_job_file_verifier_digest_json(result, result_hash);
  if (rc != SALTS_OK) goto done;
  if (json_object_size(manifest) != 12 || !is(manifest, "kind", "job_file_evidence") ||
      !is(manifest, "job_id", text(snapshot, "id", 64)) || !is(manifest, "evidence_ref", evidence_ref) ||
      number(manifest, "job_revision") != number(snapshot, "revision") ||
      number(manifest, "proposal_version") != number(proposal, "revision") ||
      !is(manifest, "source_revision", text(proposal, "source_revision", 71)) ||
      !is(manifest, "decision", decision == TURBO_AGENT_JOB_ACCEPTED ? "accepted" : "rejected") ||
      !is(manifest, "criteria_sha256", criteria_hash) || !is(manifest, "result_sha256", result_hash) ||
      !same_reference(json_object_get(manifest, "execution_reference"), reference)) { rc = SALTS_EPROTO; goto done; }
  rc = verify_files(v, v->verification, json_object_get(manifest, "files"));
done:
  json_free(manifest); return rc;
}
void turbo_agent_job_file_verifier_config_init(turbo_agent_job_file_verifier_config_t *config) {
  if (!config) return;
  memset(config, 0, sizeof(*config));
  config->struct_size = sizeof(*config); config->abi_version = TURBO_AGENT_JOB_FILE_VERIFIER_ABI_VERSION;
  config->max_manifest_bytes = 65536; config->max_files = 32;
  config->max_file_bytes = 4u * 1024u * 1024u; config->max_total_bytes = 16u * 1024u * 1024u;
}
int turbo_agent_job_file_verifier_create(const turbo_agent_job_file_verifier_config_t *config,
    turbo_agent_job_file_verifier_t **out) {
  turbo_agent_job_file_verifier_t *v;
  int rc;
  if (out) *out = NULL;
  if (!out || !config || config->struct_size < sizeof(*config) ||
      config->abi_version != TURBO_AGENT_JOB_FILE_VERIFIER_ABI_VERSION || !config->workspace_root || !config->verification_root ||
      !config->max_manifest_bytes || config->max_manifest_bytes > 262144 || !config->max_files || config->max_files > 64 ||
      !config->max_file_bytes || config->max_file_bytes > 64u * 1024u * 1024u ||
      !config->max_total_bytes || config->max_total_bytes > 256u * 1024u * 1024u) return SALTS_EINVAL;
  v = calloc(1, sizeof(*v));
  if (!v) return SALTS_ENOMEM;
  v->manifest_bytes = config->max_manifest_bytes; v->files = config->max_files;
  v->file_bytes = config->max_file_bytes; v->total_bytes = config->max_total_bytes;
  rc = fs_status(cmeta_fs_root_open(config->workspace_root, &v->workspace));
  if (rc == SALTS_OK) rc = fs_status(cmeta_fs_root_open(config->verification_root, &v->verification));
  if (rc != SALTS_OK) { turbo_agent_job_file_verifier_destroy(v); return rc; }
  *out = v; return SALTS_OK;
}
void turbo_agent_job_file_verifier_destroy(turbo_agent_job_file_verifier_t *v) {
  if (!v) return;
  if (v->workspace) cmeta_fs_root_close(v->workspace);
  if (v->verification) cmeta_fs_root_close(v->verification);
  free(v);
}
int turbo_agent_job_file_verifier_callbacks(turbo_agent_job_file_verifier_t *v, turbo_agent_job_verifier_t *out) {
  if (out) memset(out, 0, sizeof(*out));
  if (!v || !out) return SALTS_EINVAL;
  turbo_agent_job_verifier_init(out);
  out->source = verify_source; out->evidence = verify_evidence; out->user_data = v;
  return SALTS_OK;
}
