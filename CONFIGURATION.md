# Spark Configuration Reference

`config.example.json` shows common settings only. Advanced options below can
still be added to `config.json`; omitted fields use built-in defaults. Saving
omits default-valued advanced options, empty capability objects and empty
headers while preserving custom values. Saving removes `//` comments, so field
descriptions live in this document. The schema version remains `2`.

## Providers And Models

Settings supports adding, editing, duplicating, deleting and ordering
providers, connection testing and fetching model IDs. The composer menus can
select models and reasoning efforts across all configured providers.
`active` records the current default selection; it does not disable others.

| Field | When omitted | Description |
| --- | --- | --- |
| `providers.<id>.label` | Provider ID | Name shown in the interface |
| `providers.<id>.protocol` | `chat_completions` | Set explicitly when possible; supports `openai_responses` / `responses`, `openai_chat_completions` / `chat_completions`, and `anthropic_messages` |
| `providers.<id>.headers` | No extra headers | Map of header names to string values |
| `providers.<id>.test_model` | First model in the provider | Model used for the `hi` test; saving an empty value in the editor fills it with the first model ID |
| `providers.<id>.models[].display_name` | Model ID | Legacy compatibility field; model selection always displays only the model ID |
| `provider_order` | Sorted by provider ID | List of IDs saved automatically after drag reordering |

Set `base_url` to the provider's API address, `api_key` to its key or
`${ENV:VARIABLE_NAME}`, and `models[].id` to a real upstream model ID.
The endpoint, key and model must belong together.

The four boolean `capabilities` fields default to `true`; there is no need
to repeat them:

| Field | Purpose of an override |
| --- | --- |
| `streaming` | Set to `false` for an upstream that only supports complete responses |
| `tool_calls` | Set to `false` when the model does not support the tool protocol |
| `images` | Set to `false` to hide the `view_image` tool from the model; this is not an attachment permission switch, and manually attached images are still sent |
| `reasoning` | Set to `false` when the model does not accept reasoning effort parameters |
| `reasoning_efforts` | Empty by default, using built-in levels for recognized models; levels are not guessed for unknown models, so consult the upstream documentation. Custom lists for recognized models are restricted to supported levels |
| `default_reasoning_effort` | Empty means automatic resolution to the model's built-in default; chat selects and displays the resolved level without a separate automatic option. If a custom list excludes that default, `medium` is preferred, otherwise its first entry. No parameter is sent for an unknown model with no available levels. A valid override is used for new conversations and model switches |
| `context_window` | Model window in tokens; defaults to `0`, using the fallback window of `272000`. An empty editor field shows the current fallback as a muted placeholder. Use the provider's actual specification |
| `auto_compact_threshold_percent` | Defaults to `0`, inheriting the global percentage; set `1` through `100` to override the trigger for this model |

For example, add this member to a model object:

```json
"capabilities": {
  "context_window": 1000000,
  "auto_compact_threshold_percent": 80,
  "reasoning_efforts": ["low", "medium", "high"],
  "default_reasoning_effort": "medium"
}
```

Capabilities describe the request protocol. They do not bypass tool
permissions or give an upstream model capabilities it does not support.

## Automatic Compaction

Automatic compaction is always enabled and defaults to 90% of the current
model window. The editor's **Default (90%)** shows the current global percentage;
an advanced override changes that displayed value. Selecting the default does
not write a model-level override.

The same threshold is checked before every model request, including requests
after tool execution, and after a turn. Output headroom is reserved, and a known
model window is not capped by the global fallback window.

| `context` field | Default | Description |
| --- | --- | --- |
| `auto_compact_threshold_percent` | `90` | Percentage of the model window, from `1` through `100` |
| `fallback_context_window` | `272000` | Used only when the model window is unknown |
| `reserve_output_tokens` | `16384` | Output headroom in the input budget, not a forced response length |
| `summary_max_tokens` | `8000` | Target summary length requested in the prompt |
| `manual_compact_command` | `true` | Whether to accept `/compact`; does not affect automatic compaction |

The usual trigger is `min(model window * percentage, model window - output reserve)`.
Very small windows use an internal conservative budget when the full reserve
cannot fit. For a window of 1,000,000, a percentage of 90% and a reserve of
16,384, compaction triggers at roughly 900,000 tokens. Without an explicit
model window, 90% of the built-in `272000` fallback is `244800` tokens.

Local estimates, reported model usage and actual upstream limits may differ.
Compaction changes the model context while retaining the visible transcript;
summaries extract information and cannot guarantee lossless memory.

Legacy fields remain readable: `auto_compact_token_limit` is the old name for
`fallback_context_window`, and `post_turn_compact_threshold_percent` is the old
name for `auto_compact_threshold_percent`. New names take precedence, and saving
writes the new names. The old fixed limit no longer caps models with an explicit
window. The old percentage now applies consistently before requests and after
turns, instead of being multiplied into a budget already reduced by headroom.
`auto_compact: false` does not disable compaction and is removed when saved.

## Runtime Limits

These optional fields belong under `agent`. Existing custom timeout and output
limits remain supported.

