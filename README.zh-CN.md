# Spark

[English](README.md)

Spark 是使用 C++17 编写的轻量桌面 AI 编程智能体，可连接多个 API 供应商并在本地执行工具，
运行时不需要浏览器引擎、Node.js、Python 或本地 Web 服务。自行配置的 MCP 服务可能需要额外运行环境。

> 项目处于早期开发阶段，Linux、macOS 和 Windows XP 移植版仍为实验性版本。
> 预编译包见 [Releases](https://github.com/EdwardKenwayl/Spark/releases)。

## 平台

| 平台 | 发行包 / 界面 |
| --- | --- |
| Windows x64 / x86 | `Spark-windows-x64.zip` / `Spark-windows-x86.zip`；Win32/GDI |
| Windows XP SP3 x86 | `Spark-windows-xp-x86.zip`；独立实验版本，内置 TLS |
| Linux x64 | `Spark-linux-x64.tar.gz`；GTK/wxWidgets 3.2 |
| macOS x64 | `Spark-macos-x64.tar.gz`；Cocoa/wxWidgets 3.2，要求 macOS 12 或更新版本 |

各平台共用智能体运行时和会话格式，原生控件的外观可能有所不同。
Linux 包依赖系统库，应在计划支持的最旧发行版上构建；其他架构需要单独构建和测试。
原生 GUI 仍有待修复的问题，构建成功不等于已验证系统兼容性。

## 快速开始

1. 解压到可写目录，运行 `Spark.exe`（Windows）、`Spark`（Linux）或 `Spark.app`（macOS）。
2. 打开**设置 > 供应商配置**，填写 API 地址、密钥和模型 ID，通过 `hi` 测试检查连接。
3. 在发送按钮旁选择模型、推理强度和权限模式，打开项目或开始对话。

Windows 默认使用程序旁的 `config.json`。Linux 和 macOS 默认使用用户应用数据目录下的 `Spark`，
程序旁的配置优先；也可用 `--config /path/to/config.json` 指定配置。
会话数据默认存放在配置旁的 `data` 目录。

更新时先退出 Spark 及使用其程序的 MCP 进程，再将新包解压到原目录。
发行包不包含 `config.json`、`data` 和 `workspace`。分享安装目录前应备份并移除这些本地文件。
API 密钥可填写明文或环境变量引用。

## 功能

- 普通会话在后台完成时显示最后回复预览并播放提示音，点击通知进入对应会话；MCP 会话和子智能体保持静默。

- OpenAI Responses、Chat Completions 和 Anthropic Messages，支持流式输出、多供应商和模型独立设置。
- 并行会话、项目、草稿、中断与继续、归档，以及 Codex 聊天分支导入与导出。
- 紧凑显示思考摘要和命令进度，优先加载最近对话，按需补载更早记录。
- Markdown、可展开的工具过程、耗时和用量统计、文件与图片附件、剪贴板图片及窗口快照。
- 本地文件与命令工具、网页搜索、自动上下文压缩、持续目标和计划模式。
- MCP 客户端、Skills 和项目 `AGENTS.md` 规则；子智能体可使用不同的已配置模型。
  其他智能体可通过 `SparkMCP.exe` 管理 Spark 会话。
- 十种界面语言，支持跟随系统语言，无法匹配时使用英语。

图片只附加到本次发送的消息。窗口快照可在发送前预览和移除，后台截图取决于系统支持及授权，
暂不支持 Wayland 截图。
支持的图片会缩放、压缩后发送，原文件不变；上下文压缩后仅保留最近四张图片。

上下文默认在模型窗口的 90% 处压缩，未知窗口使用 272000 token。
`/compact` 立即压缩，`/context` 查看用量；压缩保留界面聊天记录，仅摘要模型上下文。
导入 Codex 分支后，续聊使用最近的压缩检查点及后续消息。
需要应用新版导入修复时，请从原始 rollout 重新导入。

权限由应用实施，不是操作系统沙箱。计划模式允许内置读取与搜索，阻止文件修改和外部工具执行。
请审查工具请求及第三方 MCP 服务。

## 配置

使用供应商编辑页或 [config.example.json](config.example.json)。
配置接受 `//` 注释，设置页保存时写为标准 JSON。
默认值、高级选项、权限、MCP 服务和存储路径见 [CONFIGURATION.md](CONFIGURATION.md)。

Skills 从 `<workspace>/.spark/skills`、用户应用数据目录下的 `Spark/skills`
以及 `skills.roots` 配置的目录中发现。
项目规则先读取用户应用数据目录下的 `Spark/AGENTS.md`，再从 Git 根目录逐级读取到工作目录。
Spark 不读取 Codex 的全局规则。

向其他 MCP 客户端开放 Spark 会话时，将 `SparkMCP.exe` 注册为 stdio 服务，
并传入 `--config /absolute/path/config.json`。可用工具包括 `list_models`、`create_subagent`、
`send_subagent_message`、`wait_subagent`、`get_subagent`、`list_subagents`、
`approve_subagent_action` 和 `cancel_subagent`。
服务最多并行运行四个会话；等待结果中 `output_complete: false` 表示进度，尚未完成。

## 构建

### Windows

需要 Visual Studio C++ 工具（MSVC 14.38）、CMake 3.20+ 和 Ninja，在源码根目录运行：

```powershell
./scripts/build-win7.ps1 -Architecture x64
./scripts/package-portable.ps1 -Architecture x64
# 32 位构建使用 -Architecture x86。
./tests/package_tests.ps1
./tests/source_export_tests.ps1
```

沿用现有构建脚本名称。构建会运行测试并检查程序导入项；打包保留已有配置和数据。

XP 需要独立的 v141_xp 工具集和兼容的 TLS 依赖：

```powershell
./scripts/build-xp-tls.ps1
# 将生成的 curl SDK 和 CA 证书路径传入 build-xp.ps1。
Get-Help ./scripts/build-xp.ps1 -Detailed
```

`cacert.pem` 必须放在两个 XP 程序旁。请在目标系统上测试发行包，宿主机测试不能替代 XP 运行验证。

### Linux 和 macOS

Debian/Ubuntu 需要安装 `build-essential`、`cmake`、`libcurl4-openssl-dev`、
`libwxgtk3.2-dev`、`libxcomposite-dev` 和 `xvfb`。
macOS 需要 Apple Command Line Tools、CMake 和 wxWidgets 3.2，
依赖脚本可从本地源码构建 wxWidgets：

```sh
# 仅 macOS；完成后将 /path/to/wx-sdk/bin 加入 PATH。
sh scripts/build-macos-deps.sh /path/to/wxWidgets-3.2.8 /path/to/wx-sdk

# Linux 或 macOS：
sh scripts/build-native.sh
sh scripts/package-native.sh
```

macOS 打包使用临时签名，不包含 Apple 公证。
`build-platform-tests.sh` 和 `build-runtime-tests.sh` 可单独测试共用库，无需构建 GUI。

### 源码导出

```powershell
./scripts/export-source.ps1
# 使用 -Force 更新已有的干净导出。
```

按白名单生成 `dist/Spark-source` 和 `dist/Spark-source.zip`，排除本地配置、会话、
构建产物和内部操作文件。导出器拒绝覆盖尚未提交的已跟踪修改。

## 源码结构

| 目录 | 内容 |
| --- | --- |
| `src/core` | 智能体循环、协议、配置、会话、工具和 MCP |
| `src/gui` | Windows 与原生界面、共用聊天记录逻辑 |
| `src/platform` | 文件、网络、进程和桌面集成 |
| `src/render` | Markdown 解析与渲染辅助 |
| `resources` | 图标与本地化 |
| `tests`、`scripts` | 回归测试、构建和打包工具 |
| `third_party` | 内置依赖及其许可证 |

## 第三方声明

Spark 内置 nlohmann/json、md4c 和 ghc::filesystem，各平台还会按需使用 libcurl、Mbed TLS 或 wxWidgets。
重新分发时请保留必要的声明与许可证，详见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
