#ifndef TURBO_AGENT_JOB_FILE_VERIFIER_H
#define TURBO_AGENT_JOB_FILE_VERIFIER_H

#include "turbo_agent_job_verifier.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TURBO_AGENT_JOB_FILE_VERIFIER_ABI_VERSION 1U
typedef struct turbo_agent_job_file_verifier_s turbo_agent_job_file_verifier_t;

typedef struct turbo_agent_job_file_verifier_config_s {
  size_t struct_size;
  uint32_t abi_version;
  const char *workspace_root;
  const char *verification_root;
  size_t max_manifest_bytes;
  size_t max_files;
  size_t max_file_bytes;
  size_t max_total_bytes;
} turbo_agent_job_file_verifier_config_t;

/** Defaults: 64 KiB manifest, 32 files, 4 MiB/file, 16 MiB total per check.
 * Hard ceilings: 256 KiB, 64 files, 64 MiB/file, 256 MiB total. All must be >0.
 * Roots must be existing Host-admitted directories. They are retained as Salts
 * root-relative capabilities, never reconstructed using cwd/path concatenation.
 * Host must pin/serialize these directories and file contents for each operation.
 * Symlink/reparse traversal below either root is refused by the Salts facade.
 * Byte limits do not provide a hard I/O deadline or an atomic filesystem snapshot.
 */
CXX_C_API void turbo_agent_job_file_verifier_config_init(turbo_agent_job_file_verifier_config_t *config);
CXX_C_API int turbo_agent_job_file_verifier_create(
    const turbo_agent_job_file_verifier_config_t *config, turbo_agent_job_file_verifier_t **out);
CXX_C_API void turbo_agent_job_file_verifier_destroy(turbo_agent_job_file_verifier_t *verifier);

/** Export complete borrowed callbacks for create_verified_with_store(). The
 * adapter must outlive every borrowing Harness/execution. Host serializes adapter
 * access and destroy; no ownership is transferred and no backend is substituted.
 * No manifests or source files are read/created during export or construction.
 *
 * Source: <job_id>.sources.json in verification_root, raw SHA-256 bound by the
 * frozen proposal's source_revision="sha256-<64 lowercase hex>". Schema-1 source
 * manifest binds Job ID, exact frozen source_json byte digest and 1..max_files
 * workspace-relative file paths, byte counts and SHA-256 digests.
 * Evidence: <evidence_ref>.evidence.json in verification_root; evidence_ref must
 * be 1..64 ASCII letters/digits/_/-. Schema-1 evidence binds Job/proposal/source,
 * explicit decision, exact execution reference, criteria/result JSON digests,
 * and 1..max_files verification-root-relative evidence files/counts/digests.
 * See docs/design/job-file-verifier.md for the exact format and trust boundary.
 *
 * Every check rereads/hashes current files. Source freshness is limited to the
 * declared file set; Host owns completeness and semantic quality of its evidence
 * and decision. This does not run commands, send, grant authority, verify a remote
 * receipt, repair files or make Host-authored manifests independently trustworthy.
 */
CXX_C_API int turbo_agent_job_file_verifier_callbacks(
    turbo_agent_job_file_verifier_t *adapter, turbo_agent_job_verifier_t *out);

/** Manifest authoring helpers. out_hex must have 65 bytes, must not alias input,
 * and clears on error.
 * JSON digest hashes exact compact TurboParser serialization, preserving object
 * insertion order; it is not an order-independent JSON canonicalization scheme.
 * Persisted results must preserve that representation. Bytes may contain NUL.
 */
CXX_C_API int turbo_agent_job_file_verifier_digest_bytes(const void *bytes, size_t size, char out_hex[65]);
CXX_C_API int turbo_agent_job_file_verifier_digest_json(const json_value_t *value, char out_hex[65]);

#ifdef __cplusplus
}
#endif
#endif