| Field | Default | Description |
| --- | --- | --- |
| `max_iterations` | `100` | Maximum model-call iterations in a turn |
| `max_parallel_read_tools` | `4` | Maximum concurrent read tools; the old name `max_parallel_tools` is still accepted |
| `request_timeout_seconds` | `180` | Maximum idle time without new response stream data |
| `response_header_timeout_seconds` | `600` | Maximum wait for response headers |
| `tool_timeout_seconds` | `600` | Default command timeout; a command can override it and can still be interrupted |
| `max_tool_output_bytes` | `16384` | Ordinary tool output budget in bytes |
| `max_command_output_bytes` | `65536` | Command output capture budget in bytes |

A `run_command` call can specify `timeout_seconds`, capped at 24 hours;
an omitted or zero value uses `agent.tool_timeout_seconds`.

## Permissions

Normally only `permissions.default_mode` needs to be configured. Other
permission fields derive from the selected mode:

| Mode | Reads/writes outside the workspace | Write, command and MCP approval | HTTP MCP network |
| --- | --- | --- | --- |
| `ask` | Requires approval | Requests approval | Blocked |
| `approve_for_me` | Requires approval | Automatic approval | Blocked |
| `full_access` | Allowed | Automatic approval | Allowed |

Advanced overrides include `workspace.allow_outside_reads`,
`workspace.allow_outside_writes`, `workspace.file_write_approval`,
`workspace.command_approval` and `workspace.mcp_tool_approval`
(`deny` / `ask` / `allow`).

`permissions.allow_network` only controls HTTP MCP; it does not isolate the
system network access of ordinary commands or stdio MCP processes.
`permissions.workspace_only` is retained mode metadata; actual path checks
use `allow_outside_*`. Permission changes take effect immediately for
subsequent actions according to their current policy.

Changing unrelated preferences such as language preserves advanced overrides.
Explicitly choosing another permission mode applies that mode's policy again;
external MCP requests that specify another mode also use the session mode.
Plan mode continues to prohibit writes and execution. These are application
permissions, not an operating-system sandbox.

An active turn retains the permission snapshot taken at its start; changes
affect subsequent turns. MCP connection rebuilding waits for parallel work to
finish. Cached HTTP tools are filtered and checked against each turn's network
policy. Resumed full-access sessions and external sessions can use their
session network permission to discover HTTP MCP tools. External parallel
approval requests are presented individually; one approval applies only to the
request currently shown.

## Paths, Skills And Storage

`workspace.path` defaults to `${DOCUMENTS}/Spark`, the output root for
no-project tasks; it does not register a project with that name.
`storage.data_dir` defaults to `./data`, storing sessions, drafts and archives.
Relative paths resolve against the directory containing `config.json`;
the interface displays resolved absolute paths.
Supported variables are `${DOCUMENTS}`, `${APPDATA}`, `${LOCALAPPDATA}`,
`${EXE_DIR}` and `${CONFIG_DIR}`.

| Field | Default | Description |
| --- | --- | --- |
| `skills.enabled` | `true` | Enable skill discovery |
| `skills.roots` | `[]` | Additional scan roots; the example includes `./skills` |
| `skills.include_workspace_root` | `true` | Scan `<workspace>/.spark/skills` |
| `skills.include_user_root` | `true` | Scan `${APPDATA}/Spark/skills` |
| `skills.max_skills` | `64` | Maximum discovered skills |
| `storage.redact_secrets` | `true` | Redact known configured keys from tool output and compaction summaries |

The old `storage.log_level` never controlled log filtering and has been removed;
legacy values are ignored. Redaction is not encryption. Saving an environment
key reference preserves the `${ENV:VARIABLE_NAME}` expression.

## MCP

`mcp_servers` is empty by default. Enable each server explicitly:

```json
"mcp_servers": {
  "local": {
    "enabled": true,
    "transport": "stdio",
    "command": "./mcp/server.exe",
    "args": []
  },
  "remote": {
    "enabled": true,
    "transport": "streamable_http",
    "url": "https://your-mcp.example/mcp",
    "bearer_token_env_var": "MY_MCP_TOKEN"
  }
}
```

Advanced options include `required` (default `false`), `timeout_seconds`
(default 60 seconds), `startup_timeout_sec` / `tool_timeout_sec` (default `0`,
inheriting the common timeout), `enabled_tools` / `disabled_tools`,
stdio `cwd` / `env`, and HTTP `headers` / `http_headers` / `env_http_headers`.
Remote servers also require HTTP MCP network access. See [README](README.md)
for client and external subagent examples.

## Interface

`ui.language` defaults to `system`, falling back to English if no translated
language matches the system. `ui.theme` defaults to `dark`, with `light` and
`system` also available. `ui.shell` defaults to `cmd.exe` on Windows and
`/bin/sh` on Linux/macOS. Windows also supports `powershell.exe`. An explicit
shell setting is preserved across platforms. `ui.send_key` defaults to `enter`, with `ctrl+enter` as an
alternative. These settings can be changed directly in the interface.
