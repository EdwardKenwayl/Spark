# Spark

[Simplified Chinese](README.zh-CN.md)

> Early development. Download prebuilt packages from [Releases](https://github.com/EdwardKenwayl/Spark/releases).
> The cross-platform ports are experimental and known issues remain.

Spark is an independent desktop AI coding agent built with C++17. The Windows
interface uses Win32/GDI; the Linux and macOS ports use native wxWidgets controls
with the same sidebar, conversation, composer and full-page settings structure.
Configure multiple API
providers and models, then switch between them in the chat interface.

The application does not require Codex, the Codex app-server, a browser engine,
Node.js, Python or a local HTTP application server at runtime. User-configured
MCP servers may have their own runtime requirements.

## Platform Builds

| Target | Interface and requirements |
| --- | --- |
| Windows x64/x86 | Existing Win32/GDI interface, Windows 7 SP1 or later |
| Linux x64 | Native GTK/wxWidgets 3.2 interface; build on the oldest distribution you intend to support |
| macOS x64/arm64 | Native Cocoa/wxWidgets 3.2 interface, macOS 12 or later; build each architecture on macOS |
| Windows XP SP3 x86 | Separate experimental build using v141_xp and bundled curl/Mbed TLS |

The ports share the agent runtime, protocols, tools, sessions and Markdown parser.
Native Linux/macOS controls are not pixel-identical to the Windows interface.
An architecture name describes the binary; it does not certify that every system
using that architecture is supported. See the verification status below before
choosing a package; a successful build alone is not a guest runtime check.

On Debian/Ubuntu, install `build-essential`, `cmake`, `libcurl4-openssl-dev`,
`libwxgtk3.2-dev`, `libxcomposite-dev` and `xvfb`, then run:

```sh
sh scripts/build-native.sh
sh scripts/package-native.sh
```

Linux packages use system GTK, wxWidgets and libcurl libraries. They are not
self-contained AppImages. On macOS, install Apple Command Line Tools and CMake.
Build wxWidgets 3.2.8 with `sh scripts/build-macos-deps.sh /path/to/wxWidgets-3.2.8 /path/to/wx-sdk`,
add `/path/to/wx-sdk/bin` to `PATH`, then run the same native build and packaging
scripts. The macOS build uses system libcurl. Open `Spark.app` from the generated
archive. Packaging applies an ad-hoc signature to the finished app; it does not
perform Apple notarization. The default deployment target is macOS 12.

On Linux/macOS, configuration defaults to the current user's application-data
directory under `Spark`. A `config.json` beside the executable takes precedence.
Pass `--config /absolute/path/config.json` for a different configuration.
Background window snapshots use XComposite on X11 or macOS Screen Recording
permission; the snapshot menu is disabled on unsupported Wayland sessions.

The experimental XP entry point is `scripts/build-xp.ps1`. It rejects modern
MSVC toolsets and requires an x86 libcurl SDK with asynchronous DNS and an
XP-compatible TLS backend, plus a PEM CA bundle. It does not download these
dependencies or turn a Windows 7 executable into an XP executable.
Build the pinned TLS dependencies first with `scripts/build-xp-tls.ps1`; pass the
installed SDK and `cacert.pem` to `scripts/build-xp.ps1`. Package its `xp-stage`
directory with `scripts/package-portable.ps1 -WindowsXP -Architecture x86 -BuildDirectory ./build-xp-x86/xp-stage`.
The certificate bundle must remain beside both XP executables.

## Quick Start

1. Extract `Spark-windows-x64.zip` (64-bit Windows) or
   `Spark-windows-x86.zip` (32-bit Windows) into a writable directory.
   Both require Windows 7 SP1 or later, including Windows 11 on x64 systems.
   These are portable packages: extract and run, with no installer.
2. Start `Spark.exe`. If `config.json` is missing, Spark creates it beside the
   executable without replacing an existing configuration.
3. Open **Settings > Provider configuration** to add a provider, API address,
   API key and model IDs. You can also edit `config.json` using the English
   comments in `config.example.json`.
4. Select a model, reasoning effort and permission mode beside the send button,
   then send a message. The provider page also offers a `hi` connection test.

The default session data directory is `data` beside the configuration file.
No-project task output goes under the current user's `Documents\Spark` directory.

To update, exit Spark and external MCP processes using its executables, then
extract the new portable package into the same directory. Release packages do
not contain `config.json`, `data` or `workspace`, so these private files are
preserved. API keys are stored as plaintext or environment-variable references;
the provider editor masks a key until you explicitly reveal it.

## Features

- Multiple providers, models and reasoning defaults in one configuration, with
  one API root and API key per provider.
- Streaming OpenAI Chat Completions, OpenAI Responses and Anthropic Messages.
- Provider creation, editing, duplication, deletion, ordering, model fetching
  and connection testing in native settings pages.
- Markdown rendering with CommonMark/GFM support through the bundled `md4c`
  parser, expandable command details, recorded durations and usage statistics.
- Projects, recent conversations, pinning, archiving, deletion and independent
  conversation drafts.
- Concurrent conversations, interruption and continuation, tray operation and
  single-instance activation.
- File and image attachments, clipboard images, smart window snapshots,
  persistent conversation goals and plan mode.
- Bounded local tools for listing, reading, literal search, atomic writes,
  patches, image viewing and supervised commands.
- Automatic context compaction with a per-model window and trigger percentage.
- MCP clients over stdio and Streamable HTTP, Skills and `AGENTS.md` rules.
- Internal subagents with independent model selection, plus `SparkMCP.exe` for
  other agents to create and manage Spark conversations.
- A summary menu grouping **Subagents** and **Sources** in a right-side tabbed
  panel. Child chats reuse the normal renderer; sources include image previews,
  provider web-search/citation metadata and MCP tool references.
- Codex conversation branch import and export.
- Ten interface languages: Simplified Chinese, Traditional Chinese, English,
  Japanese, Korean, German, French, Spanish, Brazilian Portuguese and Russian.
  **Auto-detect** is the default and follows the operating system's language, falling
  back to English when no translation matches. Manual selections stay fixed.

## Configuration

The schema version is `2`. Spark accepts `//` comments in configuration files;
the example can be used directly after filling in provider details. Saving from
settings writes standard JSON and removes comments. Default-valued advanced
options are omitted, while custom overrides are retained.

A minimal Responses provider looks like this:

```json
{
  "schema_version": 2,
  "active": {
    "provider": "my-provider",
    "model": "YOUR_MODEL_ID",
    "reasoning_effort": ""
  },
  "providers": {
    "my-provider": {
      "label": "My Provider",
      "protocol": "openai_responses",
      "base_url": "https://api.openai.com/v1",
      "api_key": "${ENV:OPENAI_API_KEY}",
      "models": [
        {
          "id": "YOUR_MODEL_ID",
          "capabilities": {
            "context_window": 272000,
            "auto_compact_threshold_percent": 90
          }
        }
      ]
    }
  }
}
```

`YOUR_MODEL_ID` is a placeholder. Replace it with an actual model ID supported
by your provider; `272000` is an example window, not a claim about that model.

| Field | Meaning |
| --- | --- |
| `active` | Current default provider, model and reasoning effort; other configured models remain selectable |
| `providers.<id>.protocol` | `openai_responses` / `responses`, `openai_chat_completions` / `chat_completions`, or `anthropic_messages` |
| `providers.<id>.base_url` | API root; the adapter adds `/responses`, `/chat/completions` or `/messages` as needed without duplicating the suffix |
| `providers.<id>.api_key` | Provider key or an expression such as `${ENV:MY_PROVIDER_KEY}` |
| `providers.<id>.models` | Actual upstream model IDs with optional context, compaction and reasoning overrides |
| `permissions.default_mode` | `ask`, `approve_for_me` or `full_access` |
| `workspace.path` / `storage.data_dir` | No-project output root and session data directory |

Relative paths resolve against the directory containing `config.json`. Supported
variables are `${DOCUMENTS}`, `${APPDATA}`, `${LOCALAPPDATA}`, `${EXE_DIR}` and
`${CONFIG_DIR}`. Settings display the resolved absolute paths.

`workspace.path` defaults to `${DOCUMENTS}/Spark`; it is not an automatically
registered project. A successful first turn in a no-project conversation
creates its own workspace:

```text
Documents\Spark\YYYY-MM-DD\chat-YYYYMMDD-HHMMSS-xxxxxxxx\work
Documents\Spark\YYYY-MM-DD\chat-YYYYMMDD-HHMMSS-xxxxxxxx\outputs
```

Project conversations use the selected project directory without rewriting
the no-project default. Older UI state does not create task folders until work
actually runs; conversations whose project was removed are shown without a project.

See [Configuration Reference](CONFIGURATION.md) for all defaults, advanced
overrides, permission behavior and legacy configuration migration.

## Chat And Attachments

- **New conversation** opens an empty draft; click a conversation to resume it.
  Projects and recent conversations are listed separately. Project controls
  let you register workspace folders; hover a conversation for its actions.
- `Enter` sends and `Shift+Enter` inserts a newline by default. Settings can
  switch sending to `Ctrl+Enter`. Enter during IME composition confirms the
  candidate rather than sending a message.
- The send button becomes a pause button during a turn. After interruption,
  its continuation action resumes the turn; sending new text starts a new one.
- **Attach files** in the `+` menu opens the platform file picker. The Windows
  picker also provides **Attach current folder** to add a folder reference.
- Image attachments appear above the composer, can be enlarged or removed, and
  are sent once rather than being attached to every later message.
- On Windows, switch to another application and press left and right Alt together to attach
  its window screenshot and title. Occluded windows can be captured without
  changing foreground focus; an isolated helper has a 3.5-second timeout.
  Minimized windows and applications that refuse window rendering are unsupported.
- Snapshots remain local draft attachments until sent. They can be previewed
  and removed, and the snapshot menu item is disabled without an available target.
- Goals and plan mode persist across sends, conversation switches and restarts.
  Goals are persistent model instructions and do not start turns automatically.
  Plan mode allows built-in reads/searches but blocks writes, patches, commands,
  MCP calls and child creation, including under full access.
- Settings contain **General**, **Provider configuration** and **Archived chats**,
  including archived conversation recovery and permanent deletion.

## Context Compaction

Automatic compaction is always enabled. By default it triggers at 90% of the
selected model's context window, subject to output headroom. An unknown window
uses the built-in fallback of `272000` tokens, whose 90% threshold is `244800`.
Each model can override its window and percentage.

The same compactor checks before model requests, including requests after tool
execution, and after a turn. It records `context.compacted` with before/after
token estimates, trigger reason, model and duration.

The model creates a bounded handoff containing goals, preferences, completed
work, modified files, current state, evidence and next steps. Oversized contexts
use multiple summary passes in order; a failed model summary falls back to a
local bounded handoff. Compaction changes the context sent to the model while
retaining the visible transcript. Summaries do not guarantee lossless memory.

Use `/compact` for immediate compaction or `/context` to inspect the estimated
usage and effective threshold without making a model request.

## MCP And Skills

### MCP Clients

Configure servers under `mcp_servers`:

```json
{
  "mcp_servers": {
    "local": {
      "enabled": true,
      "transport": "stdio",
      "command": "./mcp/server.exe",
      "args": ["--stdio"],
      "startup_timeout_sec": 20,
      "tool_timeout_sec": 120
    },
    "remote": {
      "enabled": false,
      "transport": "streamable_http",
      "url": "https://mcp.example/mcp",
      "bearer_token_env_var": "MY_MCP_TOKEN"
    }
  }
}
```

Tools appear as `mcp__<server_id>__<tool_name>`. HTTP MCP requires network
permission; stdio starts a child process. Optional headers, environment
variables, tool filters, required servers and timeouts are documented in
[Configuration Reference](CONFIGURATION.md). MCP metadata is untrusted; a
server's claimed read-only status does not bypass approval.

### Skills

Spark discovers folders containing `SKILL.md` from:

- `<workspace>/.spark/skills`.
- `${APPDATA}/Spark/skills`.
- Additional `skills.roots`, such as `["./skills"]`.

The first two roots can be disabled independently. Discovery is limited to
64 skills by default. YAML front matter supports `name`, `description`,
`short_description`, `version` and `allowed-tools`.

The skill catalog contains absolute paths. The agent loads a body or relative
resource on demand with `read_skill`; `$skill-name` explicitly loads a skill
for that turn. Skill content cannot override system instructions or permissions.

## Subagents

### Inside Spark

`list_models` lists configured provider/model pairs and effective reasoning
defaults without exposing keys or URLs. Paginated results stay below 4 KiB.

`spawn_agent` accepts `name`, `task` and optional `provider`, `model` and
`reasoning_effort`. Specify provider and model together for another provider.
Omitted model selection inherits the parent model; a different model with no
explicit effort uses its effective default. Invalid models or unsupported
efforts are rejected before starting.

Children inherit the workspace, MCP connections and permission policy, but use
their selected provider's key, endpoint and protocol. Details show the actual
provider/model. Up to four internal children run concurrently; they cannot
create another layer of children. Child selection does not change the parent's
model or `config.json`, and completed child metadata survives restart.

### From Other Agents

`SparkMCP.exe` is an independent stdio JSON-RPC MCP server using the same
configuration. It provides:

```text
list_models
create_subagent
send_subagent_message
wait_subagent
approve_subagent_action
get_subagent
list_subagents
cancel_subagent
```

For example, register it in Codex using absolute paths:

```toml
[mcp_servers.spark]
command = "E:\\Path\\To\\Spark-windows-x64\\SparkMCP.exe"
args = ["--config", "E:\\Path\\To\\Spark-windows-x64\\config.json"]
startup_timeout_sec = 20
tool_timeout_sec = 600
```

`create_subagent` accepts `task` and optional `name`, `provider`, `model`,
`reasoning_effort`, `workspace` and `permission_mode`. The default `read_only`
mode rejects writes, commands and MCP calls. Other modes are `ask`,
`approve_for_me` and `full_access`. A child has its own worker, cancellation
token, permission mode and journal under `storage.data_dir\sessions`.
One server process permits four running children and 64 total child records.

When `workspace` matches a registered project path, the conversation appears
under that project; otherwise it appears under recent conversations.
An explicit name becomes the title; otherwise a short title is derived from
the task.

`wait_subagent` waits up to 120 seconds by default, with a maximum of 600 seconds.
`output_complete: false` means the result is a progress snapshot; wait again.
In `ask` mode, inspect `awaiting_approval`, `approval_kind`, `approval_title` and
`approval_detail`, then call `approve_subagent_action` with the child `id` and
`approved: true` or `false`. `approve_for_me` approves actions permitted by its
policy; `full_access` applies the explicitly requested unrestricted mode.

## AGENTS.md Rules

Instructions load in this order:

1. `%APPDATA%\Spark\AGENTS.md`.
2. Each `AGENTS.md` from the nearest Git project root down to the active workspace.

More specific files specialize broader project rules. Loading is capped at
32 KiB per file and 128 KiB total for both GUI and external child conversations.
These rules do not override permission policies.

Spark does not automatically read Codex's global rules from `CODEX_HOME` or
`%USERPROFILE%\.codex`. `CODEX_HOME` is used only for Codex session interchange.

## Codex Session Interchange

- Right-click a conversation and select **Export as Codex chat branch** to
  create a rollout JSONL branch retaining messages, reasoning, tool calls and
  results. The dialog starts in `%CODEX_HOME%\sessions`, falling back to
  `%USERPROFILE%\.codex\sessions`.
- Select **Import Codex chat branch** from the recent conversations menu, or
  from a project's menu above **Archive chats**, to import into that scope.
- Import creates a new Spark conversation and does not modify the source rollout.

## Build And Package

Build on a Windows x64 development machine with:

- Visual Studio 2022 C++ tools and **MSVC 14.38**.
- CMake 3.20 or newer and Ninja.

The source includes nlohmann/json and md4c; it does not require the full Codex
repository or additional downloads of these libraries. End users do not need
build tools on their Win7 machines.

Run PowerShell from the source root:

```powershell
.\scripts\build-win7.ps1
.\scripts\package-portable.ps1

# Same GUI and features, targeting 32-bit Windows 7 SP1 or later.
.\scripts\build-win7.ps1 -Architecture x86
.\scripts\package-portable.ps1 -Architecture x86

# Packaging regression checks; no API calls or private configuration needed.
.\tests\package_tests.ps1
```

The build script discovers Visual Studio with `vswhere`, finds CMake and Ninja,
uses one compilation job, performs a clean build, runs local tests and checks both
executables for known incompatible imports. Customize tool locations with
`-VisualStudioDirectory`, `-CMakePath` and `-NinjaPath`; use `-BuildDirectory`
and `-ParallelJobs` for the output directory and concurrency. The default x64
build directory is `build-win7`; x86 uses `build-win7-x86`.

If execution policy blocks the script, use a per-invocation override:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build-win7.ps1
```

The `Spark-windows-x64.zip` / `Spark-windows-x86.zip` packages and their `.sha256`
files are written under `dist`. Packaging
updates only public files and verifies that existing configuration, data and
workspace hashes have not changed. Packaging also checks each executable's PE
machine type before updating the architecture-specific release directory.
The compatibility probe stays in the build
directory and is not included in the portable release.
Existing `Spark-portable-win7-*` runtime directories are updated in place
without moving configuration or breaking registered MCP executable paths.
Their old ZIPs and checksums are removed only after the renamed package succeeds.

Export a clean source directory and archive:

```powershell
.\scripts\export-source.ps1
```

This creates `dist\Spark-source`, `dist\Spark-source.zip` and its `.sha256`.
Use `-Force` to replace a previous generated export. It contains source, tests,
resources, scripts, examples and third-party notices, excluding private
configuration, conversations, build outputs, logs and internal development notes.

### Platform Foundation

The shared foundation includes configuration,
user directories, Unicode file I/O, sessions, Skills, project rules, context
compaction calculations and the existing streaming Markdown parser. It can be
built without the Windows GUI or network/process runtime:

```sh
sh scripts/build-platform-tests.sh
```

This uses one compilation job and temporary test data. On Linux, user directories
follow `HOME` and XDG settings; on macOS they use `Documents` and
`Library/Application Support`. An omitted shell defaults to `/bin/sh` there,
while Windows keeps `cmd.exe`. Explicit configuration values remain unchanged.
The portable HTTP/provider layer can also be built separately. Linux/macOS
require libcurl 7.68 or later with TLS support and asynchronous DNS, its development
headers and the system CA certificates. On Debian, install
`libcurl4-openssl-dev` and `ca-certificates`, then run:

```sh
sh scripts/build-runtime-tests.sh
```

Windows 7 and later keep WinHTTP; XP and Linux/macOS use libcurl with certificate and hostname
verification. Local fixtures exercise Chat Completions, Responses and Anthropic
Messages, streaming, binary image attachments, cancellation and separate
header/stream-idle deadlines. Responses retain at most 1 MiB for diagnostics;
stream callbacks receive the full body. The POSIX backend rejects redirects
instead of forwarding API credentials to another address.

These focused builds exercise libraries only. Use `scripts/build-native.sh` for
the complete Linux/macOS application, including the native GUI, agent runtime,
process execution, MCP and desktop integration.

## Source Layout

| Directory | Contents |
| --- | --- |
| `src/core` | Agent loop, protocols, configuration, sessions, tools, compaction, MCP, Skills and subagents |
| `src/gui` | Native chat, sidebar, settings and summary panels |
| `src/platform` | Native paths, user directories, journal/binary I/O, WinHTTP/libcurl and Windows text/window capture helpers |
| `src/render` | Markdown parsing and rendering helpers |
| `resources` | Application icons and Windows resources |
| `resources/locales` | Embedded translation JSON catalogs; tests check coverage, placeholders and code references |
| `tests` | Local mock services and regression tests |
| `scripts` | Build, release packaging and source export |
| `third_party` | Bundled libraries and their original licenses |

## Compatibility And Boundaries

Windows 7 SP1 x64 and x86 are the compatibility targets. Both use the same GUI
and agent code. The build uses PE subsystem
`6.01`, `_WIN32_WINNT=0x0601`, `WINVER=0x0601` and static CRT linking.
TLS certificate and hostname verification remain enabled.

The build rejects known incompatible imports such as
`GetSystemTimePreciseAsFileTime`, `GetDpiForWindow` and UCRT API sets. The
separate probe exercises startup, configuration, verified HTTPS, streaming,
subprocesses and cancellation. Compilation and import checks do not replace
real Win7 testing; system patches, TLS, providers and external MCP dependencies
can affect runtime behavior.

| Platform | Current status |
| --- | --- |
| Windows 7 SP1 or later, x64 | Built; 21 host test suites and import audit passed |
| Windows 7 SP1 or later, x86 | Built; 21 host test suites and import audit passed; 32-bit address-space limits apply |
| Windows XP SP3 x86 | Both executables built with v141_xp; import audit and 10 host test suites passed; guest runtime validation pending |
| Linux x64 | Native GUI and complete runtime built on Debian; 11 suites, GUI fixtures and isolated startup passed |
| macOS 12 or later, Intel x64 | Native build completed; 10 CTest suites, manual Cocoa GUI fixture, signed archive check and isolated Monterey startup passed |

The `windows-x86` package requires Windows 7 or later. XP uses the separate
`windows-xp-x86` package and bundled TLS stack. Linux's verified build uses
Debian 13 and its system libraries; it does not establish compatibility with
older distributions. ARM builds require a separate native build and verification.

For an isolated VM check, extract the matching ZIP inside the guest into a
writable local folder, start `Spark.exe`, configure a test provider and send a
short message. Then check settings, scrolling, interruption, conversation
switching and restart persistence. Test only one VM at a time. Start with the
x64 package in a 64-bit Windows guest; compare x86 in a separate folder rather
than sharing live session data. Compilation and host tests alone are not guest
runtime validation.

File tools have entry, depth, duration, size and result caps and skip reparse
points and known system directories. Command cancellation and timeouts use a
Windows Job Object where available to terminate the owned process tree.
Linux/macOS use an owned subprocess group for cancellation and cleanup.
Bounded command output retains both its beginning and end. Uncertain commands
are not silently replayed.

Permissions are enforced by the application, not an operating-system sandbox.
MCP OAuth, arbitrary restart policies, automatic cross-provider failover,
automatic goal continuation, goal completion tools and goal token budgets are
not implemented. Windows releases are portable ZIPs; Linux/macOS use tar archives.
No platform installer is included.

## Third-Party Notices

The bundled libraries are nlohmann/json 3.11.3 and the md4c CommonMark/GFM parser.
Their original license texts are in `third_party/nlohmann/LICENSE.MIT` and
`third_party/md4c/LICENSE.md`, and are also included in portable releases.
See [Third-Party Notices](THIRD_PARTY_NOTICES.md).
