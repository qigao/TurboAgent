#include <salts_capture.h>
#include <stdio.h>

/* Isolated PUBLIC, actually released Salts::Capture DSO consumer. This does
 * NOT link miniaudio's explicit test-null Playback implementation: the
 * official Capture DSO must resolve its own native OS context and PipeWire
 * dependencies, rather than accidentally importing fixture device symbols.
 * No microphone is opened or started. */
#define VERIFY(x) do { if (!(x)) { \
  fprintf(stderr, "FAIL released Capture no-device line %d: %s\n", \
          __LINE__, #x); return 1; \
} } while (0)

int main(void) {
  salts_capture_device_t devices[SALTS_CAPTURE_MAX_DEVICES] = {{0}};
  int devices_found = salts_capture_list_audio_devices(
      devices, SALTS_CAPTURE_MAX_DEVICES);
  /* Device enumeration may fail because no user audio server exists in CI;
   * that is a reported environmental outcome, never a positive mic test. */
  VERIFY(devices_found >= 0 || devices_found == SALTS_CAPTURE_ERR_DEVICE);

  salts_audio_capture_config_t invalid = {12345, 1, 16, 20};
  salts_audio_capture_config_t permitted = {16000, 1, 16, 20};
  VERIFY(salts_audio_capture_create(NULL, &invalid) == NULL);
  /* Exact ID policy must reject malformed IDs even when OS context exists.
   * Do NOT call create(NULL, permitted) without explicit physical permission. */
  VERIFY(salts_audio_capture_create("not-an-enumerated-device", &permitted) == NULL);
  VERIFY(salts_audio_capture_create("-1", &permitted) == NULL);

  printf("PASS actual published Salts Capture native context/invalid-ID rejection; listed=%d (NOT physical mic)\n",
         devices_found);
  return 0;
}
