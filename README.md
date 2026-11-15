# EngineWire（UE 5.8）

基于 [ChiR24/Unreal_mcp v0.6.0](https://github.com/ChiR24/Unreal_mcp/releases/tag/v0.6.0) 的 Unreal Engine MCP 插件：UE 5.8 源码、Windows / Mac 编译包与 Agent 接入说明。非上游官方发行，保留 MIT 许可证。此仓库不包含用户工程或测试资产。

`main` 面向 UE 5.8，Windows 与 Mac 共用同一份源码，平台差异只在编译脚本和文档里。UE 5.4 版在 [`ue54`](https://github.com/dadaozei01/enginewire/tree/ue54) 分支，最后一版为标签 `v0.6.0-ue54-final`，今后只做必要修复。仓库原名 `enginewire-ue54`，旧链接会自动跳转。

## 安装

从 [Releases](https://github.com/dadaozei01/enginewire/releases) 下载对应平台的 ZIP 与 SHA256SUMS。关闭 UE，把 ZIP 内的 EngineWire 文件夹放到以下一个位置：

- 工程级：<工程>/Plugins/EngineWire
- 引擎级：<UE_5.8>/Engine/Plugins/Marketplace/EngineWire

正确层级是 EngineWire/EngineWire.uplugin，不要双重嵌套，也不要两处同时安装。Mac 上如果系统拦截下载的 dylib，运行 `xattr -dr com.apple.quarantine <EngineWire 文件夹>`。打开工程，启用 EngineWire，在 Project Settings → Plugins → EngineWire 勾选 Enable Native MCP Server，然后重启。状态显示 MCP :3000 表示服务已启动。

从 5.4 复制的 C++ 工程要把 Target 文件改为 `BuildSettingsVersion.V7` 和 `EngineIncludeOrderVersion.Unreal5_8`。不要同时打开多个编辑器：它们都能监听 3000，连接会进入后打开的那个（UPSTREAM.md 第 18 项）。

## 统一操作规则

已验证的 EngineWire MCP 优先；成熟批量任务可用编辑器内 UE Python；独立 Remote Execution 备用。正常接入不需要任务按钮，也不要求另开 Python 网络连接。

跨 Agent 的共享规范以 [ue-ai-bridge-protocol](https://github.com/dadaozei01/ue-ai-bridge-protocol/blob/main/docs/agent-entry.md) 为准；本仓库负责插件安装、编译包与客户端接入细节。

## 平台验证（UE 5.8.4）

| 平台 | 最新包 | 状态 |
|---|---|---|
| Mac (Apple Silicon) | v0.8.0-ue58（发布中，此前为 [v0.6.0-ue58.3](https://github.com/dadaozei01/enginewire/releases/tag/v0.6.0-ue58.3)） | Xcode 27.0 编译的 Universal 包。工程加载、MCP、蓝图、材质、Niagara、Python、Chaos 破碎、PIE、截图实测通过（第 18 项）；Niagara 系统级 Scratch 模块（Custom HLSL、数据接口输入与实例赋值）、粒子数据读取、删除未引用用户参数、发射器 localSpace/determinism（第 19–25 项）；CPU/GPU 切换、逐帧锁定与截图、仿真阶段、GPU 粒子读回、发射器互读、NeighborGrid3D 一键搭建、Data Channel 创建与 CPU 发射器读取（第 26–27 项）；蓝图规则化自动排版（第 28 项）。 |
| Windows x64 | [v0.7.0-ue58](https://github.com/dadaozei01/enginewire/releases/tag/v0.7.0-ue58) 预发布 | 插件加载、MCP、引擎级安装、中文蓝图文本往返、Chaos 破碎、PIE 实测通过（第 17 项）；第 19–25 项 Niagara 功能已在 Windows 编译并实测。第 26–28 项（Niagara 模拟控制与对战搭建、蓝图自动排版）**尚未在 Windows 编译**，v0.8.0-ue58 的 Win64 包待补（步骤见 [windows-update.md](docs/windows-update.md)）。 |

今后同一版本号同时提供 Win64 与 Mac 两个 ZIP。

- [其他 Agent 如何连接并操作](docs/agent-guide.md)
- [Codex 配置与认证](docs/codex.md)
- [Claude Code 配置与认证](docs/claude-code.md)
- [Windows / Mac 编译方法](docs/build.md)
- [Windows 更新到最新版（给 Agent 的分步手册）](docs/windows-update.md)
- [来源、兼容修改与验证范围](UPSTREAM.md)
- [下次大更新待办](docs/next-update-backlog.md)
