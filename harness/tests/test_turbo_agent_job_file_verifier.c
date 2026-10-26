#include "tinytest.h"
#include "turbo_agent_job_file_verifier.h"
#include <cmeta_fs.h>
#include <cmeta_uuid.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char root[SALTS_FS_MAX_PATH], workspace[SALTS_FS_MAX_PATH], verification[SALTS_FS_MAX_PATH];
static turbo_agent_job_file_verifier_t *adapter;
static turbo_agent_job_verifier_t callbacks;
static json_value_t *snapshot, *sources, *evidence, *reference, *result;
static const char input_bytes[] = {'a', 0, 'b'};
static void write_bytes(const char *directory, const char *name, const void *bytes, size_t size) {
  char path[SALTS_FS_MAX_PATH];
  cmeta_fs_buf_t buffer = cmeta_fs_buf_init((char *)bytes, size);
  check_equal(cmeta_fs_path_join(path, sizeof(path), directory, name), 0);
  check_equal(cmeta_fs_write_file(path, &buffer), 0);
}
static void unlink_file(const char *directory, const char *name) {
  char path[SALTS_FS_MAX_PATH];
  check_equal(cmeta_fs_path_join(path, sizeof(path), directory, name), 0);
  check_equal(cmeta_fs_unlink(path), 0);
}
static void save(const char *name, const json_value_t *record) {
  size_t size;
  char *bytes = json_serialize(record, &size);
  check_not_null(bytes);
  write_bytes(verification, name, bytes, size);
  json_serialize_free(bytes);
}
static json_value_t *file_entry(const char *path, const void *bytes, size_t size) {
  json_value_t *r = json_create_object(); char hash[65];
  check_equal(turbo_agent_job_file_verifier_digest_bytes(bytes, size, hash), SALTS_OK);
  check_equal(turbo_runtime_json_object_set_string(r, "path", path), 0);
  check_equal(turbo_runtime_json_object_set_int64(r, "byte_count", (int64_t)size), 0);
  check_equal(turbo_runtime_json_object_set_string(r, "sha256", hash), 0);
  return r;
}
static void pin_sources(void) {
  char hash[65], revision[72];
  check_equal(turbo_agent_job_file_verifier_digest_json(sources, hash), SALTS_OK);
  (void)snprintf(revision, sizeof(revision), "sha256-%s", hash);
  check_equal(turbo_runtime_json_object_set_string(json_object_get(snapshot, "proposal"), "source_revision", revision), 0);
  save("file-job.sources.json", sources);
}
static int source_check(void) { return callbacks.source(snapshot, callbacks.user_data); }
static int evidence_check(void) {
  return callbacks.evidence(snapshot, reference, result, TURBO_AGENT_JOB_ACCEPTED, "review-1", callbacks.user_data);
}

