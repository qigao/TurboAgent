# TurboAgent Praktor Tools

`TurboAgent::PraktorTools` exposes reviewed [Praktor](https://github.com/qigao/praktor)
YAML workflows as normal TurboAgent tools.

The boundary is intentionally narrow:

```text
LLM / TurboAgent Harness
          |
     Tool Registry
          |
   praktor_<capability>
          |
   fixed workflow path
          |
     Praktor C ABI
          |
       YAML DAG
```

The model never supplies a workflow path. The host registers an absolute path,
input schema, execution policy, and policy capabilities before the agent starts.

## Build

Praktor support is optional so existing TurboAgent builds do not gain a new
mandatory dependency:

```cmake
-DENABLE_PRAKTOR_TOOLS=ON
-DPRAKTOR_ROOT=/path/to/praktor/install
```

When enabled, configuration fails fast if `find_package(Praktor CONFIG REQUIRED)`
cannot resolve the installed package.

## Register a workflow

```c
turbo_praktor_tool_pack_config_t pack_config;
turbo_praktor_workflow_config_t workflow;
turbo_praktor_tool_pack_t *pack;

turbo_praktor_tool_pack_config_init(&pack_config);
pack = turbo_praktor_tool_pack_create(&pack_config);

turbo_praktor_workflow_config_init(&workflow);
workflow.tool_name = "praktor_build";
workflow.description = "Build and test the current project.";
workflow.workflow_path = "/opt/workflows/build.yml";
workflow.parameters_json =
    "{\"type\":\"object\",\"properties\":{\"preset\":{\"type\":\"string\"}},"
    "\"required\":[\"preset\"],\"additionalProperties\":false}";

turbo_praktor_tool_pack_add_workflow(pack, &workflow);
```

The pack registry can be attached directly to an agent or composed with MCP,
Wasm, coding, or native registries through `turbo_tool_registry_compose()`.

## Security boundary

The default workflow configuration requires:

- `runtime_tools`
- `network`
- `shell`
- `patch`
- `outside_workspace`

This is intentionally conservative because a Praktor workflow can contain
commands, HTTP/file operations, scripts, and native calls. A host may replace
the additional capability list only after reviewing the registered YAML and its
transitive `uses` workflows.

An agent cannot override the registered path or capability metadata through
tool arguments.

## Result semantics

Praktor success and workflow-level failure both return Praktor's canonical JSON
object to the model. A workflow failure therefore remains inspectable through
`workflow_status`, task states, outputs, and `error` instead of being reduced
to a generic tool failure.

Adapter failures such as an oversized or malformed result remain tool errors.
