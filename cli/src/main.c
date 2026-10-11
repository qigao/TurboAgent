#include "turbo_cli.h"
#ifdef _WIN32
#include <windows.h>
#include <tstr.h>
#include <stdlib.h>
#endif

static volatile sig_atomic_t interrupted;
static void interrupt_handler(int signal_number) {
  (void)signal_number;
  interrupted = 1;
}

static int cli_main(int argc, char **argv) {
  turbo_cli_options_t options;
  turbo_cli_parse_options(argc, argv, &options);
  signal(SIGINT, interrupt_handler);
  return turbo_cli_run(&options, stdin, stdout, stderr, NULL, &interrupted);
}

#ifdef _WIN32
int wmain(int argc, wchar_t **wide_argv) {
  tstr *argv;
  int result = 2;
  if (argc < 1 || argc > 64) return 2;
  argv = calloc((size_t)argc, sizeof(*argv));
  if (!argv) return 1;
  for (int i = 0; i < argc; ++i) {
    int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide_argv[i], -1, NULL, 0, NULL, NULL);
    if (bytes <= 0) goto done;
    argv[i] = tstr_new_len(NULL, (size_t)bytes - 1);
    if (!argv[i] || !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide_argv[i], -1, argv[i], bytes, NULL, NULL)) goto done;
  }
  result = cli_main(argc, argv);
done:
  for (int i = 0; i < argc; ++i) tstr_free(argv[i]);
  free(argv);
  return result;
}
#else
int main(int argc, char **argv) {
  return cli_main(argc, argv);
}
#endif
