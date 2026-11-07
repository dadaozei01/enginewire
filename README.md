# EngineWire UE 5.4

基于 [ChiR24/Unreal_mcp v0.6.0](https://github.com/ChiR24/Unreal_mcp/releases/tag/v0.6.0) 的 UE 5.4 兼容源码、Windows 编译包与 Agent 接入说明（Windows 发布包仍为 v0.6.0-beta-f，见 [UPSTREAM.md](UPSTREAM.md)）。非上游官方发行，保留 MIT 许可证。此仓库不包含用户工程或测试资产。

## 安装

从 [Releases](https://github.com/dadaozei01/enginewire-ue54/releases) 下载 Windows ZIP 与 SHA256SUMS-Win64.txt。关闭 UE，将 ZIP 内 EngineWire 文件夹放到以下一个位置：

- 工程级：<工程>/Plugins/EngineWire
- 公共插件：<UE_5.4>/Engine/Plugins/Marketplace/EngineWire

正确层级是 EngineWire/EngineWire.uplugin，不要双重嵌套。备份旧版本，避免工程级与引擎级重复安装同名插件。打开 UE，在插件窗口启用 EngineWire，然后在 Project Settings → Plugins → EngineWire 勾选 Enable Native MCP Server，重启编辑器。

状态显示 MCP :3000 (0) 表示服务已启动，0 是连接客户端数量。勾选后仍显示 off 时先确认重启。

## 统一操作规则

已验证的 EngineWire MCP 优先；成熟批量任务可用编辑器内 UE Python；独立 Remote Execution 备用。正常接入不需要任务按钮，也不要求另开 Python 网络连接。

跨 Agent 的共享规范以 [ue-ai-bridge-protocol](https://github.com/dadaozei01/ue-ai-bridge-protocol/blob/main/docs/agent-entry.md) 为准；本仓库负责插件安装、编译包与客户端接入细节。

## 平台验证

| 平台 | 状态 |
|---|---|
| Windows x64 / UE 5.4.4 | 已编译、加载，MCP 与蓝图/材质/Niagara 操作实测通过（2026-10-23 包）；v0.6.0-beta-f-ue54.3 包（2026-10-26）含退出崩溃修复，重新编译后编辑器加载、作用域令牌连接、只读查询实测通过，退出稳定性与写入尚未在该版本上重新验证；2026-10-23 的旧包不含此修复 |
| Mac (Apple Silicon) / UE 5.4.4 | 已用 Xcode 27.0 编译为 Universal（arm64 + x86_64）；2026-10-25 含退出崩溃修复重新编译，编辑器加载、作用域令牌连接、只读查询实测通过，此后多次退出未再崩溃；写入已实测蓝图创建与复制、组件、批量写图表、删节点与函数、生成 Actor、编辑器内 Python、PIE 与截图；2026-10-31 按 v0.6.0 重新编译，蓝图文本读写往返、材质创建与连节点、Niagara 拖尾创建与调参实测通过（见 UPSTREAM.md 第 8–15 项）；同日新增 Chaos 破碎命令（第 16 项），从 StaticMesh 新建 GC、Voronoi 切割、Auto Cluster、读取与物理设置、撤销与保存、放置 Actor 实测通过，PIE 中落地碎开。其他写入首次使用后需检查结果 |
| Windows x64 / UE 5.8.4 | [v0.6.0-ue58.1](https://github.com/dadaozei01/enginewire-ue54/releases/tag/v0.6.0-ue58.1) 预发布。已验证插件加载、MCP 连接、引擎级安装、中文蓝图文本往返与编译，以及 Chaos 破碎和 PIE。完整范围见 [UPSTREAM.md](UPSTREAM.md) 第 17 项。 |
| Mac (Apple Silicon) / UE 5.8.4 | 已用 Xcode 27.0 编译为 Universal，插件代码未改动；[v0.6.0-ue58.2](https://github.com/dadaozei01/enginewire-ue54/releases/tag/v0.6.0-ue58.2) 为预发布。工程加载、MCP 连接、蓝图、材质与实例、Niagara、Python、Chaos 破碎、PIE、截图和 3 次退出实测通过。已知问题和未覆盖范围见 [UPSTREAM.md](UPSTREAM.md) 第 18 项。 |

2026-10-25 起：修复编辑器退出时 Native MCP 关闭竞态导致的偶发崩溃（见 [UPSTREAM.md](UPSTREAM.md) 第 7 条）；请求头 helper 优先使用作用域令牌，配置见 [Claude Code 配置](docs/claude-code.md) 的「作用域令牌」。

2026-10-29 起：请求头 helper 不带工程参数时，自动读取监听 MCP 端口的 UnrealEditor 当前打开工程的令牌，打开哪个工程就连哪个，换工程不用改配置（Windows 已实测，Mac 尚未实测）。写法与限制见 [Claude Code 配置](docs/claude-code.md) 的「添加」。

实际 Windows 构建使用 5.4.4，描述符兼容标记为 5.4.0；定制引擎或其他版本应重新编译。

### UE 5.8 预发布

[v0.6.0-ue58.1](https://github.com/dadaozei01/enginewire-ue54/releases/tag/v0.6.0-ue58.1) 适用于 Windows x64 / UE 5.8.4，Latest 仍为 5.4 版。源码在 [`build/ue58`](https://github.com/dadaozei01/enginewire-ue54/tree/build/ue58)，尚未合并到 `main`。该分支的 `EngineVersion` 为 `5.8.0`，并将 MovieRenderPipeline、MoviePipelineMaskRenderPass 设为必需依赖，不能直接合并到 5.4 主分支。

工程级安装用 `<工程>/Plugins/EngineWire`，引擎级安装用 `<UE_5.8>/Engine/Plugins/Marketplace/EngineWire`。两处任选其一，每个工程需自行启用插件。全新工程启用插件后是否默认开启 Native MCP 尚未实测。若状态为 `off`，在 Project Settings → Plugins → EngineWire 勾选 Enable Native MCP Server 并重启。不要同时打开多个编辑器：Mac 上它们都能监听 3000，连接进入后打开的那个（UPSTREAM.md 第 18 项）；Windows 的行为未在本轮验证。

- [其他 Agent 如何连接并操作](docs/agent-guide.md)（含 Chaos 破碎命令）
- [Codex 配置与认证](docs/codex.md)
- [Claude Code 配置与认证](docs/claude-code.md)
- [Windows / Mac 编译方法](docs/build.md)
- [来源、兼容修改与验证范围](UPSTREAM.md)
