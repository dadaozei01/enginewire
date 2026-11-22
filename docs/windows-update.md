# Windows 更新到最新 UE 5.8 版（给 Windows 上的 Agent）

## 本轮交接：蓝图排版与写入修复（2026-11-22，含 PR #13 / #15 / #16）

用户要求在公司 Windows 电脑更新插件。**本轮走源码构建路径，不直接安装下面的旧 Release ZIP。** 最新修复已全部合入 `main`：

- [PR #13](https://github.com/dadaozei01/enginewire/pull/13)（合并提交 `3e0efd7c8dfc222fdede86b20db61f3515c85754`，含 `9fa676273130494058b0749f37baffd3f4f5c3a7`）：真实 Slate 节点尺寸与引脚位置、执行流对齐、Return/Completed 回线走廊、普通成员变量 Get 就近复制。
- [PR #15](https://github.com/dadaozei01/enginewire/pull/15)（合并提交 `1b2e06fa649242bfc54b5b2248d050614c1c322d`，含 `80afdf645d2e29db697e03ea09926d7696995b3a`）：`write_graph_from_code` 多 Form 未排版节点隔离、按场景区分 `bAllowMovingExisting`、以及 `FTranspiler::Connect` 自动转换节点（如 `Conv_IntToDouble`）追踪。
- [PR #16](https://github.com/dadaozei01/enginewire/pull/16)（合并提交 `d748389d41d50584ff795b95756c3bbf3cb166b0`，含 `bae493aa6dbb5f8d76eff0a3392fbe0bdbf3ba7d`、`d989a14ba37ef2aef06d4d42b257da90102366e9`、`72af6d71b726eb3ed4b3558e4cfa55c345110bc8`）：深层纯 DAG 排版优化（列松弛消除反向纯数据线、2D 装箱、入口左侧边界钳制）以及扩展远距离零副作用读取就近复制（`UK2Node_Self`、同作用域只读局部变量、只读函数输入参数、无额外参数的原生 `const` 纯成员 Getter 连同简单 `Target` 读取节点一起复制）。

现有 v0.8.0 Release ZIP 不包含以上修复，本轮不采用。

给公司 Codex 的执行要求（用户最新要求：只更新、编译、替换，不做功能验证）：

- 按第 0–4 节执行：确认本机 UE 5.8.4、目标工程、编辑器状态和现有安装层级，拉取最新 `main`，完整编译 Win64，备份旧插件后替换。不要沿用 Mac 工程路径或假定本机插件版本。
- 源码应至少包含 `d748389d41d50584ff795b95756c3bbf3cb166b0`（即包含 PR #13、#15、#16 全部提交）；记录实际构建的 `git rev-parse HEAD`。不要切回历史修复分支，也不要使用旧 Release ZIP。
- **本轮跳过第 5 节全部 MCP、Niagara、PIE、自动化及视觉回归，不新建测试资产，不对用户蓝图重新排版。** 保留安装脚本自带的备份、DLL 哈希与目录检查。
- Mac UE 5.8.4 Universal 构建及 7 项无头自动化回归（`Decompile`、`Parse`、`WriteLayout`、`Geometry`、`Layered`、`Localization`、`Ruled`）已全部通过；Windows 本轮仅报告实际编译和安装结果，不宣称功能验证通过。
- MSVC 编译兼容性修复分支用 `fix/win-msvc-layout-20261122-v2`（已存在则检查后使用或加唯一后缀），替代旧示例 `fix/win-msvc-v080`。修复可推送该分支，不自动合并主线。
- 本轮跳过第 6–7 节打包和手册同步，不发布 Release，不额外改客户端配置。最终只报告源码 SHA、编译结果、安装位置与备份路径；编译失败不替换旧插件。

可直接给公司 Codex：

> 请按 https://github.com/dadaozei01/enginewire/blob/main/docs/windows-update.md 的“本轮交接”拉取最新 main（至少包含 d748389），编译 UE 5.8.4 Win64，备份并替换本机现有 EngineWire 插件，先确认工程、引擎和安装层级，需要关闭编辑器时让我保存关闭。不做功能测试、自动化、PIE、截图或蓝图排版，不新建测试资产；保留安装脚本的备份与文件检查。编译失败保留旧插件，完成后报告源码 SHA、编译结果、安装位置和备份路径，不发布 Release。

## 直接安装发布包：v0.8.0-ue58（历史包，本轮不采用）

在 [Release Assets](https://github.com/dadaozei01/enginewire/releases/tag/v0.8.0-ue58#release-assets) 下载 `EngineWire-v0.8.0-ue58-UE5.8.4-Win64.zip` 和 `SHA256SUMS-Win64.txt`。用 PowerShell 的 `Get-FileHash -Algorithm SHA256` 核对 ZIP 后解压，按第 4 节安装（将 `-Package` 指向解压目录）。仅安装不需要重新编译。

已上架不等于已完整实测：此 Win64 ZIP 对应的源码提交及完整实测范围尚未确认。历史验证记录和后续提交上的实测不能直接归到这个 ZIP。建议先在空的 UE 5.8.4 工程确认插件加载和 MCP 连接，再用于正式工程。

## 从源码重新构建或补做验证

本轮需要重点验证三块功能：

- Niagara 模拟控制（UPSTREAM 第 27 项）：`McpNiagaraSimControl.cpp`（新）、`McpNiagaraModuleInfo.cpp`、`McpNiagaraInfoValidate.cpp`、`McpNiagaraScratch.cpp`。
- Niagara 对战搭建与 NDC 修复（第 28 项）：`McpNiagaraBattleSetup.cpp`（新）、`McpNiagaraParticleData.cpp`（GPU 读回经 sim cache，链接 `UNiagaraSimCacheFunctionLibrary`）、`McpNiagaraDataInterfaceConfig.cpp`、`McpPropertyReflectionImport.cpp`。
- 蓝图规则化自动排版（第 29 项）：`Foundation/GraphLayout/McpRuled.cpp`、`McpRuledNodes.cpp`、`McpRuledTests.cpp`（新）和 `Domains/BlueprintGraph` 下几个文件。

如需重新构建或补做验证，按下面的步骤执行。如果链接报 LNK1194，先检查是否从 NiagaraEditor 或其他延迟加载模块导入了静态数据符号（如 `UEdGraphSchema_Niagara::PinCategory*`），按第 26 项的做法改用同值的文件内常量。


## 默认路径

优先级：先保证更新后能正常使用，其次才是速度和额度。任何时候都要让用户手上有一个能用的插件。

省额度只体现在阅读上：只读本页，不读 README、UPSTREAM、agent-guide 等其他文档，不逐项 search/describe 探索能力。

1. 第 0 步全做（只读，很快）。
2. 第 1 步原样执行。
3. 第 2 步直接完整编译（跳过 `-CheckOnly`）。编译失败时自己修到编过为止：
   - 先 `git switch -c fix/win-msvc-v080`。
   - 看脚本打印的“Distinct errors”，每个报错先看报错行附近，不够再扩大范围；一轮把本次报错一起改完再重编（脚本自动用新的输出目录）。
   - 第 3 节表格里的问题直接改；表格外但原因清楚、只涉及 EngineWire 源码的编译问题也可以改，在报告里逐条说明。
   - 不设轮数上限，只要每轮报错在减少就继续。连续两轮报错没有减少，或需要改引擎、VS、SDK 配置，或要改 `#if ENGINE_*` 版本条件，才停下报告。
   - 编过后提交并推送这个分支（`git push -u origin fix/win-msvc-v080`），用这次的编译结果安装。不合并、不推送 `main`。
   - 实在编不过：不安装，旧插件保持原样，用户继续用旧版。
4. 第 4 步安装（先请用户关闭 UE），记下脚本打印的备份路径。
5. 第 5 步 1–12 项全部做完。
6. 第 7 步同步本机手册。
7. 任何一项实测失败而当场修不好：回退到旧版，再报告。回退方法：请用户关闭 UE，运行
   `powershell -ExecutionPolicy Bypass -File scripts\install-enginewire.ps1 -Package "<备份路径>" -Project "<工程>.uproject"`（引擎级安装改用 `-Engine`），打开工程确认 MCP :3000。
8. 需要生成新 ZIP 时才做第 6 步；此处仅在本地打包，不发布。
9. 报告按第 8 步，尽量简洁（若打包，附上 ZIP 路径与 SHA-256）：编译是否通过（修了几轮、分支名、改了什么）、安装或回退结果、12 项实测逐项结果、手册同步情况。

---

按顺序执行。每一步失败都先停下报告，不要自己换路线。只做本机操作：不推送 `main`，不发布 Release。两个脚本于 2026-11-08 在 Mac 上写成，2026-11-09 已在 Windows 跑通编译、安装与打包（`-Zip` 压缩时若报 DLL 被占用，多为杀毒扫描，稍后重试一次即可）；脚本本身出错时按第 3 节同样的规则修复并提交到分支。

## 0. 只读确认

1. 找到 EngineWire 克隆（旧文档示例为 `D:\tools\enginewire-ue54`）。运行 `git status` 和 `git branch -vv`。有未提交改动就停下报告，不 stash、不丢弃。
2. 确认装有 UE 5.8.4：读 `<UE_5.8>\Engine\Build\Build.version`。只有 UE 5.4 就停下：Niagara 新功能只在 5.8 编译。
3. 列出运行中的 UnrealEditor 进程和它们打开的 .uproject，确认要更新哪个工程，插件现在装在工程级还是引擎级。

## 1. 更新源码

仓库已从 `enginewire-ue54` 改名为 `enginewire`，`main` 现在是 UE 5.8（Windows 与 Mac 共用），UE 5.4 在 `ue54` 分支。

```powershell
git remote set-url origin https://github.com/dadaozei01/enginewire.git
git fetch --prune --tags origin
git checkout main
git merge --ff-only origin/main
```

本地 `main` 原来跟踪 5.4 且有本地提交、无法快进时停下报告。本机还要用 5.4 时另建工作树：`git worktree add ..\enginewire-ue54-line ue54`。

## 2. 编译

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build-ue58-win.ps1 -CheckOnly
powershell -ExecutionPolicy Bypass -File scripts\build-ue58-win.ps1
```

`-CheckOnly` 报告引擎、描述符版本和 MSVC / Windows SDK，不编译。找不到引擎时加 `-Engine "<UE_5.8 目录>"`。完整编译输出到 `%USERPROFILE%\Builds\EngineWire-UE5.8.4-Win64-<rev>-<时间>`，日志在 `%USERPROFILE%\Builds\logs`。失败时脚本列出不重复的错误，并统计其中属于 `Domains\NiagaraAuthoring` 的行数。

已验证过的 Windows 环境：UE 5.8.4（BuildId 55116800）、MSVC 14.44.35229、Windows SDK 10.0.22621.0。不修改引擎、VS 或 SDK 配置，不删除其他插件。

## 3. MSVC 报错怎么处理

UPSTREAM 第 19–25 项已在 v0.7.0-ue58 用 MSVC 编译通过（当时的修复已合入 `main`）；第 27–29 项仍应对本次构建逐项验证。下面这些属于 clang 与 MSVC 的差异，可以做最小修改：

| 报错 | 原因 | 改法 |
|---|---|---|
| C4456 / C4457 / C4458 / C4459（UE 视为错误） | 局部变量遮蔽外层、参数或成员同名变量 | 给内层变量改名 |
| C4701 / C4703 | 变量可能未初始化 | 声明时给初值 |
| C4244 / C4267 | 窄化转换（int64→int32、size_t→int32、double→float） | 加显式转换，如 `static_cast<int32>(...)` |
| C2065 / C3861 / C2027 未声明、类型不完整 | clang 经别的头文件间接包含了，MSVC 没有 | 补上对应 `#include` |
| C2084 / C2995 重复定义、LNK2005 | Unity 合并编译时，不同 .cpp 里同名的 static 函数或匿名命名空间符号撞名 | 改成文件内唯一的名字，或放进带文件名的命名空间 |
| C2666 / C2668 重载有歧义 | 如 `FMath::Min(int32, int64)` | 统一参数类型 |
| C2440 / C2664 类型转换 | 字符串字面量、`TCHAR*` 与 `FString`、`FName` 混用 | 用 `TEXT()`、`FString(...)`、`FName(...)` 显式构造 |
| C4100 等其他新警告被当错误 | MSVC 警告级别不同 | 按报错位置最小修改，不全局关警告 |

规则：

- 上表的问题直接改；表格外但原因清楚、只涉及 EngineWire 源码的编译问题也可以改，报告里逐条说明。需要改引擎、VS、SDK 配置或 `#if ENGINE_*` 版本条件时停下报告。
- 修改提交到新分支 `fix/win-msvc-v080`（脚本问题用 `fix/win-scripts`），推送该分支，不合并、不推送 `main`。每改一轮重新运行编译脚本，用新的输出目录。
- 报告里附 diff 和对应报错原文。

## 4. 安装

需要用户先关闭所有 UE 编辑器；脚本不会关闭或启动编辑器。

```powershell
# 工程级（推荐）
powershell -ExecutionPolicy Bypass -File scripts\install-enginewire.ps1 -Package "<第 2 步的输出目录>" -Project "<工程>\<工程>.uproject"
# 引擎级
powershell -ExecutionPolicy Bypass -File scripts\install-enginewire.ps1 -Package "<输出目录>" -Engine "<UE_5.8 目录>"
```

先加 `-WhatIf` 看一遍检查结果。脚本会：确认包是 Win64 版、编辑器已关闭、没有第二处安装；把旧插件备份到 `%USERPROFILE%\Builds\EngineWire-backup-<时间>`；镜像复制新包；核对 DLL 哈希并检查没有双重嵌套。

之后请用户打开工程，确认状态显示 MCP :3000。客户端的 headersHelper 若指向克隆目录里的 `scripts\mcp-headers.cjs`，目录没变就不用改；改了路径要重连 MCP。

## 5. 通过 MCP 实测

按 search → describe → execute。临时资产放在 `/Game/EwTest`，名称以 `_CTest` 结尾，测完删除，不保存关卡。

1. search 能找到 `create_niagara_scratch_module`、`apply_niagara_scratch_module`、`get_niagara_particle_data`、`set_niagara_data_interface`、`remove_niagara_user_parameter`。
2. `create_effect kind=niagara_emitter` 新建 `NE_A_CTest`，回执 `defaultSpriteMaterialsApplied` 为 1；新建 `NS_A_CTest`，`edit_niagara_system edit=add_emitter` 加入该发射器，命名为 `Dots`。
3. `create_niagara_scratch_module`：ParticleUpdate 阶段，`map_get` 读 `Particles.Position`（Position），`custom_hlsl` 输入 InPos、输出 OutPos，代码 `OutPos = InPos + float3(0, 0, 1);`，`map_set` 写回 `Particles.Position`；`apply_niagara_scratch_module` 的 `compileOk` 为 true。
4. `spawn_niagara` 放到 (1000000, 1000000, 100)，`get_niagara_particle_data`（`advanceTicks` 30）读 Position：x / y 为 1e6，z 随年龄上升。
5. `edit_niagara_system edit=set_emitter_properties`，`emitterProperties` 为 `{"localSpace": true}`：回执为 true；再读粒子数据，`localSpaceEmitter` 为 true。
6. `add_user_parameter` 加 Float 参数 `TmpDel`，不引用，`remove_niagara_user_parameter` 删除（带 describe 返回的 consentGrant）成功。
7. 复制 `NS_A_CTest`，`edit_niagara_system edit=enable_gpu_simulation`（`fixedBoundsEnabled` true），`apply` 的 `compileOk` 为 true；`configure_editor setting=window unthrottle`，`configure_viewport setting=realtime enabled=true`，截图能看到粒子。
8. `control_editor play control=play`，在 PIE 中读一次粒子数据，`control=stop`；查日志中有没有新的 Niagara 错误。
9. 把第 7 步的 GPU 副本放进关卡，`get_niagara_particle_data` 能读回粒子（回执带 readbackMs）；`set_niagara_sim_target target=CPU` 后 `compileOk` 为 true，`step_niagara_simulation reset=true frames=1` 成功，再 `frames=1` 一次，两次读回的粒子位置不同。
10. 蓝图自动排版：新建 `/Game/EwTest/BP_Layout_CTest`（Actor），`manage_blueprint` 的 `edit_graph` `batch` 在 BeginPlay 后接 Branch（条件来自一个比较节点），True 接 PrintString、False 接 Delay，节点都不给坐标；编译通过，读图确认执行节点从左到右、False 分支在 True 行下方、节点不重叠。
11. 运行完整自动化前缀 `EngineWire.Foundation.GraphLayout`（`system_control` 的 run_tests 或编辑器 Session Frontend），核对 `Geometry`、`Layered`、`Ruled` 均通过。`Fixture` 未设置 `ENGINEWIRE_LAYOUT_FIXTURE` 时会跳过实际资产验证，不能算真实蓝图回归：如需运行，须在隔离测试工程中使用用户授权测试蓝图的副本，环境变量填写该副本对象路径，绝不指向正式资产。另做以下可视回归：
    - 在本轮临时蓝图中分别检查 EventGraph 和一个带循环、Branch、Return 的函数；明确调用 `arrange_nodes`、`layout=ruled` 后重新编译并读回。节点不重叠，执行线对齐，Return/Completed 回线不穿过主体。
    - 普通成员变量 Get 同时供给相隔四五个节点的消费者时，远处消费者旁应出现就近 Get；相邻的两个数组 Get 可共用一个局部 Get。比较排版前后引脚类型、默认值与数据来源，执行与返回结果应一致；只增加获准的 Get/转接节点。
    - 保存排版前后截图并报告可视结论。测试资产仅使用本轮创建或明确获准的副本，不对用户业务蓝图批量排版。
12. 删除所有 `_CTest` 资产和放置的 Actor。

## 6. 打包（不发布）

编译与实测都通过后：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build-ue58-win.ps1 -Zip -Version v0.8.0-ue58
```

在 `%USERPROFILE%\Builds\release-v0.8.0-ue58\` 生成 `EngineWire-v0.8.0-ue58-UE5.8.4-Win64.zip`（根目录为 EngineWire，不含 PDB 和 Intermediate）与 `SHA256SUMS-Win64.txt`。不要上传 Release。

## 7. 同步本机手册

- 本机规则仓库（Claude Code 为 `%USERPROFILE%\.claude\ue-claude-bridge`）与 `ue-ai-bridge-protocol` 克隆：先 `git status`，干净就 `git pull --ff-only`，有本地改动就停下报告。
- 检查客户端配置（Claude Code 的 `.claude.json`、Codex 的 `config.toml`）里是否还有 `github.com/dadaozei01/enginewire-ue54` 链接，改为新名字；本地目录名可以保留。

## 8. 报告

- 编译结果；EngineWire 自身的新警告或错误（C4996 弃用警告不用列）。
- 是否改过代码：分支名、diff、对应报错原文。
- 第 5 步逐项通过或失败，附关键回执字段。
- 实际源码提交 SHA、引擎版本/BuildId、安装位置、备份位置；若打包，附 ZIP 路径与 SHA256。
- 事件图与函数排版的前后截图、编译与结果一致性；自动化逐项结果，Fixture 是否实际执行或跳过。
- 手册同步了哪些、改了什么。
