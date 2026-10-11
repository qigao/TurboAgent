#include <salts/thread.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
  #include <process.h>
  #define fixture_pid _getpid
#else
  #include <unistd.h>
  #define fixture_pid getpid
#endif

int main(int argc, char **argv) {
  if (argc > 1 && strcmp(argv[1], "binary") == 0) {
    fputc(0, stdout);
    return 3;
  } else if (argc > 1 && strcmp(argv[1], "wait") == 0) {
    cmeta_sleep_ms(10000);
  } else if (argc > 1 &&
             (strcmp(argv[1], "partial") == 0 || strcmp(argv[1], "partial_binary") == 0)) {
    FILE *marker;
    if (strcmp(argv[1], "partial_binary") == 0) fputc(0, stdout);
    else fputs("partial stdout", stdout);
    fputs("partial stderr", stderr);
    fflush(stdout);
    fflush(stderr);
    /* Publish readiness after flushing both streams. Append detects replay. */
    marker = fopen("started.txt", "a");
    if (!marker) return 4;
    fprintf(marker, "%d\n", (int)fixture_pid());
    if (fclose(marker) != 0) return 5;
    cmeta_sleep_ms(10000);
    return 6;
  } else if (argc > 1 && strcmp(argv[1], "flood") == 0) {
    for (int i = 0; i < 65536; ++i)
      fputc('x', stdout);
  } else {
    fputs("fixture stdout", stdout);
    fputs("fixture stderr", stderr);
    return 7;
  }
  return 0;
}
