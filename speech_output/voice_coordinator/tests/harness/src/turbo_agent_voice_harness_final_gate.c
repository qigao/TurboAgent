#include "turbo_agent_voice_harness_final_gate.h"
#include <turbo_agent_state.h>

#include <json_parser.h>
#include <stdlib.h>
#include <string.h>

static void wipe_text(char *s, size_t bytes) {
  volatile unsigned char *p = (volatile unsigned char *)s;
  while (bytes--) *p++ = 0u;
}

int turbo_voice_admit_harness_completed_final(
    turbo_voice_coordinator_t *voice,
    uint64_t generation,
    turbo_agent_harness_t *harness,
    turbo_agent_harness_execution_t *execution,
    const char *expected_run_id,
    int host_approved,
    const char *voice_id,
    const char *language) {
  if (!voice || !generation || !harness || !execution ||
      !expected_run_id || !expected_run_id[0] || !voice_id || !language ||
      (host_approved != 0 && host_approved != 1))
    return TURBO_VOICE_EINVAL;
  if (generation != turbo_voice_get_generation(voice))
    return TURBO_VOICE_ESTALE;
  if (!host_approved ||
      turbo_voice_get_state(voice) != TURBO_VOICE_WAIT_APPROVAL)
    return TURBO_VOICE_EBUSY;

  turbo_agent_execution_status_t status = TURBO_AGENT_EXECUTION_FAILED;
  int operation_rc = SALTS_EIO;
  if (!turbo_agent_harness_execution_id(execution) ||
      !turbo_agent_harness_execution_id(execution)[0] ||
      turbo_agent_harness_execution_get_status(execution, &status) != SALTS_OK ||
      status != TURBO_AGENT_EXECUTION_COMPLETED ||
      turbo_agent_harness_execution_result_code(execution, &operation_rc) != SALTS_OK ||
      operation_rc != SALTS_OK)
    return TURBO_VOICE_EBUSY;

  turbo_agent_app_t *app = turbo_agent_harness_app(harness);
  if (!app) return TURBO_VOICE_EBUSY;
  const char *current_run_id = turbo_agent_app_last_run_id(app);
  if (!current_run_id || strcmp(current_run_id, expected_run_id) != 0)
    return TURBO_VOICE_ESTALE;

  json_value_t *summary = NULL;
  json_value_t *state = NULL;
  if (turbo_agent_harness_execution_take_result(execution, &summary, &state)
      != SALTS_OK || !summary || !state) {
    turbo_runtime_json_destroy(state);
    turbo_runtime_json_destroy(summary);
    return TURBO_VOICE_EBUSY;
  }

  const char *result_status = json_get_string(summary, "status");
  const char *result_run_id = json_get_string(summary, "run_id");
  int rc = TURBO_VOICE_EBUSY;
  if (!result_status || strcmp(result_status, "completed") != 0 ||
      !result_run_id || strcmp(result_run_id, expected_run_id) != 0)
    goto cleanup;

  /* If the Harness's App advanced to a newer run while taking the result,
   * the old run must not be spoken. Never silently bind to latest output. */
  current_run_id = turbo_agent_app_last_run_id(app);
  if (!current_run_id || strcmp(current_run_id, expected_run_id) != 0) {
    rc = TURBO_VOICE_ESTALE;
    goto cleanup;
  }

  /* Preflight canonical borrowed final text BEFORE the public malloc-copy.
   * The same canonical accessor underlies turbo_agent_app_result_text. */
  const char *canonical = turbo_agent_state_final_answer_text(state);
  if (!canonical || !canonical[0] ||
      strlen(canonical) > TURBO_TTS_MAX_TEXT_BYTES)
    goto cleanup;

  /* Original Agent result API owns the canonical final-answer projection.
   * Its malloc-returned bytes belong to Host until copied by SpeechOutput. */
  char *final = turbo_agent_app_result_text(state);
  if (!final) goto cleanup;
  size_t bytes = strlen(final);
  if (bytes == 0u || bytes > TURBO_TTS_MAX_TEXT_BYTES) {
    wipe_text(final, bytes);
    free(final);
    goto cleanup;
  }
  rc = turbo_voice_approved_final(voice, generation, final, bytes,
                                   voice_id, language);
  wipe_text(final, bytes);
  free(final);

cleanup:
  turbo_runtime_json_destroy(state);
  turbo_runtime_json_destroy(summary);
  return rc;
}
