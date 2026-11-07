# UE 5.8 Mac 验证清单

目标：在 Apple Silicon、UE 5.8 上编译并验证 EngineWire。按阶段往下做，前一阶段不过就先停下处理。每项的结果记入「结果」列，完成后汇总到 UPSTREAM.md。

规则：
- 工具返回成功不等于通过。参数读回、编译、保存、运行、视觉效果分别记录。
- 测试资产放在 `/Game/MCP_Test_58/`，不改工程已有资产。
- 只保存本次测试资产。结果不明时先读状态，不重复写入。
- 在 UE 输入框里改数值前切换到英文输入法。中文输入法可能导致卡死。

## 环境

| 项目 | 值 |
|---|---|
| 日期 | |
| 引擎版本 / BuildId | |
| Xcode / clang | |
| 分支 / commit | |
| 测试工程 | `BP_Test 5.8`（复制自 `BP_Test 5.4`） |
| 插件安装位置 | `<工程>/Plugins/EngineWire` |

## 阶段 0：编译

| # | 检查 | 通过标准 | 结果 |
|---|---|---|---|
| 0.1 | `scripts/build-ue58-mac.sh --check-only` | 引擎完整，版本匹配，Xcode 在允许范围内 | |
| 0.2 | `scripts/build-ue58-mac.sh` | BuildPlugin 退出码 0 | |
| 0.3 | 产物架构 | 记录 `EngineWire`、`EngineWireFab`、`EngineWireValidation` 三个 dylib 的架构，至少含 arm64 | |

Xcode 不在允许范围时，由用户决定换 Xcode 还是调高上限。为通过编译做的每处代码改动都记入 UPSTREAM.md。

## 阶段 1：加载与只读

| # | 检查 | 通过标准 | 结果 |
|---|---|---|---|
| 1.1 | 打开测试工程 | C++ 工程模块编译通过，编辑器能进入主界面 | |
| 1.2 | 插件启用 | 没有模块加载错误，MovieRenderPipeline 已被自动启用 | |
| 1.3 | Native MCP | 状态显示 MCP :3000；全新工程若为 off，记录下来，再手动开启并重启 | |
| 1.4 | `mcp-smoke.py --project <测试工程> --engine 5.8` | 全部 PASS | |
| 1.5 | Claude Code 连接 | `~/.claude.json` 指向测试工程，重连后 `search` 可用 | |
| 1.6 | `control_actor` list | 结果与编辑器中的关卡一致 | |

## 阶段 2：写入

| # | 操作 | 读回与验证 | 结果 |
|---|---|---|---|
| 2.1 | 新建蓝图，加组件，批量写图表，编译 | 节点和连线数一致，编译 0 错误 | |
| 2.2 | 中文编辑器下 `read_graph_as_code` → `write_graph_from_code` | 副本读出、写回、再读，文本一致 | |
| 2.3 | 删节点与函数，复制蓝图 | 读回一致，再次编译通过 | |
| 2.4 | 新建材质并连节点 | 读回连接，材质编译通过 | |
| 2.5 | 材质实例参数 | 读回一致（5.8 的材质实例有自己的 usage flag，留意 `LogMaterial`） | |
| 2.6 | 建 Niagara 拖尾并调参 | `SpawnRate` 不为 0，PIE 中可见 | |
| 2.7 | 生成 Actor，改属性 | Outliner 与读回一致 | |
| 2.8 | 编辑器内 Python | 输出正确，无残留状态 | |
| 2.9 | 截图 | 图片内容与当前视口一致 | |
| 2.10 | PIE 启动与停止 | 只停止自己启动的 PIE，编辑器状态正常 | |

## 阶段 3：Chaos 破碎

