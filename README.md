# Spark

[简体中文](README.zh-CN.md)

Spark is a lightweight desktop AI coding agent built with C++17. It connects to
multiple API providers and runs tools locally, without a browser engine, Node.js,
Python or a local web server. Configured MCP servers may need their own runtimes.

> Early development. Linux, macOS and Windows XP ports are experimental.
> Download prebuilt packages from [Releases](https://github.com/EdwardKenwayl/Spark/releases).

## Platforms

| Platform | Package / interface |
| --- | --- |
| Windows x64 / x86 | `Spark-windows-x64.zip` / `Spark-windows-x86.zip`; Win32/GDI |
| Windows XP SP3 x86 | `Spark-windows-xp-x86.zip`; separate experimental build with bundled TLS |
| Linux x64 | `Spark-linux-x64.tar.gz`; GTK/wxWidgets 3.2 |
| macOS x64 | `Spark-macos-x64.tar.gz`; Cocoa/wxWidgets 3.2, macOS 12 or later |

All platforms share the agent runtime and session format. Native controls can
look different across systems. Linux packages depend on system libraries;
build on the oldest distribution you intend to support. Other architectures
require their own build and testing. Native GUI issues remain; a successful
build does not establish runtime compatibility.

## Quick Start

1. Extract the package into a writable directory and open `Spark.exe` (Windows),
   `Spark` (Linux) or `Spark.app` (macOS).
2. Open **Settings > Provider configuration**. Add an API address, key and model
   IDs, then use the `hi` test to check the connection.
3. Choose a model, reasoning effort and permission mode beside the send button.
   Open a project or start a conversation.

Windows keeps `config.json` beside the executable. Linux and macOS default to
the user's application-data directory under `Spark`; a configuration beside the
executable takes precedence. Use `--config /path/to/config.json` to override it.
Session data defaults to `data` beside the configuration.

To update, exit Spark and any MCP processes using its executables, then extract
the new package into the same directory. Release packages exclude `config.json`,
`data` and `workspace`. Back up these local files before sharing an installation.
API keys can be plaintext or environment-variable references.

## Features

- Background completion cards preview the final reply and open its chat when clicked, with a completion sound. MCP sessions and subagents stay silent.

- OpenAI Responses, Chat Completions and Anthropic Messages, with streaming,
  multiple providers and per-model settings.
- Concurrent chats, projects, drafts, interruption and continuation, archives
  and Codex conversation branch import/export.
- Compact live reasoning and command progress; recent chat history loads first,
  with earlier turns loaded on demand.
- Markdown, expandable tool activity, durations, usage statistics, file/image
  attachments, clipboard images and window snapshots.
- Local file and command tools, public web search, automatic context compaction,
  persistent goals and plan mode.
- MCP clients, Skills and project `AGENTS.md` rules; subagents can use different
  configured models. `SparkMCP.exe` lets other agents manage Spark chats.
- Ten interface languages with system-language detection and English fallback.

Images attach only to the message being sent. Window snapshots can be previewed
and removed before sending; background capture depends on system support and
permissions. Wayland capture is currently unavailable.
Request copies of supported images are resized and compressed; original files
stay unchanged. Compaction retains only the four most recent images.

Context compaction defaults to 90% of the model window; unknown windows use
272000 tokens. `/compact` triggers compaction and `/context` shows usage.
Compaction preserves the visible chat while summarizing model context.
When importing Codex branches, continuation uses the latest compaction checkpoint
and later messages. Reimport an original rollout to apply newer import fixes.

Permissions are enforced by the application, not an operating-system sandbox.
Plan mode allows built-in reads and searches and blocks actions that can modify
files or run external tools. Review tool requests and third-party MCP services.

## Configuration

Use the provider editor or [config.example.json](config.example.json).
The file accepts `//` comments; saving in settings writes standard JSON.
See [CONFIGURATION.md](CONFIGURATION.md) for defaults, advanced options,
permissions, MCP servers and storage paths.

Skills are discovered in `<workspace>/.spark/skills`, the user's
`Spark/skills` application-data directory and configured `skills.roots`.
Project rules load from `Spark/AGENTS.md` in the application-data directory,
then from the Git root down to the working directory. Spark does not load
Codex's global rules.

To expose Spark conversations to another MCP client, register `SparkMCP.exe`
with `--config /absolute/path/config.json` as a stdio server. Use `list_models`,
`create_subagent`, `send_subagent_message`, `wait_subagent`, `get_subagent`,
`list_subagents`, `approve_subagent_action` and `cancel_subagent`.
The service runs at most four conversations concurrently. A wait response with
`output_complete: false` is progress, not a finished result.

## Build

### Windows

Requires Visual Studio C++ tools (MSVC 14.38), CMake 3.20+ and Ninja.
From the source root:

```powershell
./scripts/build-win7.ps1 -Architecture x64
./scripts/package-portable.ps1 -Architecture x64
# Use -Architecture x86 for a 32-bit build.
./tests/package_tests.ps1
./tests/source_export_tests.ps1
```

The existing script name is retained. The build runs tests and checks executable
imports; packaging preserves existing configuration and data.

XP requires the separate v141_xp toolset and compatible TLS dependencies:

```powershell
./scripts/build-xp-tls.ps1
# Pass the generated curl SDK and CA bundle to build-xp.ps1.
Get-Help ./scripts/build-xp.ps1 -Detailed
```

Keep `cacert.pem` beside both XP executables. Test the resulting package on the
target system; host tests alone cannot establish XP compatibility.

### Linux and macOS

On Debian/Ubuntu, install `build-essential`, `cmake`, `libcurl4-openssl-dev`,
`libwxgtk3.2-dev`, `libxcomposite-dev` and `xvfb`.
On macOS, install Apple Command Line Tools, CMake and wxWidgets 3.2.
The dependency helper can build wxWidgets from a local source directory:

```sh
# macOS only; add /path/to/wx-sdk/bin to PATH afterwards.
sh scripts/build-macos-deps.sh /path/to/wxWidgets-3.2.8 /path/to/wx-sdk

# Linux or macOS:
sh scripts/build-native.sh
sh scripts/package-native.sh
```

macOS packaging applies an ad-hoc signature, not Apple notarization.
`build-platform-tests.sh` and `build-runtime-tests.sh` test the shared libraries
without building the GUI.

### Source Export

```powershell
./scripts/export-source.ps1
# Use -Force to refresh an existing clean export.
```

This creates `dist/Spark-source` and `dist/Spark-source.zip` from an allowlist.
Local configuration, conversations, build outputs and internal operating files
are excluded. The exporter refuses to overwrite uncommitted tracked edits.

## Source Layout

| Directory | Contents |
| --- | --- |
| `src/core` | Agent loop, protocols, configuration, sessions, tools and MCP |
| `src/gui` | Windows and native interfaces, shared transcript logic |
| `src/platform` | Files, networking, processes and desktop integration |
| `src/render` | Markdown parsing and rendering helpers |
| `resources` | Icons and localization |
| `tests`, `scripts` | Regression tests, build and packaging tools |
| `third_party` | Bundled dependencies and their licenses |

## Third-Party Notices

Spark bundles nlohmann/json, md4c and ghc::filesystem. Platform builds also use
libcurl, Mbed TLS or wxWidgets where applicable. Keep their required notices and
licenses when redistributing. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
