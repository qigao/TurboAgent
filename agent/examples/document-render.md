# Bounded document rendering Host example

This is the first native-tool slice of the [Skill/workflow package proposal](https://github.com/qigao/TurboAgent/pull/225).
It demonstrates explicit registration, Skill projection, a canonical input/result contract and reference identity checks.
It adds no installed API, package loader, YAML execution backend or default CLI tool.

`document_render_tool.h` is an example implementation shared by the executable and the offline Harness tests.
`document_render_register()` borrows a Host-owned, immutable catalog of selected source IDs and revisions.
The catalog, its strings and implementation must outlive every registry, selection, Agent and compiled plan using them.
Do not refresh the catalog or replace tool policy inside a callback; drain execution before a quiescent replacement.
The example uses fixture revisions; a real Host must bind the actual selected source snapshots and explain stale sources.

The tool accepts a title and one to eight plain-text passages, each with one source ID from that catalog.
The title is limited to 160 UTF-8 bytes, each passage to 2048 bytes, IDs/revisions to 64 ASCII letters/digits/underscores/hyphens,
and the catalog to 16 unique IDs. The callback enforces byte limits in addition to schema character limits.
It produces at most 40000 Markdown bytes, escaping draft punctuation and appending Host source IDs/revisions.
There is no file, process, network, credential or model access. A Host export is a separate authorized operation.
Rendering in a web UI still requires the Host's own safe rendering policy.

`verification: reference_identity_only` means the cited IDs resolve to the selected Host catalog.
It does **not** prove that a passage is factually supported by its source, that the source is current,
or that the user's task is complete. Content assessment and exact evidence locators remain separate work.

To use the executable, put this Skill at `skills/document-report/SKILL.md` under a dedicated workspace:

```markdown
---
name: document-report
description: Prepare a report from Host-selected sources
tools: [document_render]
capabilities: [runtime_tools]
---

Collect the writing goal and use only Host-selected source IDs. Draft plain text,
call document_render, and show unsupported claims and missing information.
Reference identity verification is not factual verification. Request an explicit
Host export when saving is required.
```

Enable the repository's existing `BUILD_EXAMPLES` option with a configured SDK preset and build normally;
the Agent example glob creates `workspace_document_render`. Run it with the absolute workspace directory.
It renders a fixed, offline fixture with source ID `brief` and revision `fixture-v1`, then writes Markdown to stdout.
It has no model connection or automatic file export. The Host grants only `runtime_tools` before selection,
explicitly checks policy before its direct RuntimeTools call, and explicitly validates the canonical result afterward.
Direct RuntimeTools does not itself provide Agent approval/journal/compiler guarantees.

`test_turbo_document_render_example.c` is included in the normal Harness CTest suite.
It checks rendering/escaping and UTF-8, embedded NUL, invalid sources and parameters, ambiguous catalogs, Skill grant projection,
and the same tool under compiled Inspect: default denial, committed replay without callback re-entry,
quiescent same-grant generation invalidation, revoked result delivery and invalid-result rejection.
These tests do not extend compiled Inspect guarantees to arbitrary ordinary-loop tools or multi-step workflows.
