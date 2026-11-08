# Windows 更新到最新 UE 5.8 版（给 Windows 上的 Agent）

## 默认路径

优先级：先保证更新后能正常使用，其次才是速度和额度。任何时候都要让用户手上有一个能用的插件。

省额度只体现在阅读上：只读本页，不读 README、UPSTREAM、agent-guide 等其他文档，不逐项 search/describe 探索能力。

1. 第 0 步全做（只读，很快）。
2. 第 1 步原样执行。
3. 第 2 步直接完整编译（跳过 `-CheckOnly`）。编译失败时自己修到编过为止：
   - 先 `git switch -c fix/win-msvc-niagara`。
   - 看脚本打印的“Distinct errors”，每个报错先看报错行附近，不够再扩大范围；一轮把本次报错一起改完再重编（脚本自动用新的输出目录）。
   - 第 3 节表格里的问题直接改；表格外但原因清楚、只涉及 EngineWire 源码的编译问题也可以改，在报告里逐条说明。
   - 不设轮数上限，只要每轮报错在减少就继续。连续两轮报错没有减少，或需要改引擎、VS、SDK 配置，或要改 `#if ENGINE_*` 版本条件，才停下报告。
   - 编过后提交并推送这个分支（`git push -u origin fix/win-msvc-niagara`），用这次的编译结果安装。不合并、不推送 `main`。
   - 实在编不过：不安装，旧插件保持原样，用户继续用旧版。
4. 第 4 步安装（先请用户关闭 UE），记下脚本打印的备份路径。
5. 第 5 步 1–9 项全部做完。
6. 第 7 步同步本机手册。
7. 任何一项实测失败而当场修不好：回退到旧版，再报告。回退方法：请用户关闭 UE，运行
   `powershell -ExecutionPolicy Bypass -File scripts\install-enginewire.ps1 -Package "<备份路径>" -Project "<工程>.uproject"`（引擎级安装改用 `-Engine`），打开工程确认 MCP :3000。
8. 第 6 步打包只在用户要求发版时做。
9. 报告按第 8 步，尽量简洁：编译是否通过（修了几轮、分支名、改了什么）、安装或回退结果、9 项实测逐项结果、手册同步情况。

---

按顺序执行。每一步失败都先停下报告，不要自己换路线。只做本机操作：不推送 `main`，不发布 Release。两个脚本于 2026-11-08 在 Mac 上写成，**尚未在 Windows 上运行过**；脚本本身出错时按第 3 节同样的规则修复并提交到分支。

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

UPSTREAM 第 19–25 项的 Niagara 代码只在 Mac clang 上编译过。下面这些属于 clang 与 MSVC 的差异，可以做最小修改：

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

- 只改上表这类机械问题；逻辑、引擎接口、`#if ENGINE_*` 版本条件的问题停下报告。
- 修改提交到新分支 `fix/win-msvc-niagara`（脚本问题用 `fix/win-scripts`），推送该分支，不合并、不推送 `main`。每改一轮重新运行编译脚本，用新的输出目录。
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
9. 删除所有 `_CTest` 资产和放置的 Actor。

## 6. 打包（不发布）

编译与实测都通过后：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build-ue58-win.ps1 -Zip -Version v0.7.0-ue58
```

在 `%USERPROFILE%\Builds\release-v0.7.0-ue58\` 生成 `EngineWire-v0.7.0-ue58-UE5.8.4-Win64.zip`（根目录为 EngineWire，不含 PDB 和 Intermediate）与 `SHA256SUMS-Win64.txt`。不要上传 Release。

## 7. 同步本机手册

- 本机规则仓库（Claude Code 为 `%USERPROFILE%\.claude\ue-claude-bridge`）与 `ue-ai-bridge-protocol` 克隆：先 `git status`，干净就 `git pull --ff-only`，有本地改动就停下报告。
- 检查客户端配置（Claude Code 的 `.claude.json`、Codex 的 `config.toml`）里是否还有 `github.com/dadaozei01/enginewire-ue54` 链接，改为新名字；本地目录名可以保留。

## 8. 报告

- 编译结果；EngineWire 自身的新警告或错误（C4996 弃用警告不用列）。
- 是否改过代码：分支名、diff、对应报错原文。
- 第 5 步逐项通过或失败，附关键回执字段。
- 安装位置、备份位置、ZIP 路径与 SHA256。
- 手册同步了哪些、改了什么。
