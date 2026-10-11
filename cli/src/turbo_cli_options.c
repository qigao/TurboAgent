#include "turbo_cli.h"
#include <cmd_arger.h>

static const char cli_description[] =
    "TurboAgent\n"
    "Without -p, read one user message per line; /quit or EOF exits.\n"
    "Model configuration: OPENAI_*/ANTHROPIC_* environment and optional .env.\n"
    "Each turn loads workspace AGENTS.md and matching skills/ instructions.\n"
    "Default tools can edit workspace files and run shell commands with host\n"
    "privileges. --read-only registers only file reading/listing/search.\n"
    "Sessions are in memory; at most 64 turns, 1 MiB projected history,\n"
    "128 graph steps and 10 minutes per turn. Ctrl-C cancels the run.";

void turbo_cli_parse_options(int argc, char **argv, turbo_cli_options_t *options) {
  char *workspace = ".", *prompt = NULL, *model = NULL;
  CmdArgerBool read_only = cmd_arger_false, help = cmd_arger_false;
  CmdArgerDesc arguments[] = {
    cmd_arger_desc_string_sh(&workspace, "workspace", "C", "Workspace directory"),
    cmd_arger_desc_string_sh(&prompt, "prompt", "p", "Run one task instead of reading messages from stdin"),
    cmd_arger_desc_string(&model, "model", "Override the environment model"),
    cmd_arger_desc_flag(&read_only, "read-only", "Register only file reading/listing/search tools"),
    cmd_arger_desc_flag_sh(&help, "help", "h", "Show help")
  };
  const uint32_t count = sizeof(arguments) / sizeof(arguments[0]);
  /* CmdParser owns parsing and generated help. Keep its terminating API outside
   * the driver so no Session, executor or workspace resources exist on exit. */
  cmd_arger_parse(arguments, count, NULL, 0, argc, argv, cli_description, cmd_arger_false);
  /* The SDK recognizes --help intrinsically; -h uses the same generated help. */
  if (help)
    cmd_arger_show_help_and_exit(arguments, count, NULL, 0, argv[0], cli_description, cmd_arger_false);
  *options = (turbo_cli_options_t){workspace, prompt, model, read_only};
}