| # | 操作 | 读回与验证 | 结果 |
|---|---|---|---|
| 3.1 | `/Engine/BasicShapes/Sphere` 新建 GC | 1 块 | |
| 3.2 | Voronoi 切割：40 个 site、种子 7 | 与 Windows 比较块数（Windows 为 38 块），无渲染线程断言 | |
| 3.3 | 再切一次 | 被 `ALREADY_FRACTURED` 拒绝 | |
| 3.4 | Auto Cluster，8 个一组 | 层级读回一致 | |
| 3.5 | 打开内容浏览器缩略图后再切割和撤销 | 无 `NumTransforms == Transforms.Num()` 断言（Windows 修复点） | |
| 3.6 | 物理设置、撤销重做、保存 | 读回恢复正确，重开资产一致 | |
| 3.7 | 放置 Actor，PIE | 落地破碎 | |

## 阶段 4：稳定性与边界

| # | 检查 | 通过标准 | 结果 |
|---|---|---|---|
| 4.1 | MCP 连接中退出编辑器，重复 3 次 | 没有崩溃报告（Windows 也还未专门验证） | |
| 4.2 | 从命令行打开工程，helper 不带工程参数 | 自动连到当前工程 | |
| 4.3 | 从启动器或 Finder 打开工程，helper 不带工程参数 | 记录能否连接；smoke 的 INFO 行会显示命令行是否含 `.uproject` | |
| 4.4 | 禁用 Fracture 和 PlanarCut 启动 | 插件能加载，破碎命令给出明确拒绝（Windows 未实测通过） | |
| 4.5 | 同时运行 UE 5.4 和 5.8 | smoke 脚本发现目标不符后停止，不发送令牌 | |

## 完成后

1. 把结果写入 UPSTREAM.md，并更新 README 平台表和 `docs/build.md` 中「UE 5.8 的 Mac 包未编译」的说明。
2. 确认没有测试资产、截图、令牌或本机日志进入提交。
3. 用户确认后再提交、推送和上传 Mac 包。

## 2026-11-07 第一轮结果

环境：Apple M1 Max，macOS 27.0.1，UE 5.8.4（Changelist 58902052，BuildId 55116800），Xcode 27.0（Apple clang 21），`build/ue58-mac` @ c6505b6，未改插件代码。引擎自带的 `Apple_SDK.json` 上限为 27.9.0，VerifySdk 为 Valid，未改引擎配置。

| 项 | 结果 | 依据 |
|---|---|---|
| 0.1–0.3 编译 | 通过 | BuildPlugin 3 分 45 秒，三个 dylib 均为 x86_64 + arm64；134 条警告全部是 `-Wdeprecated-declarations` |
| 1.1 工程编译 | 通过，需改设置 | 副本的 Target 由 V5 / Unreal5_4 改为 V7 / Unreal5_8，否则与共享编译环境冲突 |
| 1.2 插件加载 | 通过 | 三个模块加载，子系统初始化 |
| 1.3 Native MCP | 通过 | 启动后约 525 秒开放 3000 端口，含首次启动的准备时间 |
| 1.4 smoke | 通过 | 全部 PASS，initialize 返回 `unreal-mcp 0.6.0` |
| 1.5 Claude Code | 通过 | `/mcp` 重连后可调用 |
| 1.6 Actor 查询 | 通过 | summary 共 138 个 Actor，按类统计；未与 Outliner 逐项核对 |
| 2.1 蓝图 | 通过 | 新建；6 节点写入，编译 0 错误 |
| 2.2 中文文本往返 | 通过 | 读出、写回（kept 4，remade 2）、读回文本一致 |
| 2.3 删节点、复制蓝图 | 未测 | |
| 2.4 材质 | 通过 | batch 4 步，编译并保存；读回 2 个参数、2 条连线、默认值 |
| 2.5 材质实例 | 未测 | |
| 2.6 Niagara | 部分通过 | 创建，SpawnRate=1，validate 有效；未在 PIE 中查看 |
| 2.7 生成 Actor | 通过 | 蓝图 Actor 和 GC Actor，PIE 中蓝图 BeginPlay 已运行 |
| 2.8 Python | 通过 | arm64，Python 3.11.8，中文输出正常 |
| 2.9 截图 | 通过，有小问题 | 图片正确；Mac 上也存到 `Saved/Screenshots/WindowsEditor`，路径写死在源码里 |
| 2.10 PIE | 通过 | 启动、停止正常；中文 PrintString 输出到日志 |
| 3.1 新建 GC | 通过 | 1 块，960 三角形 |
| 3.2 Voronoi | 通过 | 40 site、种子 7，得 38 块，与 Windows 相同 |
| 3.3 重复切割 | 通过 | `ALREADY_FRACTURED` |
| 3.4 Auto Cluster | 通过 | 9 簇，层级 [1, 8, 38]，与 Windows 相同 |
| 3.5 缩略图场景 | 部分通过 | 切割和撤销重做未出现 `NumTransforms` 断言；未专门打开内容浏览器缩略图 |
| 3.6 物理、撤销重做、保存 | 部分通过 | 撤销读回 1 块；重做成功，未单独读回；阈值 [1,1,1] 读回一致；未关闭重开资产 |
| 3.7 PIE 落地破碎 | 通过 | 截图显示碎块散落在地面 |
| 4.1 退出稳定性 | 未测 | |
| 4.2–4.5 | 未测 | |