spec("Host file verifier real rooted files") {
  before_each() {
    char temp[SALTS_FS_MAX_PATH], name[96], uuid[SALTS_UUID_STRING_SIZE], hash[65];
    cmeta_uuid_t id;
    turbo_agent_job_file_verifier_config_t config;
    json_value_t *files;
    const char *snapshot_bytes = "{\"id\":\"file-job\",\"revision\":2,\"acceptance_criteria\":\"Host verifies\",\"proposal\":{\"revision\":1,\"source_json\":\"{}\"}}";
    check_equal(cmeta_fs_get_tmpdir(temp, sizeof(temp)), 0);
    check_equal(cmeta_uuid_v4_generate(&id), 0); check_equal(cmeta_uuid_format(&id, uuid, sizeof(uuid)), 0);
    (void)snprintf(name, sizeof(name), "Job verifier %s", uuid);
    check_equal(cmeta_fs_path_join(root, sizeof(root), temp, name), 0);
    check_equal(cmeta_fs_mkdir(root, 0700), 0);
    check_equal(cmeta_fs_path_join(workspace, sizeof(workspace), root, "workspace"), 0);
    check_equal(cmeta_fs_path_join(verification, sizeof(verification), root, "verification"), 0);
    check_equal(cmeta_fs_mkdir(workspace, 0700), 0); check_equal(cmeta_fs_mkdir(verification, 0700), 0);
    write_bytes(workspace, "source.bin", input_bytes, sizeof(input_bytes));
    write_bytes(verification, "check.txt", "pass\n", 5);
    turbo_agent_job_file_verifier_config_init(&config);
    config.workspace_root = workspace; config.verification_root = verification;
    check_equal(turbo_agent_job_file_verifier_create(&config, &adapter), SALTS_OK);
    check_equal(turbo_agent_job_file_verifier_callbacks(adapter, &callbacks), SALTS_OK);
    snapshot = json_parse(snapshot_bytes, strlen(snapshot_bytes));
    check_not_null(snapshot);
    sources = json_create_object();
    check_equal(turbo_runtime_json_object_set_int64(sources, "schema_version", 1), 0);
    check_equal(turbo_runtime_json_object_set_string(sources, "kind", "job_file_sources"), 0);
    check_equal(turbo_runtime_json_object_set_string(sources, "job_id", "file-job"), 0);
    check_equal(turbo_agent_job_file_verifier_digest_bytes("{}", 2, hash), SALTS_OK);
    check_equal(turbo_runtime_json_object_set_string(sources, "source_json_sha256", hash), 0);
    files = json_create_array(); check_equal(turbo_runtime_json_array_append(files, file_entry("source.bin", input_bytes, sizeof(input_bytes))), 0);
    check_equal(turbo_runtime_json_object_set(sources, "files", files), 0);
    pin_sources();
    {
      const char *r = "{\"schema_version\":1,\"kind\":\"compiled_inspect\",\"job_id\":\"file-job\",\"job_revision\":2,\"proposal_version\":1,\"thread_id\":\"thread\",\"attempt_id\":\"attempt\",\"attempt_record_id\":\"006_thread_attempt\",\"run_id\":\"run\",\"run_thread_id\":\"run-thread\",\"execution_id\":\"execution\",\"plan_hash\":\"0123456789abcdef\",\"authority_generation\":2}";
      reference = json_parse(r, strlen(r));
      result = json_parse("{\"ok\":true}", 11);
    }
    evidence = json_create_object();
    check_equal(turbo_runtime_json_object_set_int64(evidence, "schema_version", 1), 0);
    check_equal(turbo_runtime_json_object_set_string(evidence, "kind", "job_file_evidence"), 0);
    check_equal(turbo_runtime_json_object_set_string(evidence, "job_id", "file-job"), 0);
    check_equal(turbo_runtime_json_object_set_int64(evidence, "job_revision", 2), 0);
    check_equal(turbo_runtime_json_object_set_int64(evidence, "proposal_version", 1), 0);
    check_equal(turbo_runtime_json_object_set_string(evidence, "source_revision", json_get_string(json_object_get(snapshot, "proposal"), "source_revision")), 0);
    check_equal(turbo_runtime_json_object_set_string(evidence, "decision", "accepted"), 0);
    check_equal(turbo_runtime_json_object_set_string(evidence, "evidence_ref", "review-1"), 0);
    check_equal(turbo_agent_job_file_verifier_digest_json(json_object_get(snapshot, "acceptance_criteria"), hash), SALTS_OK);
    check_equal(turbo_runtime_json_object_set_string(evidence, "criteria_sha256", hash), 0);
    check_equal(turbo_agent_job_file_verifier_digest_json(result, hash), SALTS_OK);
    check_equal(turbo_runtime_json_object_set_string(evidence, "result_sha256", hash), 0);
    check_equal(turbo_runtime_json_object_set_clone(evidence, "execution_reference", reference), 0);
    files = json_create_array(); check_equal(turbo_runtime_json_array_append(files, file_entry("check.txt", "pass\n", 5)), 0);
    check_equal(turbo_runtime_json_object_set(evidence, "files", files), 0);
    save("review-1.evidence.json", evidence);
  }
  after_each() {
    turbo_agent_job_file_verifier_destroy(adapter); adapter = NULL;
    json_free(snapshot); json_free(sources); json_free(evidence); json_free(reference); json_free(result);
    snapshot = sources = evidence = reference = result = NULL;
    unlink_file(workspace, "source.bin"); unlink_file(verification, "check.txt");
    unlink_file(verification, "file-job.sources.json"); unlink_file(verification, "review-1.evidence.json");
    check_equal(cmeta_fs_rmdir(workspace), 0); check_equal(cmeta_fs_rmdir(verification), 0); check_equal(cmeta_fs_rmdir(root), 0);
  }

  it("hashes binary source and evidence files and detects same-size replacements") {
    char hash[65];
    check_equal(turbo_agent_job_file_verifier_digest_bytes("abc", 3, hash), SALTS_OK);
    check_equal(hash, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    check_equal(source_check(), SALTS_OK); check_equal(evidence_check(), SALTS_OK);
    write_bytes(workspace, "source.bin", "abc", 3);
    check_equal(source_check(), SALTS_EPROTO);
    write_bytes(workspace, "source.bin", input_bytes, sizeof(input_bytes));
    check_equal(source_check(), SALTS_OK);
    write_bytes(verification, "check.txt", "fail\n", 5);
    check_equal(evidence_check(), SALTS_EPROTO);
  }
  it("pins the exact source manifest bytes and frozen proposal digest") {
    size_t size;
    char *serialized = json_serialize(sources, &size), *changed = malloc(size + 1);
    check_not_null(serialized); check_not_null(changed);
    memcpy(changed, serialized, size); changed[size] = '\n';
    write_bytes(verification, "file-job.sources.json", changed, size + 1);
    free(changed); json_serialize_free(serialized);
    check_equal(source_check(), SALTS_EPROTO); /* Same parsed values, different pinned bytes. */
    pin_sources(); check_equal(source_check(), SALTS_OK);
    check_equal(turbo_runtime_json_object_set_string(sources, "job_id", "different"), 0);
    save("file-job.sources.json", sources);
    check_equal(source_check(), SALTS_EPROTO);
    pin_sources(); check_equal(source_check(), SALTS_EPROTO);
    check_equal(turbo_runtime_json_object_set_string(sources, "job_id", "file-job"), 0);
    check_equal(turbo_runtime_json_object_set_string(sources, "source_json_sha256", "invalid"), 0);
    pin_sources(); check_equal(source_check(), SALTS_EPROTO);
  }
  it("rejects every external evidence identity and every execution reference field") {
    const char *fields[] = {"kind", "job_id", "job_revision", "proposal_version", "source_revision", "decision", "evidence_ref", "criteria_sha256", "result_sha256"};
    const char *refs[] = {"schema_version", "kind", "job_id", "job_revision", "proposal_version", "thread_id", "attempt_id", "attempt_record_id", "run_id", "run_thread_id", "execution_id", "plan_hash", "authority_generation"};
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
      json_value_t *changed = json_clone(evidence);
      check_equal(turbo_runtime_json_object_set_string(changed, fields[i], "unrelated"), 0);
      save("review-1.evidence.json", changed); json_free(changed);
      check_equal(evidence_check(), SALTS_EPROTO);
    }
    for (size_t i = 0; i < sizeof(refs) / sizeof(refs[0]); ++i) {
      json_value_t *changed = json_clone(evidence);
      check_equal(turbo_runtime_json_object_set_string(json_object_get(changed, "execution_reference"), refs[i], "unrelated"), 0);
      save("review-1.evidence.json", changed); json_free(changed);
      check_equal(evidence_check(), SALTS_EPROTO);
    }
  }
  it("preserves missing files and unknown schemas without repairing evidence") {
    unlink_file(verification, "check.txt");
    check_equal(evidence_check(), SALTS_ENOENT);
    write_bytes(verification, "check.txt", "pass\n", 5);
    check_equal(turbo_runtime_json_object_set_int64(evidence, "schema_version", 2), 0);
    save("review-1.evidence.json", evidence); check_equal(evidence_check(), SALTS_ENOTSUP);
    check_equal(turbo_runtime_json_object_set_int64(sources, "schema_version", 2), 0);
    pin_sources(); check_equal(source_check(), SALTS_ENOTSUP);
  }
  it("supports an explicitly empty source file while refusing an empty file set") {
    json_value_t *entry = json_array_get(json_object_get(sources, "files"), 0);
    char hash[65];
    write_bytes(workspace, "source.bin", "", 0);
    check_equal(turbo_agent_job_file_verifier_digest_bytes(NULL, 0, hash), SALTS_OK);
    check_equal(turbo_runtime_json_object_set_int64(entry, "byte_count", 0), 0);
    check_equal(turbo_runtime_json_object_set_string(entry, "sha256", hash), 0);
    pin_sources(); check_equal(source_check(), SALTS_OK);
    check_equal(turbo_runtime_json_object_set(sources, "files", json_create_array()), 0);
    pin_sources(); check_equal(source_check(), SALTS_EPROTO);
  }
  it("rejects traversal, aliases and duplicate file paths before reading outside roots") {
    const char *paths[] = {"../source.bin", "/source.bin", "a//b", "a/./b", "a\\b", "C:secret", "source.bin/", "source.bin.", "source.bin ", "a/../b"};
    json_value_t *entry = json_array_get(json_object_get(sources, "files"), 0);
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
      check_equal(turbo_runtime_json_object_set_string(entry, "path", paths[i]), 0);
      pin_sources(); check_equal(source_check(), SALTS_EPROTO);
    }
    check_equal(turbo_runtime_json_object_set_string(entry, "path", "source.bin"), 0);
    check_equal(turbo_runtime_json_array_append(json_object_get(sources, "files"), json_clone(entry)), 0);
    pin_sources(); check_equal(source_check(), SALTS_EPROTO);
  }
  it("enforces manifest, file, count and aggregate limits") {
    turbo_agent_job_file_verifier_config_t config;
    turbo_agent_job_file_verifier_t *limited = NULL;
    turbo_agent_job_verifier_t check;
    turbo_agent_job_file_verifier_config_init(&config); config.workspace_root = workspace; config.verification_root = verification;
    for (int i = 0; i < 3; ++i) {
      config.max_manifest_bytes = i == 0 ? 10 : 65536;
      config.max_file_bytes = i == 1 ? 2 : 4u * 1024u * 1024u;
      config.max_total_bytes = i == 2 ? 2 : 16u * 1024u * 1024u;
      check_equal(turbo_agent_job_file_verifier_create(&config, &limited), SALTS_OK);
      check_equal(turbo_agent_job_file_verifier_callbacks(limited, &check), SALTS_OK);
      check_equal(check.source(snapshot, check.user_data), SALTS_EFBIG);
      turbo_agent_job_file_verifier_destroy(limited); limited = NULL;
    }
    config.max_manifest_bytes = 65536; config.max_file_bytes = 1024; config.max_total_bytes = 1024; config.max_files = 1;
    check_equal(turbo_runtime_json_array_append(json_object_get(sources, "files"), file_entry("source.bin", input_bytes, sizeof(input_bytes))), 0);
    pin_sources();
    check_equal(turbo_agent_job_file_verifier_create(&config, &limited), SALTS_OK);
    check_equal(turbo_agent_job_file_verifier_callbacks(limited, &check), SALTS_OK);
    check_equal(check.source(snapshot, check.user_data), SALTS_EFBIG);
    turbo_agent_job_file_verifier_destroy(limited);
  }
  it("refuses excessive nesting, NUL and malformed JSON before verification") {
    const char nested[] = "[[[[[[[[[0]]]]]]]]]";
    write_bytes(verification, "review-1.evidence.json", nested, strlen(nested));
    check_equal(evidence_check(), SALTS_EPROTO);
    write_bytes(verification, "review-1.evidence.json", "{}\0{}", 5);
    check_equal(evidence_check(), SALTS_EPROTO);
    write_bytes(verification, "review-1.evidence.json", "{broken", 7);
    check_equal(evidence_check(), SALTS_EPROTO);
  }
