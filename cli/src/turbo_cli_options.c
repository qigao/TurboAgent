#include "turbo_cli.h"
#include <cmd_arger.h>

static const char cli_description[] =
    "TurboAgent\n"
    "Without -p, read one user message per line; /quit or EOF exits.\n"
    "Model configuration: OPENAI_*/ANTHROPIC_* environment and optional .env.\n"
    "Both modes load workspace AGENTS.md and matching skills/ instructions.\n"
    "Default tools can edit workspace files and run shell commands with host\n"
    "privileges. --read-only registers only file reading/listing/search.\n"
    "--session-db stores a session; --resume continues completed turns.\n"
    "--mode general selects a general assistant without coding tools.\n"
    "--workflow loop|review|engineering selects execution (default loop).\n"
    "--knowledge-db enables FTS5 retrieval before each turn; --index-dir imports text/code.\n"
    "--vector-db selects vector retrieval with --embedding-url and --embedding-model.\n"
    "--diagram mermaid|json exports a saved session without running a model.\n"
    "--diagram-journals all|ID1,ID2 adds current tool journals for the published run.\n"
    "--diagram-focus ID limits that view to a node's neighborhood.\n"
    "--diagram-to ID selects one shortest path from the focus instead.\n"
    "Sessions default to memory; at most 64 turns, 1 MiB projected history,\n"
    "128 graph steps and 10 minutes per turn. Ctrl-C cancels the run.";

void turbo_cli_parse_options(int argc, char **argv, turbo_cli_options_t *options) {
  char *workspace = ".", *prompt = NULL, *model = NULL, *session_db = NULL, *diagram = NULL;
  char *diagram_focus = NULL, *diagram_direction = NULL, *diagram_depth = NULL;
  char *diagram_journals = NULL;
  char *diagram_to = NULL;
  char *mode = NULL, *instructions = NULL, *knowledge_db = NULL, *index_dir = NULL;
  char *vector_db = NULL, *embedding_url = NULL, *embedding_model = NULL;
  char *workflow = NULL;
  CmdArgerBool read_only = cmd_arger_false, help = cmd_arger_false, resume = cmd_arger_false;
  CmdArgerBool diagram_origins = cmd_arger_false;
  CmdArgerBool diagram_plan_steps = cmd_arger_false;
  CmdArgerDesc arguments[] = {
    cmd_arger_desc_string_sh(&workspace, "workspace", "C", "Workspace directory"),
    cmd_arger_desc_string_sh(&prompt, "prompt", "p", "Run one task instead of reading messages from stdin"),
    cmd_arger_desc_string(&model, "model", "Override the environment model"),
    cmd_arger_desc_string(&mode, "mode", "Agent mode: coding (default) or general"),
    cmd_arger_desc_string(&workflow, "workflow", "Workflow: loop (default), review (plan/execute with review gate), engineering (also replan failures)"),
    cmd_arger_desc_string(&instructions, "instructions", "Override the agent's instructions with UTF-8 text"),
    cmd_arger_desc_string(&knowledge_db, "knowledge-db", "Dedicated SQLite knowledge index for per-turn FTS5 retrieval"),
    cmd_arger_desc_string(&index_dir, "index-dir", "Import supported text/code files at startup (requires --knowledge-db or --vector-db)"),
    cmd_arger_desc_string(&vector_db, "vector-db", "Dedicated persistent vector index; mutually exclusive with --knowledge-db"),
    cmd_arger_desc_string(&embedding_url, "embedding-url", "Full HTTPS embeddings endpoint, or loopback HTTP endpoint"),
    cmd_arger_desc_string(&embedding_model, "embedding-model", "Embedding model ID; key comes from TURBOAGENT_EMBEDDING_API_KEY"),
    cmd_arger_desc_flag(&read_only, "read-only", "Register only file reading/listing/search tools"),
    cmd_arger_desc_string(&session_db, "session-db", "SQLite session file (parent directory must exist)"),
    cmd_arger_desc_flag(&resume, "resume", "Continue a completed session from --session-db"),
    cmd_arger_desc_string(&diagram, "diagram", "Export published session lineage as mermaid or json and exit"),
    cmd_arger_desc_string(&diagram_journals, "diagram-journals", "Discover with all, or select 1..32 unique comma-separated lowercase SHA-256 journal keys (requires --diagram)"),
    cmd_arger_desc_flag(&diagram_origins, "diagram-origins", "Link verified published model requests (requires --diagram-journals)"),
    cmd_arger_desc_flag(&diagram_plan_steps, "diagram-plan-steps", "Link recorded workflow steps and model requests (requires --diagram-journals)"),
    cmd_arger_desc_string(&diagram_focus, "diagram-focus", "Focus on an exact JSON node id (requires --diagram)"),
    cmd_arger_desc_string(&diagram_to, "diagram-to", "Select one shortest path to an exact node id (requires --diagram-focus)"),
    cmd_arger_desc_string(&diagram_direction, "diagram-direction", "Follow incoming, outgoing or both edges (default both; requires --diagram-focus)"),
    cmd_arger_desc_string(&diagram_depth, "diagram-depth", "Neighborhood/path hop limit 0..127 (default 1; requires --diagram-focus)"),
    cmd_arger_desc_flag_sh(&help, "help", "h", "Show help")
  };
  const uint32_t count = sizeof(arguments) / sizeof(arguments[0]);
  /* CmdParser owns parsing and generated help. Keep its terminating API outside
   * the driver so no Session, executor or workspace resources exist on exit. */
  cmd_arger_parse(arguments, count, NULL, 0, argc, argv, cli_description, cmd_arger_false);
  /* The SDK recognizes --help intrinsically; -h uses the same generated help. */
  if (help)
    cmd_arger_show_help_and_exit(arguments, count, NULL, 0, argv[0], cli_description, cmd_arger_false);
  *options = (turbo_cli_options_t){workspace, prompt, model, read_only, session_db, resume,
      diagram, diagram_focus, diagram_direction, diagram_depth, diagram_journals, diagram_origins, diagram_to, diagram_plan_steps,
      mode, instructions, knowledge_db, index_dir, vector_db, embedding_url, embedding_model, workflow};
}