日志：启动自测后无 Error、Fatal、Assertion 或 Ensure。启动时有 13 条 `LogAutomationTest: Error: Condition failed`，紧跟引擎的 `UE::UnifiedErrorTest` 输出，判断来自引擎自测。EngineWire 唯一的警告是副本使用了已弃用的完整权限令牌，因为 `Saved` 未复制，尚无作用域令牌。

## 2026-11-07 第二轮结果

重新打开工程后 42 秒内 MCP 端口已开放，smoke 全部通过，Claude Code 无需手动重连。

| 项 | 结果 | 依据 |
|---|---|---|
| 2.3 复制、删节点、删函数 | 通过 | 副本文本一致；删 1 个 PrintString 后编译 0 错误，读回 8 节点 5 连线；新增后删除函数成功 |
| 2.5 材质实例 | 通过 | 创建并设置两个参数，读回一致；PIE 中球显示实例颜色；日志无材质错误 |
| 2.6 Niagara | 通过 | HangingParticulates 模板放入关卡，PIE 截图可见粒子 |
| 3.6 重开资产 | 通过 | 重启后两个 GC 读回一致：38 块 9 簇 [1, 8, 38]；38 块阈值 [1, 1, 1] |
| 4.1 退出 1 | 通过 | MCP 连接中关闭；`LogExit: Exiting.`，`Log file closed`，无崩溃报告，端口释放 |

日志：第二轮启动自测后无 Error、Fatal、Assertion 或 Ensure。

## 2026-11-07 第三轮结果

| 项 | 结果 | 依据 |
|---|---|---|
| 4.1 退出 2、3 | 通过 | 第 2 次重开后、第 3 次 Finder 打开后关闭，均正常结尾，无崩溃报告 |
| 4.2 命令行打开 | 通过 | `mcp-smoke.py` INFO 显示命令行含 `.uproject`；未在该启动方式下单独运行 helper |
| 4.3 Finder 打开 | 发现并修复 | 命令行不含工程路径，自动识别失败；改为从打开的文件反推后，Python 和 Node 都读到正确令牌 |
| 4.4 禁用 Fracture / PlanarCut | 未达到测试条件 | 在 uproject 中禁用后仍被挂载：ChaosEditor（默认启用）等引擎插件把它们列为必需依赖。dylib 直接链接这两个模块，真正缺失时的行为未验证 |
| 4.5 同时开 5.4 和 5.8 | 发现问题 | 两个都监听 3000（`SetReuseAddr(true)` 在 Mac 上同时设置 `SO_REUSEPORT`）。6 次请求全部进后打开的 5.4：5.4 令牌成功，5.8 令牌 401；5.4 日志仍写启动成功。两个编辑器都未崩溃。helper 和 smoke 已改为发现多个监听者时报错，插件尚未修改 |

用户决定暂不修改插件，4.5 作为已知问题记录在 UPSTREAM.md 第 18 项。