#ifndef _WIN32
  it("refuses a linked source path through the retained root capability") {
    char target[SALTS_FS_MAX_PATH], link[SALTS_FS_MAX_PATH];
    json_value_t *entry = json_array_get(json_object_get(sources, "files"), 0);
    check_equal(cmeta_fs_path_join(target, sizeof(target), workspace, "source.bin"), 0);
    check_equal(cmeta_fs_path_join(link, sizeof(link), workspace, "linked.bin"), 0);
    check_equal(cmeta_fs_symlink(target, link, 0), 0);
    check_equal(turbo_runtime_json_object_set_string(entry, "path", "linked.bin"), 0);
    pin_sources(); check_not_equal(source_check(), SALTS_OK);
    check_equal(cmeta_fs_unlink(link), 0);
  }
#endif
  it("clears outputs and rejects unsupported configuration and identifiers") {
    turbo_agent_job_file_verifier_config_t config;
    turbo_agent_job_file_verifier_t *bad = adapter;
    char hash[65];
    turbo_agent_job_file_verifier_config_init(&config); config.workspace_root = workspace; config.verification_root = verification;
    config.max_files = 65;
    check_equal(turbo_agent_job_file_verifier_create(&config, &bad), SALTS_EINVAL); check_null(bad);
    check_equal(callbacks.evidence(snapshot, reference, result, TURBO_AGENT_JOB_ACCEPTED, "../bad", callbacks.user_data), SALTS_EINVAL);
    check_equal(turbo_agent_job_file_verifier_digest_json(NULL, hash), SALTS_EINVAL); check_equal(hash[0], '\0');
    check_equal(turbo_agent_job_file_verifier_callbacks(NULL, &callbacks), SALTS_EINVAL);
    check_null(callbacks.source); check_null(callbacks.evidence);
  }
}
