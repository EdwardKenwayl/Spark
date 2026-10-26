# Spark

[English](README.md)

> 目前处于早期开发阶段，可在 [Releases](https://github.com/EdwardKenwayl/Spark/releases) 下载预编译发行包。
> 跨平台移植仍属实验阶段，存在已知问题。

Spark 是使用 C++17 构建的独立桌面 AI 编程智能体。Windows 界面继续使用 Win32/GDI；Linux 和 macOS
移植版使用 wxWidgets 原生控件，保留侧边栏、对话、发送框和整页设置的布局。
可配置多家 API 供应商和多个模型，并在聊天界面中切换。

程序运行时不需要 Codex、Codex app-server、浏览器内核、Node.js、Python
或本地 HTTP 应用服务。用户自行配置的 MCP 服务可能有自己的运行依赖。

## 平台构建

| 目标 | 界面与要求 |
| --- | --- |
| Windows x64/x86 | 原有 Win32/GDI 界面，Windows 7 SP1 或更新系统 |
| Linux x64 | GTK/wxWidgets 3.2 原生界面；应在计划支持的最旧发行版上构建 |
| macOS x64/arm64 | Cocoa/wxWidgets 3.2 原生界面，最低 macOS 12；在 macOS 上分别构建各架构 |
| Windows XP SP3 x86 | 独立实验性构建，使用 v141_xp 及随包提供的 curl/Mbed TLS |

各平台共享智能体运行时、协议、工具、会话和 Markdown 解析器。
Linux/macOS 原生控件与 Windows 界面不会逐像素一致。架构名称表示二进制
格式，不代表所有采用该架构的系统都兼容。选择发行包前请查看下方的验证状态；
编译成功本身不代表已通过来宾系统运行验证。

Debian/Ubuntu 安装 `build-essential`、`cmake`、`libcurl4-openssl-dev`、
`libwxgtk3.2-dev`、`libxcomposite-dev` 和 `xvfb` 后执行：

```sh
sh scripts/build-native.sh
sh scripts/package-native.sh
```

Linux 包使用系统的 GTK、wxWidgets 和 libcurl，并非自包含的 AppImage。
macOS 安装 Apple Command Line Tools 和 CMake 后，
用 `sh scripts/build-macos-deps.sh /path/to/wxWidgets-3.2.8 /path/to/wx-sdk`
构建 wxWidgets 3.2.8，将 `/path/to/wx-sdk/bin` 加入 `PATH`，
再执行相同的原生构建与打包脚本。macOS 使用系统 libcurl。
解压生成的包并打开 `Spark.app`。打包脚本会对完成的应用进行本地临时签名，
不包含 Apple 公证。默认最低部署版本为 macOS 12。

Linux/macOS 默认把配置放在当前用户应用数据目录下的 `Spark` 文件夹。
可执行文件旁的 `config.json` 优先；也可用 `--config /绝对路径/config.json`
指定其他配置。后台窗口快照使用 X11/XComposite 或 macOS 的屏幕录制权限，
不支持的 Wayland 会话会禁用快照菜单。

实验性 XP 构建入口为 `scripts/build-xp.ps1`，拒绝使用新版 MSVC 工具集。
它需要支持异步 DNS、兼容 XP 的 TLS 后端的 x86 libcurl SDK，以及 PEM
根证书包；脚本不会下载这些依赖，也不会把 Windows 7 程序改名充当 XP 程序。
先用 `scripts/build-xp-tls.ps1` 构建固定版本的 TLS 依赖，
再将安装后的 SDK 和 `cacert.pem` 传给 `scripts/build-xp.ps1`。
用 `scripts/package-portable.ps1 -WindowsXP -Architecture x86 -BuildDirectory ./build-xp-x86/xp-stage`
打包其 `xp-stage` 目录。根证书包必须保留在两个 XP 程序旁边。

## 快速开始

1. 将 `Spark-windows-x64.zip`（64 位 Windows）或
   `Spark-windows-x86.zip`（32 位 Windows）解压到可写目录。
   两者最低要求 Windows 7 SP1，x64 系统也包括 Windows 11。
   这是便携包，即解压运行，不需要安装程序。
2. 启动 `Spark.exe`。如果缺少 `config.json`，程序会在 EXE 旁创建，
   不会替换已有配置。
3. 打开**设置 > 供应商配置**，添加供应商、API 地址、API Key 和模型 ID。
   也可参照 `config.example.json` 中的英文注释手动编辑 `config.json`。
4. 在发送按钮旁选择模型、推理强度和权限模式，然后发送消息。
   供应商页面还提供发送 `hi` 的连接测试。

会话数据默认保存在配置文件旁的 `data` 目录。
无项目任务的产出默认位于当前用户的 `Documents\Spark` 目录。

更新时先退出 Spark 和正在使用其 EXE 的外部 MCP 进程，再将新便携包
解压到原目录。发行包不包含 `config.json`、`data` 或 `workspace`，
这些私人文件会保留。API Key 使用明文或环境变量表达式保存；
供应商编辑页默认遮盖密钥，主动点击显示后才会展开。

## 主要功能

- 在一个配置文件中设置多家供应商、多个模型和默认推理强度，
  每个供应商使用自己的 API 根地址和 API Key。
- 支持 OpenAI Chat Completions、OpenAI Responses 和 Anthropic Messages 流式输出。
- 原生设置页支持供应商添加、编辑、复制、删除、排序、获取模型和连接测试。
- 使用内置 `md4c` 解析器渲染 CommonMark/GFM Markdown，
  支持展开命令详情、记录处理时间和用量统计。
- 项目、最近会话、置顶、归档、删除和会话独立草稿。
- 多会话并行、中断与继续、托盘运行和单实例唤起。
- 文件与图片附件、剪贴板图片、智能窗口快照、持续目标和计划模式。
- 有界本地工具：目录列表、文件读取、字面搜索、原子写入、
  补丁、读图和受控命令执行。
- 按模型窗口和触发百分比自动压缩上下文。
- stdio 与 Streamable HTTP MCP 客户端、Skills 和 `AGENTS.md` 规则。
- 内部子智能体可独立选择模型；其他智能体可通过 `SparkMCP.exe`
  创建和管理 Spark 会话。
- 摘要菜单按**子智能体**和**来源**分组，打开右侧标签页面板。
  子会话复用正常聊天渲染；来源包括图片预览、供应商网页搜索与引用元数据、
  MCP 工具引用。
- 导入与导出 Codex 聊天分支。
- 十种界面语言：简体中文、繁体中文、英文、日文、韩文、德文、法文、
  西班牙文、巴西葡萄牙文和俄文。默认选择**自动检测**，
  跟随操作系统语言，无法匹配翻译时使用英文；手动选择后保持固定。

## 配置

配置版本为 `2`。Spark 接受配置文件中的 `//` 注释，
填写供应商信息后即可直接使用示例。设置页保存时会写为标准 JSON 并移除注释。
高级选项使用默认值时会省略，自定义覆盖值会保留。

一个最小的 Responses 供应商配置如下：

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

`YOUR_MODEL_ID` 是占位符，需替换为供应商实际支持的模型 ID；
`272000` 是窗口示例值，不代表该模型的真实规格。

| 字段 | 含义 |
| --- | --- |
| `active` | 当前默认供应商、模型和推理强度，不会禁用其他已配置模型 |
| `providers.<id>.protocol` | `openai_responses` / `responses`、`openai_chat_completions` / `chat_completions` 或 `anthropic_messages` |
| `providers.<id>.base_url` | API 根地址；适配器按需补齐 `/responses`、`/chat/completions` 或 `/messages`，不会重复添加后缀 |
| `providers.<id>.api_key` | 供应商密钥，或 `${ENV:MY_PROVIDER_KEY}` 这样的环境变量表达式 |
| `providers.<id>.models` | 上游真实模型 ID，以及可选的窗口、压缩和推理覆盖值 |
| `permissions.default_mode` | `ask`、`approve_for_me` 或 `full_access` |
| `workspace.path` / `storage.data_dir` | 无项目产出根目录和会话数据目录 |

相对路径以 `config.json` 所在目录为基准。支持 `${DOCUMENTS}`、
`${APPDATA}`、`${LOCALAPPDATA}`、`${EXE_DIR}` 和 `${CONFIG_DIR}`。
设置页显示解析后的绝对路径。

`workspace.path` 默认为 `${DOCUMENTS}/Spark`，不会自动注册成项目。
无项目会话在首次成功运行一轮后创建独立工作目录：

```text
Documents\Spark\YYYY-MM-DD\chat-YYYYMMDD-HHMMSS-xxxxxxxx\work
Documents\Spark\YYYY-MM-DD\chat-YYYYMMDD-HHMMSS-xxxxxxxx\outputs
```

项目会话直接使用所选项目目录，不会改写无项目默认目录。
旧界面状态在任务真正运行前不会创建目录；
原项目被删除的会话会按无项目会话显示。

完整默认值、高级覆盖、权限行为和旧配置迁移规则见
[配置说明](CONFIGURATION.md)。

## 聊天与附件

- **新对话**打开空白草稿；单击已有会话即可恢复。项目和最近会话分开显示。
  项目控件用于添加工作目录；悬浮到会话上可显示操作按钮。
- 默认 `Enter` 发送、`Shift+Enter` 换行，可在设置中改为 `Ctrl+Enter` 发送。
  输入法组词期间按 Enter 只确认候选，不会发送。
- 回合运行时发送按钮变成暂停按钮。中断后可点击继续；
  此时发送新文字会开始新的一轮。
- `+` 菜单中的**附加文件**打开对应平台的文件选择器；Windows 选择器还可用
  **附加当前文件夹**添加目录引用。
- 图片附件显示在发送框上方，可放大或移除，只随本次发送附加，
  不会在后续每条消息中重复携带。
- 在 Windows 上，切换到其他应用后，同时按左右 Alt，可附加其窗口截图和标题。
  被遮挡的窗口可在不切换前台的情况下截取；
  独立辅助进程的超时为 3.5 秒。最小化窗口和拒绝窗口截图的应用暂不支持。
- 快照发送前只作为本地草稿附件，可预览和移除；
  没有可用目标时快照菜单项会禁用。
- 目标和计划模式随发送、切换会话及重启保留。
  目标是持续模型指令，不会自动开启回合。
  计划模式允许内置读取和搜索，禁止写入、补丁、命令、MCP 调用和创建子智能体，
  完全访问权限也不能绕过。
- 设置包含**常规**、**供应商配置**和**已归档聊天**，
  可恢复或永久删除归档会话。

## 上下文压缩

自动压缩始终开启，默认在所选模型窗口的 90% 触发，并受输出余量约束。
未知模型窗口使用内置后备值 `272000` token，其 90% 阈值为 `244800`。
每个模型可单独覆盖窗口和百分比。

每次模型请求前（包括工具执行后的请求）和回合结束后，都使用同一个压缩器检查。
`context.compacted` 记录压缩前后 token 估算、触发原因、模型和耗时。

模型生成有界交接摘要，包含目标、偏好、已完成工作、修改文件、当前状态、
证据和下一步。过大的上下文会分段摘要并按顺序合并；
模型摘要失败时回退为本地有界交接。
压缩改变发送给模型的上下文，界面聊天记录继续保留，摘要不保证无损记忆。

使用 `/compact` 立即压缩；使用 `/context` 查看估算用量与有效阈值，
不会因此发起模型请求。

## MCP 与 Skills

### MCP 客户端

在 `mcp_servers` 中配置服务：

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

工具名为 `mcp__<server_id>__<tool_name>`。HTTP MCP 需要网络权限，
stdio 会启动子进程。可选请求头、环境变量、工具过滤、必需服务与超时设置见
[配置说明](CONFIGURATION.md)。
MCP 元数据不受信任，服务声明自己只读也不能绕过审批。

### Skills

Spark 从以下位置发现包含 `SKILL.md` 的文件夹：

- `<workspace>/.spark/skills`。
- `${APPDATA}/Spark/skills`。
- 额外的 `skills.roots`，例如 `["./skills"]`。

前两个目录的扫描可分别关闭，默认最多发现 64 个技能。
YAML front matter 支持 `name`、`description`、`short_description`、
`version` 和 `allowed-tools`。

技能目录列出绝对路径，智能体通过 `read_skill` 按需加载正文或相对资源；
消息中的 `$skill-name` 会显式加载该技能到本回合。
技能内容不能覆盖系统指令或权限策略。

## 子智能体

### 在 Spark 内调用

`list_models` 列出已配置的供应商与模型、有效默认推理强度，
不暴露密钥或 URL，分页结果控制在 4 KiB 以下。

`spawn_agent` 接受 `name`、`task`，以及可选的 `provider`、`model`
和 `reasoning_effort`。选择其他供应商时需同时指定供应商和模型。
不指定模型则继承父智能体模型；选择不同模型但未指定强度时，采用该模型的有效默认值。
无效模型或不支持的强度会在启动前拒绝。

子智能体继承工作目录、MCP 连接和权限策略，使用所选供应商的密钥、
地址和协议，详情中显示实际供应商与模型。
内部最多并行运行四个子智能体，不允许继续嵌套创建。
子智能体的选择不修改父智能体模型或 `config.json`，
已完成子智能体的元数据会在重启后保留。

### 从其他智能体调用

`SparkMCP.exe` 是使用同一配置文件的独立 stdio JSON-RPC MCP 服务，
提供以下工具：

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

例如，使用绝对路径在 Codex 中注册：

```toml
[mcp_servers.spark]
command = "E:\\Path\\To\\Spark-windows-x64\\SparkMCP.exe"
args = ["--config", "E:\\Path\\To\\Spark-windows-x64\\config.json"]
startup_timeout_sec = 20
tool_timeout_sec = 600
```

`create_subagent` 接受 `task`，以及可选的 `name`、`provider`、`model`、
`reasoning_effort`、`workspace` 和 `permission_mode`。
默认 `read_only` 模式禁止写入、命令和 MCP 调用；
其他模式为 `ask`、`approve_for_me` 和 `full_access`。
每个子会话拥有独立工作线程、取消令牌、权限模式和
`storage.data_dir\sessions` 下的日志。
单个服务进程最多运行四个子会话、保留 64 条子会话记录。

`workspace` 匹配已注册项目路径时，会话出现在该项目下；否则出现在最近会话中。
指定名称时直接作为标题，否则从任务中提取简短标题。

`wait_subagent` 默认最多等待 120 秒，上限 600 秒。
`output_complete: false` 表示仅返回进度快照，需要继续等待。
`ask` 模式下应检查 `awaiting_approval`、`approval_kind`、`approval_title`
和 `approval_detail`，再调用 `approve_subagent_action`，
传入子会话 `id` 与 `approved: true` 或 `false`。
`approve_for_me` 自动批准其策略允许的操作；
`full_access` 应用明确请求的完全访问模式。

## AGENTS.md 规则

按以下顺序加载：

1. `%APPDATA%\Spark\AGENTS.md`。
2. 从最近的 Git 项目根目录到当前工作目录之间，每一级目录的 `AGENTS.md`。

较具体的文件细化上层规则。GUI 和外部子会话均限制为单文件最多 32 KiB、
总计最多 128 KiB。这些规则不能覆盖权限策略。

Spark 不会自动读取 `CODEX_HOME` 或 `%USERPROFILE%\.codex` 中的
Codex 全局规则；`CODEX_HOME` 仅用于 Codex 会话导入和导出。

## Codex 会话导入与导出

- 右键会话选择**导出为 codex 聊天分支**，生成保留消息、推理、
  工具调用和结果的 rollout JSONL 分支。
  对话框默认指向 `%CODEX_HOME%\sessions`，
  未设置时使用 `%USERPROFILE%\.codex\sessions`。
- 在最近会话菜单选择**导入 codex 聊天分支**，或在项目菜单
  **归档聊天**上方选择同一项，导入到对应范围。
- 导入创建新的 Spark 会话，不修改源 rollout 文件。

## 构建与打包

在 Windows x64 开发电脑上构建，需要：

- Visual Studio 2022 C++ 开发工具及 **MSVC 14.38**。
- CMake 3.20 或更新版本和 Ninja。

源码已包含 nlohmann/json 和 md4c，无需完整 Codex 仓库或额外下载这两个库。
最终用户的 Win7 电脑无需安装构建工具。

在源码根目录运行 PowerShell：

```powershell
.\scripts\build-win7.ps1
.\scripts\package-portable.ps1

# 相同 GUI 和功能，面向 32 位 Windows 7 SP1 或更新系统。
.\scripts\build-win7.ps1 -Architecture x86
.\scripts\package-portable.ps1 -Architecture x86

# 打包回归检查，不调用 API，也不需要私人配置。
.\tests\package_tests.ps1
```

构建脚本使用 `vswhere` 查找 Visual Studio，查找 CMake 和 Ninja，
默认使用一个编译任务，执行干净构建、本地测试及两个 EXE 的已知不兼容导入检查。
工具路径可通过 `-VisualStudioDirectory`、`-CMakePath` 和 `-NinjaPath` 指定；
`-BuildDirectory` 与 `-ParallelJobs` 分别控制输出目录和并发数。
x64 默认构建目录为 `build-win7`，x86 为 `build-win7-x86`。

执行策略阻止脚本时，可只为本次运行指定覆盖：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build-win7.ps1
```

`Spark-windows-x64.zip` / `Spark-windows-x86.zip` 及其 `.sha256` 文件输出到 `dist`。
打包只更新公开文件，并核对原有配置、数据和工作目录哈希没有改变。
更新对应架构的发行目录前，还会校验每个 EXE 的 PE 机器类型。
兼容性探针留在构建目录，不进入便携发行包。
已有的 `Spark-portable-win7-*` 运行目录会原位更新，不移动配置，
也不破坏已注册的 MCP 程序路径。改名后的包成功生成后才会移除旧 ZIP 和校验文件。

导出干净源码目录和压缩包：

```powershell
.\scripts\export-source.ps1
```

生成 `dist\Spark-source`、`dist\Spark-source.zip` 及其 `.sha256`。
使用 `-Force` 可覆盖之前生成的导出副本。
包含源码、测试、资源、脚本、示例和第三方声明；
不包含私人配置、会话、编译产物、日志和内部施工笔记。

### 跨平台基础层

共用基础层包含配置、用户目录、Unicode 文件读写、
会话、Skills、项目规则、上下文压缩计算和现有流式 Markdown 解析器。
可独立于 Windows GUI、网络和进程运行层编译：

```sh
sh scripts/build-platform-tests.sh
```

脚本使用一个编译任务和临时测试数据。Linux 用户目录遵循 `HOME` 和 XDG 配置；
macOS 使用 `Documents` 和 `Library/Application Support`。
这些平台省略 shell 时默认使用 `/bin/sh`，Windows 仍使用 `cmd.exe`；
配置中明确填写的值保持不变。
可移植的 HTTP/供应商层也可单独编译。Linux/macOS 需要支持 TLS 和异步 DNS
的 libcurl 7.68 或更新版本、开发头文件和系统 CA 证书。Debian 安装
`libcurl4-openssl-dev` 和 `ca-certificates` 后运行：

```sh
sh scripts/build-runtime-tests.sh
```

Windows 7 及更新系统保留 WinHTTP；XP 和 Linux/macOS 使用 libcurl，并验证证书及主机名。
本地模拟服务测试覆盖 Chat Completions、Responses、Anthropic Messages、
流式输出、二进制图片附件、取消以及独立的响应头等待和流空闲超时。
响应最多保留 1 MiB 用于诊断，流回调仍接收完整内容。POSIX 后端拒绝重定向，
避免把 API 凭据转发到其他地址。

这两种单独构建只检查对应库。完整 Linux/macOS 应用使用
`scripts/build-native.sh` 构建，包括原生 GUI、智能体运行时、进程执行、
MCP 和桌面集成。

## 源码结构

| 目录 | 内容 |
| --- | --- |
| `src/core` | 智能体循环、协议、配置、会话、工具、压缩、MCP、Skills 和子智能体 |
| `src/gui` | 原生聊天、侧边栏、设置和摘要面板 |
| `src/platform` | 原生路径、用户目录、会话日志和二进制读写、WinHTTP/libcurl 及 Windows 文本和窗口截图辅助 |
| `src/render` | Markdown 解析和渲染辅助 |
| `resources` | 应用图标与 Windows 资源 |
| `resources/locales` | 内嵌翻译 JSON；测试检查覆盖率、占位符和代码引用 |
| `tests` | 本地模拟服务和回归测试 |
| `scripts` | 构建、发行打包与源码导出 |
| `third_party` | 已随源码附带的第三方库及其原始许可证 |

## 兼容性与边界

Windows 7 SP1 x64 和 x86 是兼容目标，两者复用相同 GUI 和智能体代码。
构建使用 PE 子系统 `6.01`、`_WIN32_WINNT=0x0601`、`WINVER=0x0601`
和静态 CRT 链接，保留 TLS 证书及主机名验证。

构建脚本拒绝已知不兼容导入，如 `GetSystemTimePreciseAsFileTime`、
`GetDpiForWindow` 和 UCRT API 集。
独立探针检查启动、配置、验证 HTTPS、流式读取、子进程和取消。
编译与导入检查不能替代 Win7 真机测试；
系统补丁、TLS、供应商和外部 MCP 依赖会影响实际运行。

| 平台 | 当前状态 |
| --- | --- |
| Windows 7 SP1 或更新系统，x64 | 已构建；21 项宿主机测试及导入检查通过 |
| Windows 7 SP1 或更新系统，x86 | 已构建；21 项宿主机测试及导入检查通过，受 32 位地址空间限制 |
| Windows XP SP3 x86 | 两个程序已用 v141_xp 构建；导入检查和 10 项宿主机测试通过，来宾系统运行验证待完成 |
| Linux x64 | 原生 GUI 和完整运行时已在 Debian 构建；11 项测试、GUI 用例及隔离启动检查通过 |
| macOS 12 或更新系统，Intel x64 | 原生构建完成；10 项 CTest、手动 Cocoa GUI 用例、签名归档检查及 Monterey 隔离启动通过 |

`windows-x86` 包要求 Windows 7 或更新系统，XP 使用独立的
`windows-xp-x86` 包和随包提供的 TLS。Linux 已验证构建使用 Debian 13
及其系统库，不能据此声称兼容更旧发行版。ARM 架构需要另行原生构建和验证。

虚拟机隔离检查时，将匹配的 ZIP 解压到来宾系统内可写的本地目录，
启动 `Spark.exe`，配置测试供应商并发送短消息。然后检查设置、滚动、
中断、会话切换及重启后数据保留。一次只运行一台虚拟机。
64 位 Windows 来宾先测试 x64 包；比较 x86 时使用独立目录，
不要共用正在使用的会话数据。编译和宿主机测试不能替代来宾系统运行验证。

文件工具限制条目数、深度、耗时、文件大小和结果数量，
跳过重解析点与已知系统目录。
系统允许时，命令取消和超时通过 Windows Job Object 终止其所属进程树；
Linux/macOS 使用独立的子进程组取消和清理命令。
有界命令输出同时保留头部和尾部，不会静默重放执行状态不确定的命令。

权限由应用实施，不是操作系统沙箱。
尚未实现 MCP OAuth、任意重启策略、跨供应商自动故障转移、
目标自动继续、目标完成工具和目标 token 预算。
Windows 使用便携 ZIP，Linux/macOS 使用 tar 压缩包，均不包含安装程序。

## 第三方声明

内置 nlohmann/json 3.11.3 和 md4c CommonMark/GFM 解析器。
原始许可证分别位于 `third_party/nlohmann/LICENSE.MIT`
和 `third_party/md4c/LICENSE.md`，便携发行包也包含这些文本。
详见[第三方声明](THIRD_PARTY_NOTICES.md)。
