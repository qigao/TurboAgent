# TurboAgent CLI

The installed `turboagent` executable uses the existing Harness, Session loop,
Provider/CHttp transport and CodingTools pack. Model settings come from the
existing `OPENAI_*` / `ANTHROPIC_*` environment configuration and optional `.env`.

```sh
turboagent -C /absolute/workspace -p "Inspect the failing tests and fix the cause"
turboagent -C /absolute/workspace
turboagent --read-only -p "Explain this repository"
```

`-C` / `--workspace` defaults to the current directory and accepts relative paths.
`-p` / `--prompt` runs one task; otherwise each input line starts a user turn.
`--model` overrides the environment model, `/quit` or EOF exits, and Ctrl-C requests
cooperative cancellation. Agent output goes to stdout; interactive prompts and
execution failures go to stderr.
The default HTTP transport checks cancellation and the execution deadline between
poll intervals of at most 50 ms, then cancels and drains the request on its owner
thread. A server response is not required to finish cancellation. This is not a
hard real-time latency guarantee: connection cleanup can take additional time.
Explicitly supplied blocking HTTP clients and custom transport functions still
depend on their callback returning; the CLI does not forcibly interrupt them.

Failed runs keep exit code 1 (130 for Ctrl-C) and now append a bounded diagnostic
with the known failure phase, run code, execution status, HTTP status and transport status. HTTP
401/403/404/429/5xx include a fixed configuration/access/rate-limit/server hint.
Provider response bodies, credentials and arbitrary phase text are not printed.
The default HTTP path records each call in the existing `provider_attempts`
state array and remains single-attempt; it does not automatically replay a task.
Custom v1 callbacks have no structured HTTP metadata, so their HTTP status is 0.

Only a `completed` execution can print an answer or become conversation history.
Runtime return code 0 may also represent a successfully recorded cancellation,
deadline expiry or workflow pause; those outcomes exit with 1 even if their state
contains model text. Ctrl-C exits with 130, including a signal observed after the
worker completes but before its answer is printed. Diagnostics distinguish these
outcomes without printing partial model text.

Argument parsing and generated help use the SDK's `Salts::CmdParser`
(`cmd_arger.h`). Long options accept both `--model name` and `--model=name`;
use the short aliases separately (`-C path`, `-p text`). CmdParser also expands
`@file` arguments and loads the current directory's optional `.env` before
parsing. Use `--prompt=@text` for a literal prompt starting with `@`.
The CLI adopts CmdParser's current process behavior: `--help` / `-h` and syntax
errors print to **stdout and exit with 1**, replacing the earlier help exit 0
and syntax-error exit 2/stderr behavior. Driver validation errors (such as an
empty prompt or invalid workspace) still exit with 2 and report to stderr.
Parsing occurs before any Agent resources are created; the execution driver
receives borrowed parsed options and does not call the terminating parser API.

Default tools are `fs.read`, `fs.list`, `fs.search`, `fs.write`, `fs.edit`, and `shell.exec`.
File operations use retained workspace root capabilities; `fs.list` and `fs.search` accept `.`.
Search takes a literal `pattern` and a file/directory `path`, returns matching
lines, and reports limits/skips through `truncated` and metrics.
Shell commands have host filesystem/network access and inherited environment.
`--read-only` registers the three reading tools and denies patch/shell policy.
See [tool ownership, limits and failure semantics](../coding_tools/README.md).

Each user turn loads the selected workspace's root `AGENTS.md` and selects
matching entries from its `skills/` catalog using the existing Agent Workspace
module. The catalog is refreshed between turns, so later prompts see updated
instructions and select skills for the new task. This does not search parent
directories or apply every nested directory's instructions globally.
Missing `AGENTS.md` or the default `skills/` directory is allowed. Malformed
skill metadata, oversized files, unavailable declared tools, and denied skill
capabilities stop the turn before a model request. `--read-only` also denies
skills requiring patch or shell capabilities.

The CLI owns the workspace catalog on its input thread. Preparation validates
skill tools against the same registry used for execution; it does not narrow
the default coding tool set. The selected text is copied into a project memory
context layer on the fresh turn state, then the temporary selection is freed.
Harness copies that state for its worker. After the worker drains, the next
turn projects conversation history without carrying forward old instruction
layers and refreshes the catalog. Workspace defaults bound composed text to
256 KiB, each skill to 128 KiB, the catalog to 128 skills, selection to 16 skills,
and discovery depth to 4. No second session store or instruction parser is added.

The initial CLI is intentionally an in-memory session with bounded input and
execution: 64 KiB per prompt, 1 MiB projected conversation input, 64 user turns,
128 graph steps and 10 minutes per turn. Exceeding a bound fails explicitly.
It prints completed answers rather than incremental tokens. It does not expose
persistent resume, approval UI, or interactive TUI controls. When building with
CodingTools disabled, the CLI remains available without workspace tools.

Follow-up turns use `turbo_agent_session_create_follow_up_state_json_value` to
project the completed model/tool history before appending the new user message.
This is a new loop run, not an interrupted-workflow resume. It retains full
history under the host limit rather than reusing a stale compaction event offset.
The Session's Runtime remains the record owner; the CLI retains only the current
completed state. After the single worker drains, the CLI queries its terminal
status and moves the summary/state exactly once. It frees the summary and every
rejected result; only a completed answer transfers state into the next turn.
The execution handle remains retained through this consumption boundary. No
stored-data format changes are required.

Formal tests inject the existing transport callback and execute actual tools in
a temporary workspace. They cover write/edit/read/command execution, second-turn
history order, read-only schema exposure, one-shot mode, model overrides, and
CmdParser defaults/aliases/equals syntax. Child-process tests verify generated
help, rejected arguments, output streams and exit codes
without making a paid model request.
Workspace tests cover instruction refresh, skill reselection without stale
layers, read-only capability checks, missing tools, malformed/oversized files,
and failure before model invocation, including a failed refresh on a later turn.
`test_turbo_cli_diagnostics` uses a local CHttp server to check failure categories,
response-body and terminal-control-character isolation, and preserved stdout on
a failed second turn. These tests do not require a model or credentials.
`test_turbo_cli_completion` runs the real asynchronous Harness to cover late
model responses after cancellation, another run after cancellation, deadline
expiry, workflow pause, a signal observed at completion, and one-time result
consumption. `test_turbo_agent_http_cancel` holds a local server response open to
verify user/shutdown cancellation and in-flight deadlines, then resumes the same
Session on another worker. It checks that cancellation commits no model response
and admits no retry; a second POST occurs only after explicit resume.
`test_turbo_cli_tool_cancel` runs a real command through Harness, cancels it
after stdout/stderr and a side effect have been produced, and checks that the
child is gone before completion. User/shutdown/deadline cancellation retains
bounded process detail in the existing tool journal. Explicit checkpoint resume
does not launch the command again or expose host-only detail to the model.
Pre-spawn deadlines, binary capture and legacy context compatibility are also
covered by the CLI/CodingTools suites. This does not add a CLI resume command.

## Optional live model verification

`test_turbo_cli_live` is a separate CTest integration test. It is skipped unless
`TURBOAGENT_LIVE_TESTS=1`; having a key in `.env` alone never enables it. It launches
the real CLI with the repository root as its initial working directory so
CmdParser can load that `.env`. `OPENAI_BASE_URL`, model and provider settings
follow the CLI's normal environment configuration. For a local proxy, use the
client key accepted by that proxy. Python 3 must be available as `python` on PATH.
The suite first checks a general question through synchronous Session and
background Harness execution, then exercises the coding CLI.

```powershell
$env:TURBOAGENT_LIVE_TESTS = "1"
# Example for an OpenAI-compatible local Chat Completions proxy:
$env:OPENAI_BASE_URL = "http://localhost:64263/v1"
$env:OPENAI_PROVIDER = "openai_compatible_chat_completions"
$env:OPENAI_ENDPOINT_PATH = "chat/completions"
# Set OPENAI_MODEL if the proxy requires a different model.
ctest --preset win-release-user -R '^test_turbo_cli_live$' --output-on-failure
Remove-Item Env:TURBOAGENT_LIVE_TESTS
```

The test creates a temporary Python program with a known off-by-one bug and
first confirms its four tests fail. The model must inspect/edit the program and
run the tests through workspace tools. The test then checks that the test source
was not modified, checks the success artifact and independently reruns the tests.
Only this generated fixture is sent as task content; no repository source or
`.env` is copied into its workspace. The CLI retains its ordinary host-shell
capabilities. Process output is captured without printing it into CTest logs,
and the coding child process is bounded to three minutes. Temporary files are
removed after the process tree stops on normal test completion or assertion
failure; external termination of the test runner can leave the fixture behind.

The live test can incur model usage charges and depends on endpoint availability,
credentials, model access and model behavior. A passing small repair task does
not establish success rates on larger repositories. Run normal regression without
`TURBOAGENT_LIVE_TESTS` to keep it offline.
