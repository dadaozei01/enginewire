# 来源、改动与验证

上游：https://github.com/ChiR24/Unreal_mcp
固定版本：v0.6.0（2026-10-27 正式版；此前为 v0.6.0-beta-f），MIT 许可证。非官方构建。

升级到 v0.6.0 时逐条核对下列修改：第 1、6 项上游已自带同等 include，第 2 项上游改用 5.3 起可用的 Builder.FindNodeOutputs 重写了别名重试，三者不再另行修改；第 3、4、5、7 项上游仍需要，已重新应用。

UE 5.4 修改：

1. McpSystemMaterialUsage.cpp、McpAssetMeshMatSlots.h 显式包含 Engine/SkinnedAssetCommon.h。
2. McpAudioMetaSoundNodesConnect.cpp 新前端输出别名重试受 MCP_HAS_METASOUND_FRONTEND_V2 条件保护。
3. McpPCGGraph.cpp 两处 GetNodeTitle(EPCGNodeTitleType) 版本判断从 5.5 起改为 5.4 起。
4. 按上游 UE 5.2+ 打包策略使用必需 PCG 依赖；Installed=true，EngineVersion=5.4.0，移除 EnabledByDefault。

Mac 编译修改（新版 Apple clang 下才暴露，不影响 Windows）：

5. McpAudioAuthoringPrivate.h、McpAudioMetaSoundDefaults.cpp 在引入引擎 MetaSound 头文件处用 clang pragma 临时忽略 -Wextra-qualification 和 -Wmissing-template-arg-list-after-template-kw；UE 5.4 的这些头文件在 Xcode 27 的 clang 下报错，不修改引擎文件。
6. McpBPHudFlow.cpp、McpBPAddRespawn.cpp 补充 McpBPAssetLoad.h 引用；它们调用 LoadBlueprintAsset，靠 Unity 合并编译间接可见，分组变化时报未声明。McpBPAddRespawn.cpp 在 2026-10-25 加入第 7 项修复后的 Mac 编译中暴露。

稳定性修复（各平台通用）：

7. FMcpNativeTransport 关闭时的竞态：Shutdown 先 Stop() 关闭监听 socket，接受线程醒来后销毁该 socket；随后 Thread->Kill(true) 再次调用 Stop()，可能对已释放的 FSocket 调 Close()，编辑器退出时 SIGABRT（Mac 实际崩溃栈：UEngineWireSubsystem::Deinitialize → FMcpNativeTransport::Shutdown → FRunnableThreadPThread::Kill → abort）。现以 ListenSocketMutex 保护 Stop() 的关闭与接受线程的销毁，Shutdown 改用 WaitForCompletion()。2026-10-25 Mac 重新编译安装后已实测编辑器加载与只读 MCP；该崩溃为偶发，退出稳定性仍在观察。Windows 发布包自 v0.6.0-beta-f-ue54.3（2026-10-26）起包含此修复。

2026-10-23 Windows 构建环境：UE 5.4.4，MSVC 14.38.33145，SDK 10.0.18362.0。引擎与插件 BuildId 33043543。RunUAT 因本机无关 LightForge 插件规则扫描异常未完整通过；使用其生成的隔离 HostProject 直接 UBT 编译，参数 -DisablePlugin=LightForge，仅排除该插件而不修改其文件。UBT 退出 0，EngineWire、EngineWireFab DLL 链接成功，再整理发布包。

2026-10-26 Windows 从 tag v0.6.0-beta-f-ue54.3（c296a26，含第 6、7 项与作用域令牌 helper）重新编译：UE 5.4.4，MSVC 14.38.33145，SDK 10.0.18362.0，BuildId 33043543。RunUAT 仍因同一规则扫描异常退出 6，照上法用 HostProject 直接 UBT 并 -DisablePlugin=LightForge，UBT 退出 0，两个 DLL 均已生成。安装到 Engine/Plugins/Marketplace 后已实测：编辑器加载的是新 DLL，配置作用域令牌后日志不再出现 deprecated all-or-nothing capability token 警告，Native MCP 握手与 control_actor.list summary 只读查询正常。Windows 上的退出稳定性尚未专门观察，写入操作未重新验证。

已实测编辑器加载、Native MCP 与 Codex 连接、随机巡逻蓝图、蓝色门材质、蓝白 Niagara 拖尾。验证仅代表这些操作，不能代表上游所有能力。

2026-10-23 Mac 构建环境：Apple Silicon，UE 5.4.4，Xcode 27.0（本机 Apple_SDK.json 的 MaxVersion 调到 27.9.0）。RunUAT BuildPlugin -TargetPlatforms=Mac 成功，EngineWire、EngineWireFab 为 Universal dylib，BuildId 33043543。安装到 Engine/Plugins/Marketplace 后已实测编辑器加载、Native MCP 启动于 127.0.0.1:3000、握手与 control_actor.list 只读查询；Mac 上的写入操作尚未验证。

之后在 Mac 上已实测写入：蓝图创建与复制、组件、批量写图表、删节点与函数、生成 Actor、编辑器内 Python、PIE 与截图。其他写入能力首次使用后仍需检查结果。

2026-10-31 Mac 从 v0.6.0 加上述补丁编译：Apple Silicon，UE 5.4.4，Xcode 27.0，RunUAT BuildPlugin -TargetPlatforms=Mac 成功，EngineWire、EngineWireFab、EngineWireValidation 均为 Universal dylib。安装到 Engine/Plugins/Marketplace 后已实测：编辑器加载、Native MCP 启动于 127.0.0.1:3000（serverInfo 版本 0.6.0）、原作用域令牌 helper 无需改动即可连接、search 找到 read_graph_as_code 与 write_graph_from_code、read_graph_as_code 读出蓝图事件图。

蓝图文本读写修复（graph as code，上游 v0.6.0 与 dev 截至 2026-10-31 均未修复，各平台通用，代码在 Domains/BlueprintGraph/Code/）：

8. 中文界面下常用分类（变量、工具|流程控制、类、游戏等）读出为中文、写入按英文匹配：CategoryText 经 FText::FindText("EditorCategoryUtils", 键) 换回引擎英文原文，读写两端统一用英文 id（McpGraphCodeCatalog.cpp）。
9. Math Expression 的 id 里带括号时解析中断：符号可跨过成对的 (...)（McpGraphCodeParse.cpp，附测试）。
10. 同名成员混淆（Manager 的 GetSlot 被当成控件的 GetSlot）：按函数或变量所属类匹配（MemberKey）。
11. 菜单不提供的运算函数（vector * float 等）直接按函数建为普通 CallFunction 节点；原先经函数 spawner 建成可提升运算节点，接上 A 后按 vector * vector 选函数，B 改为浮点也不改回，读回 id 变化且可能断线（McpGraphCodeWriteNodes.cpp）。
12. 写表达式时每接一个参数重新取引脚：Spawn Actor 的 Class 引脚会重建引脚，旧指针触发 Assertion failed: OwningNode 使编辑器崩溃（McpGraphCodeWriteExpr.cpp、McpGraphCodeWriteOps.cpp）。
13. 一个纯函数输出被读两次（含同一语句内）时只在开头 bind 一次；排序穿过内联打印的纯函数节点查找依赖，先绑定被依赖的值，避免后面的文本把它整段重写、写回时多出节点（McpGraphCodeDecompile.cpp，附测试）。
14. 多输出节点的非首个输出（GetSlot 的 Vel）按位置绑定为 (bind (_loc _vel) ...)；原先内联打印会被写回为第一个输出。
15. (for i (range (+ e 1))) 写回时 e 直接接 LastIndex，不再生成 +1 再 -1 两个节点（McpGraphCodeWriteFlow.cpp）。

2026-10-31 Mac 用含第 8–15 项的构建（rt7）验证：SoldierBurst 两个蓝图（54 节点/59 连线、110/131，含变量读写、多输出函数、ForLoop、Math Expression、vector*float）每段读出再写回，再读文本完全一致，节点与连线数不变，无孤立节点，编译通过。限制：含 SpawnActor<蓝图类> 这类菜单里没有的节点的段，只有保留 `; id` 注释时才能写回（复用原节点）。同日在 Mac 上还实测了材质创建与批量连节点、Niagara 拖尾创建、改参数、换渲染材质、加模块并在 PIE 中显示；Niagara 自定义模块只能经编辑器内 Python 建空模块资产再挂上，模块内部逻辑目前无法经 MCP 编写。Windows 发布包仍为 v0.6.0-beta-f，尚未按 v0.6.0 重新编译。

本仓库新增命令（各平台通用，仅 UE 5.4 构建）：

16. Chaos Geometry Collection 破碎：animation_physics 下的 create_geometry_collection、fracture_geometry_collection、cluster_geometry_collection、get_geometry_collection_info、configure_geometry_collection（代码在 Domains/Animation/Physics/McpAnimGeometryCollection*.cpp）。按 Fracture Mode 的 New、Uniform/Custom Voronoi、Auto Cluster 实现：建 GC 用 FGeometryCollectionEngineConversion::AppendStaticMesh，并按反射读取 Fracture Mode 项目设置里的凸包与邻近默认值；切割照编辑器工具直接调 PlanarCut 的 CutMultipleWithPlanarCells（UE 5.4 的 FFractureEngineFracturing::VoronoiFracture 忽略 Chance To Fracture 与 Group Fracture，故不用）；分组调 FFractureEngineClustering::AutoCluster。思路参考过 CarlxsMG/UEAgent 的 Chaos 命令，未复制其代码。Build.cs 仅在 UE 5.4 且找到 FractureEngine、PlanarCut 时加 GeometryCollectionEngine、Chaos、Voronoi 与延迟加载的 FractureEngine、PlanarCut，并定义 MCP_HAS_GC_FRACTURE；.uplugin 增加可选插件依赖 Fracture、PlanarCut。没有用到 DataflowCore。
   能力记录不在上游生成的 MCP/Generated/ 里（那里标明不可编辑，生成脚本也不在本仓库），而写在 MCP/Extensions/*.capabilities.json，由 scripts/gen-extension-capabilities.py 生成同格式的 McpExtensionCapabilityShards.*；能力库在生成分片之后加载它们，目录修订号（catalogRevision）由两者共同计算。改 JSON 或升级上游生成分片后需重新运行该脚本。
   2026-10-31 Mac（gc1 构建）实测：从 SM_PeelShell 新建 GC（1 块，包围盒与手动建的一致）；40 个 site、种子 7 切成 54 块；再切被 ALREADY_FRACTURED 拒绝，4000 个 site 被 TOO_MANY_PIECES 拒绝，均未改动资产；Auto Cluster 8 组，层级 [1, 8, 54]；configure 写入伤害阈值、Convex Overlap Removal 等 4 项后读回一致，撤销恢复、重做再生效；spawnActor 放置 Actor（不模拟）、用 actorName 读取、撤销后移除。实测发现新建 GC 首次保存时引擎报 ensure「CachedDensityFromPhysicsMaterialInGCm3 is not mapped when saving」（保存过程中才缓存密度，两遍序列化不一致），资产仍正常保存；已改为保存前先调用 CacheMaterialDensity()；修复后的 gc2 构建实测新建与切割后保存均无该 ensure，同种子再切同样得到 54 块。随后测试 spawnActor 且 simulatePhysics 为 true 时编辑器崩溃（SIGSEGV，UGeometryCollectionComponent::SetSimulatePhysics → RegisterAndInitializePhysicsProxy → FGeometryCollectionPhysicsProxy::InitializeDynamicCollection）：在编辑器世界调用 SetSimulatePhysics(true) 会立即注册物理代理。已改为只设置 BodyInstance.bSimulatePhysics（与细节面板勾选相同，PIE 开始时生效）。PIE 实测（gc2，不传 simulatePhysics，组件默认即模拟）：从 250 cm 落地，切成 54 块并分 8 组的外壳碎成贴合曲面的薄片；但首次未碎，原因是组件放置时已拷贝资产当时的伤害阈值（500000/50000/5000），之后改资产阈值不影响已放置的 Actor。configure_geometry_collection 因此改为同步 actorName 指定的 Actor 与 applyToActors 点名的 Actor（ApplyAssetDefaults，同一撤销步骤），其余列在 placedActorsNotUpdated；gc4 构建实测：spawnActor 带 simulatePhysics true 不再崩溃；经 actorName 切割、分组、configure 阈值后组件 DamageThreshold 读回 [10, 10, 10]（updatedActors 列出该 Actor），PIE 中不再手动改组件即落地碎开；本次编辑器日志无 ensure 与崩溃。未在 PIE 中验证破碎效果；Windows 未编译。
   2026-11-02 Windows 首次用 MSVC 编译 main（a5b39a3，含第 8–16 项）：UE 5.4.4，BuildId 33043543。RunUAT 仍因 LightForge 规则扫描退出 6，照上文用 HostProject 直接 UBT 并 -DisablePlugin=LightForge。第 16 项代码在 MSVC 下编译无错误；链接报 LNK1194：v0.6.0 的 McpAIStateTreeFeature.h 在 UE 5.4 分支直接引用 StateTreeModule 导出的数据 UE::StateTree::Delegates::OnRequestCompile，而该模块按延迟加载链接，MSVC 不能延迟加载导入了数据的 DLL（Mac 不受影响）。未改为普通链接（那样未启用 StateTree 的工程加载不了插件），改为 Windows 上经 FModuleManager::QueryModule 取已加载的 StateTreeModule 路径，再按导出名 GetDllExport 取委托地址；其他平台仍直接引用。修改后 UBT 退出 0，三个 DLL 均已生成。
   同日装到 Engine/Plugins/Marketplace 后打开工程，弹出「模块 SmartObjectsModule 无法被加载」：v0.6.0 的 .uplugin 新增了对 PropertyBindingUtils 的可选引用，UE 5.4 的 FPluginManager 先把它记入 SeenPlugins，再以「plugin was not built by target」跳过，之后 SmartObjects 对它的必需引用不再入队，PropertyBindingUtils 未启用，SmartObjectsModule.dll 缺少导入。UE 5.4 上 SmartObjects 本身会带上 PropertyBindingUtils，故从 .uplugin 删去这条可选引用；改后 SmartObjects、PropertyBindingUtils、Fracture、PlanarCut 均正常挂载，Native MCP 在 127.0.0.1:3000 启动。Mac 包含同一 .uplugin，可能有同样问题。
   Windows 实测：search「geometry collection」列出 5 个破碎命令。从 /Engine/BasicShapes/Sphere 新建 GC（1 块，960 三角形）；40 个 site、种子 7 切成 38 块；再切被 ALREADY_FRACTURED 拒绝；Auto Cluster 8 个一组得 9 个簇，层级 [1, 8, 38]，get_geometry_collection_info 读回一致；configure 阈值改为 [100, 50, 10] 后撤销，读回恢复为 [500000, 50000, 5000]。蓝图：BP_数值 的副本读出、原样写回、再读，文本一致，11 节点 10 连线不变，编译 0 错误。BP_转刀 的副本先是写回被拒（CODE_INVALID）：变量 Get/Set 节点以原名作标题（Get BPC_切割组件），菜单项按编辑器「显示友好名称」用友好名（Get BPC 切割组件），名字含下划线时读出找不到菜单项，打印为「|GetBPC_切割组件」，写回按 id 建不出。现读出时变量节点的 Get/Set 标题按菜单的写法生成（McpGraphCodeSnapshot.cpp）。修后又见写回时运算节点（- /）带 ; id 却被重建：(- a b) 请求的是 Utilities|Operators|Subtract，原节点是 Math|Float|float-float，标题比较不同；现按运算符号匹配复用（McpGraphCodeWriteForm.cpp）。两处修后，从原蓝图新复制的 BP_转刀 读出（108 节点 125 连线）原样写回，返回文本与写入完全一致，编译 0 错误 0 警告；与原图相比只少了两个转接点（Reroute），文本语法不表示转接点，写回时改为直连，只影响排版。
   已知限制（上游即有，本次未修）：转接点不保留；BPC_切割组件 读出时把非纯的 SphereTraceForObjects 内联进 BreakHitResult，Branch 的 else 接 DoOnce.Reset 在文本中未体现，写回时 MakeArray("ObjectTypeQuery6") 连不上 ObjectTypes（CODE_INVALID，未写入）。未做 PIE 落地测试。

17. UE 5.8 适配与验证（2026-11-06）：Windows x64，UE 5.8.4，BuildId 55116800，MSVC 14.44.35229，Windows SDK 10.0.22621.0。[v0.6.0-ue58.1](https://github.com/dadaozei01/enginewire-ue54/releases/tag/v0.6.0-ue58.1) 为预发布，Latest 仍为 5.4。源码在 `build/ue58`，未合并到 `main`。该分支将 `EngineVersion` 设为 5.8.0，将 MovieRenderPipeline 和 MoviePipelineMaskRenderPass 设为必需依赖，并启用 UE 5.8 的 Chaos 破碎命令。分支描述符不能直接合并到 5.4 主分支。

   已实测：插件加载、MCP 连接、引擎目录安装加载；中文编辑器下的蓝图文本读写和编译；打开资产；通过 `call_editor_tool` 调用官方工具集的一个只读工具；Geometry Collection 创建、Voronoi 切割、Auto Cluster、物理设置、切割和分组的撤销重做、保存、放置 Actor，以及 PIE 落地破碎。

   切割或分组 Geometry Collection 时，缩略图预览组件曾因变换数据数量不一致触发渲染线程断言 `NumTransforms == Transforms.Num()`。5.8 预发布版已修复。修复尚未进入 `main` 或现有 5.4 版本。5.4 有同类风险，依据是源码，尚未实测。

   其他写入功能首次使用后需检查结果。未验证关闭编辑器的稳定性、Fracture 和 PlanarCut 同时禁用时的启动情况。Mac 结果见第 18 项。

   UE 5.8 官方实验性 MCP 默认使用端口 8000、路径 `/mcp`。每次启动编辑器需执行 `ModelContextProtocol.StartServer` 或开启自动启动。服务器代码没有鉴权。2026-11-06 单次中文编辑器实测中，`write_graph_dsl` 写入失败，`read_graph_dsl` 读回为空。英文界面未测，此结果仅限当次测试。

18. UE 5.8 Mac 验证（2026-11-07）：Apple M1 Max，macOS 27.0.1，UE 5.8.4，BuildId 55116800，Xcode 27.0（Apple clang 21），基于 `build/ue58` c6505b6，插件代码未改动。BuildPlugin 约 4 分钟，三个 dylib 均为 Universal，134 条警告全部是 `-Wdeprecated-declarations`。逐项记录见 docs/ue58-mac-test-plan.md。

   已实测（工程目录安装）：插件加载和 MCP 连接；中文蓝图文本写入、往返和编译；复制蓝图、删节点、删函数；材质批量建图和材质实例；Niagara 创建及 PIE 中可见；Python（arm64）；生成 Actor、PIE 和截图；Geometry Collection 创建、40 site 种子 7 切成 38 块、Auto Cluster 得 [1, 8, 38]（均与 Windows 一致）、撤销重做、物理设置、重启后读回、PIE 落地破碎；连接 MCP 时退出编辑器 3 次，均正常结束。

   本轮修复：Mac 上在 Finder 双击 `.uproject` 时命令行不带工程路径，helper 自动识别失败。helper 改为从编辑器打开的文件反推工程，Python 和 Node 版都读到正确令牌。纯蓝图工程配引擎目录插件时可能认不出，未实测。

   已知问题（插件未改）：`McpNativeTransportRunLoop.cpp` 的 `SetReuseAddr(true)` 在 Mac 上同时设置 `SO_REUSEPORT`，所以两个编辑器都能监听 3000，且日志都显示启动成功。5.8 先开、5.4 后开时，6 次请求全部进入 5.4，5.8 令牌返回 401。不同工程令牌不同，请求会被拒绝；同一工程开两个编辑器时令牌相同，请求可能进入另一个编辑器（未实测）。helper 和 `scripts/mcp-smoke.py` 发现多个监听者时报错，但其他客户端不受此保护。5.4 代码相同，未单独评估；Windows 行为未验证。

   小问题：截图默认目录在 Mac 上也是 `Saved/Screenshots/WindowsEditor`，路径写死在源码里。

   未验证：编辑器目录安装；Fracture 和 PlanarCut 真正缺失时的启动。在 `.uproject` 中禁用后，ChaosEditor 等引擎插件的必需依赖仍会挂载它们；dylib 直接链接这两个模块。内容浏览器缩略图打开时切割也未专门测试。其他写入功能首次使用后需检查结果。

19. Niagara 系统级 Scratch 模块（2026-11-07，UE 5.8 Mac）：新增 `create/edit/apply/get_niagara_scratch_module`，脚本存入 `System.ScratchPadScripts`，所以出现在「本地模块」。代码在 `Domains/NiagaraAuthoring/McpNiagaraScratch.cpp`，能力记录在 `MCP/Extensions/niagara-scratch.capabilities.json`，只在 UE 5.8 编译，其他版本返回 NOT_SUPPORTED。

   NiagaraEditor 没有导出 Map Get/Set、If、Select 的节点类和 `RequestNewTypedPin`，插件用反射按名字取类和属性，用虚函数指针调用 `OnNewTypedPinAdded`，不改引擎，不新增模块。新引脚总排在 Add 引脚之前，第二版 Custom HLSL 也要走这条路，否则会重现按引脚下标越界的闪退。

   同时修复：`add_spawn_burst_module` 改到 Emitter Update；用户参数和绑定支持 Position、Quat，未知类型报错；`set_dynamic_input` 能接上 Set Variables 的输入。

   已实测（工程目录安装）：在复制出的测试系统里重建 `CrowdLateralOffset`（SamplePos + RightVec * Lateral 写入 Particles.Position），绑定输入后编译通过并保存，模块列在「本地模块」，双击能打开图表（系统级与发射器级同名时也可以），系统编辑器打开不崩，预览中粒子的横向队列与原系统一致；另一个测试系统覆盖 If、Select、函数节点和六种声明类型；连线缺失时编译错误能归到对应脚本且不保存；未知类型、未知运算会报错；上述三个修复的结果。

   已知情况：系统编辑器开着时写入返回 SYSTEM_EDITOR_OPEN；声明输入不能带默认值，用 `set_parameter_value` 设；If、Select 的引脚名随编辑器语言变化，要从回复里读；可转换的类型连线会被引擎自动插入转换节点。校验 `NS_ScratchFlow` 时引擎堆栈 UI 对 Sample Spline 的 Distance Distribution Mode 报过一次重复子项 ensure，与 Scratch 无关，但未确认原系统是否同样触发。

   未验证：Windows；运行关卡（PIE）中的粒子位置，以上位置结论来自预览对照，不是数值比对。

20. Niagara Scratch 的 Custom HLSL 节点（2026-11-07，UE 5.8 Mac）：`add_node` 增加 `kind: custom_hlsl`（`hlsl`、`inputs`、`outputs`），新增 `set_hlsl` 操作，`get_niagara_scratch_module` 返回节点代码。

   引脚走第 19 项的 `AddTypedPin`：新引脚排在 Add 引脚之前，`UNiagaraNodeCustomHlsl::OnNewTypedPinAdded` 同步 Signature。引擎的 `BuildParameterMapHistory` 按引脚下标取 Signature，只跳过 Add 引脚，所以引脚排到 Add 之后会越界闪退（VibeUE 的 `add_pin` 用 `CreatePin` 就是这样）。代码属性是私有的，`SetCustomHlsl` 未导出，这里用反射写入后调用 `PostEditChangeProperty`，与细节面板同一路径；它会按 Signature 重建引脚，连线和默认值按名字保留，因此创建时先加引脚后写代码。引擎会静默改写的引脚名（含空格、保留字、节点两侧重名）用它自己的 `VerifyEditablePinName` 提前拒绝，否则代码里的名字对不上。加完引脚清掉待重命名状态，避免图表打开时引脚名进入编辑框。节点中途失败会整个移除，不留半成品。

   已实测（工程目录安装）：复制 `NS_ScratchFlow` 为测试系统，用 HLSL 重写横向偏移模块，编译通过并保存；用户对照新旧两个系统的预览无异常，系统编辑器打开、双击打开图表、重启后从磁盘加载都不崩，代码和引脚保留；脚本统计中生成和更新脚本的操作数与原系统一致。另测：非法引脚名和节点两侧重名被拒绝并带引擎建议的名字，失败不留节点，语法错误不保存，已连线节点上加引脚和 `set_hlsl` 后连线与默认值保留，未连线的输入引脚可用默认值。

   已知情况：不支持 Map 类型引脚；数据接口输入支持见第 21 项，也不能删除或改名引脚（删节点重建）。VM 编译错误不带节点信息，所以语法错误只出现在 `compiledScripts` 且 `compileOk` 为 false，`scratchScripts` 仍显示 ok。

   引擎问题，与本项无关：打开使用 Sample Spline 的系统并编译时，`NiagaraStackEntry.cpp:832` 报 `Duplicate child Distance Distribution Mode`，堆栈中没有本插件；ensure 拉起的 `CrashReportClientEditor` 退出时又在 `FScheduler::StopWorkers` 崩溃，弹出系统崩溃窗口，编辑器不受影响。两者都在引擎内，本仓库不改引擎。Mac 编辑器读偏好设置 `bSendUnattendedBugReports`，关闭后 ensure 不再拉起报告程序。

   数值对照已实测（scratch-7，编辑器世界）：HLSL 的 `NS_ScratchHlsl` 与 `NS_ScratchFlow` 各 31 个粒子，同样推进 120 × 0.0333 秒后 age 均为 3.9959964752；Y、Z 均为 0，X 范围分别约为 -373 到 355、-376 到 378，均值分别约为 25.010115、-26.279506。未固定随机种子，只能记为本次样本分布相近，不是逐粒子比对，不能据此证明两系统严格等价。未验证：Windows；固定随机种子的逐粒子数值对照；未在未经 Scratch 修改的系统上单独复现上述 ensure。

21. Niagara Scratch 数据接口输入类型（2026-11-07，UE 5.8 Mac）：仅 Scratch 的解析器先调用共用 `ResolveNiagaraTypeByName`，再查已加载的非抽象 `UNiagaraDataInterface` 子类，支持完整类名和省略 `NiagaraDataInterface` 前缀（`NiagaraDataInterfaceArrayFloat` / `ArrayFloat`），不加载额外插件。未知、抽象、重名类均拒绝。类型用 `FNiagaraTypeDefinition(UClass*)`；CDO 的 `PostInitProperties` 负责类型选择 flags（Array 基类为 AllowAnyVariable | AllowParameter），再以 `FNiagaraTypeRegistry::Register(Type, None)` 保证注册而不扩大原 flags。共用用户参数解析器不变。

   仅 Map Get 输出（模块输入）和 Custom HLSL 输入接受数据接口；`declare_output`、Custom HLSL 输出、Map Set、op/if/select 拒绝。继续走第 19、20 项的 AddTypedPin、反射与虚函数路径，没有新增 NiagaraEditor 未导出符号调用。

   已实测（UE 5.8.4 Mac，工程目录安装）：`ArrayFloat` 可声明 Module.Arr 并连接 Custom HLSL 的 Arr 输入；未手动赋值/配置接口实例，`Arr.Length(Count);` 的首次 apply 为 compileOk true、compiledScripts 空。declare_output ArrayFloat、Custom HLSL 输出 ArrayFloat、Map Set 的 DI 引脚、declare_input/declare_output NoSuchType 和抽象 NiagaraDataInterface 均被拒绝；抽象基类报 Unknown type，未单独说明抽象原因。scratch-6 再测 Map Set 的 ArrayFloat、NoSuchType，两次都拒绝，节点数 5 → 5，完整图和 Map 链不变。

   运行验证（scratch-7，编辑器世界）：只修改 `NS_DiTest`。确认 Crowd 的 HlslDiTest 已启用且在 Particle Update 栈末尾；断开 Count → Particles.DiTestCount，增加 Result（Position），代码改为 `Arr.Length(Count); Result = float3(0, 0, 1000.0 + Count);`，Result 写入 Particles.Position。apply 为 compileOk true、compiledScripts 空，保存成功；未手动赋接口实例。临时 Actor 激活并推进 120 × 0.0333 秒后读回 31/31 个粒子，age 3.9959964752、localSpaceEmitter false，全部 Position 为 (0,0,1000)，Z 最小值、最大值均恰好为 1000。按此测试判据，ArrayFloat 的 Length 调用已运行且返回空数组长度 0。本轮没有重试调整；临时 Actor 已删除，未保存关卡。此前 DiTestCount 读不到时只说明该属性不在当前编译缓冲中，未单独证明原因是编译优化；本次改写 Position 后验证了函数结果。

   已知情况：声明数据接口输入时会出现 `ResetVariableToDefaultValue called, but failed on var Arr type NiagaraDataInterfaceArrayFloat.` 警告，本次未阻止编译和运行，日志无新增 Ensure、Fatal、Array index out of bounds 或 Sample Spline Duplicate child。接口只声明类型，不提供手动创建、赋值、配置接口实例的能力；本次没有追踪引擎自动创建/提供默认实例的内部过程，不能将“未手动赋值也可用的空 ArrayFloat”推广到所有数据接口。源码核对：Map Get 的默认引脚对接口不分配普通值，字符串默认值不能配置接口；堆栈 `UNiagaraStackFunctionInput::SetDataInterfaceValue` 创建 override pin，`SetDataInterfaceValueForFunctionInput` 创建拥有接口对象的 NiagaraNodeInput 并连接，5.8 external 编辑还通过 placeholder manager 同步。未验证：非空数组数据赋值/配置、其他 DI 类、完整类名作为请求 type 的单独用例、op/if/select 的 DI 负面实测、接口实例绑定、PIE、Windows。

22. 关卡 Niagara CPU 粒子快照（2026-11-07，UE 5.8 Mac）：新增只读能力 `get_niagara_particle_data`，源码 `McpNiagaraParticleData.cpp`，Extensions 独立记录并由生成器注册。`actorName`、`emitterName`、`attributes` 必填，`maxParticles` 默认 100、范围 1–2000；可显式传 `advanceTicks`（0–10000）、`tickDeltaSeconds`（大于 SMALL_NUMBER、至多 1 秒，默认 1/60），后者不能单独传。effect 为 read；显式推进会改变瞬时模拟状态，因此记录为非幂等、不可安全重试，不保存资产。

   读取路径：游戏线程选 PIE PlayWorld，否则编辑器世界；按精确 label/object name/path 匹配并拒绝歧义，Actor 必须只有一个 Niagara 组件。通过组件 controller 的内联 `GetSystemInstance_Unsafe()` 取得实例，调用已导出的 `WaitForConcurrentTickDoNotFinalize()` 等待系统与实例并行任务；不 finalize、不激活、不重置、不 tick。然后 GetEmitters → emitter GetParticleData → GetCurrentData，用引擎 typed dataset reader 读 Float/Int/Bool/Vector2/Vector/Vector4/Color/Position/Quat，返回真实总数和截断数量；属性接受有/无 Particles. 前缀。GPU/stateless 在读取前返回 NOT_SUPPORTED，不做假 CPU 读回；无实例、未激活分别说明；已完成实例返回 SYSTEM_COMPLETE，未请求推进且 pending-first-tick/无缓冲返回 NO_SIMULATION_DATA，并提示传 advanceTicks 或在 PIE 中读取，Actor/组件/发射器/属性缺失列出可用名字。

   线程安全与推进：整个快照在同步游戏线程调用内完成，不留出 raw buffer；等待函数不触发完成回调。只有 advanceTicks > 0 调用导出的 `UNiagaraComponent::AdvanceSimulation`（引擎内部等待/finalize、临时 Solo、同步 ManualTick、恢复 Solo）；暂停则拒绝。scratch-7 修复：推进前只检查激活、有效 controller/实例、未完成、未暂停，pending spawn 或无当前缓冲不提前报错；已有缓冲时先验证目标发射器、CPU 类型和可读属性，否则推进后完整验证。源码确认 `SetSolo` 可转移 PendingSpawn，`ManualTick` 进入 `FNiagaraSystemSimulation::Tick_GameThread_Internal` 后处理 pending 队列并转成 Running；编译/执行未就绪时仍可能不前进，由现有 age 检查报告。网关会提前注入 schema default，因此移除 advanceTicks 与 tickDeltaSeconds 的 schema 默认值（C++ 默认 0、1/60 不变），保留单传 tickDeltaSeconds 的 INVALID_ARGUMENT 检查。scratch-7 编辑器世界的推进读取已实测通过。推进后重新取得弱组件、controller、实例、发射器和缓冲，对比实例 ID、系统 age，拒绝替换实例或没有实际推进。推进可能完成/回收系统，之后失败不回滚，调用者不可盲目重试。本项没有使用新的未导出 NiagaraEditor 函数；快照访问器内联，等待和 LWC 函数有 NIAGARA_API。

   Position 通过 `GetLWCSimToWorld(IsLocalSpace)` 变换，包含世界发射器 LWC tile 和局部发射器变换，输出世界空间、UE 单位；其余向量保留模拟空间，Quat 顺序 xyzw，Color rgba。索引只在同次快照各属性之间对应，不是跨调用稳定粒子 ID；编译优化移除的属性无法读取，列可用属性；无效 reader/非有限数值报错，不填零。未验证：PIE 及 PIE/编辑器世界选择对照、局部空间发射器、远离原点的 LWC、GPU/stateless 发射器的 NOT_SUPPORTED 实测、其余支持类型的逐项读取、异步压力、完成/池化回调、暂停/完成状态错误的单独实测、Windows。编译结果以本次任务回报为准，不将编译通过当成实测。

   已实测（scratch-7，BP_Test 5.8.4 Mac，非 PIE 编辑器世界）：新激活的 NS_ScratchHlsl 临时组件不传 advanceTicks 时返回 NO_SIMULATION_DATA，并提示传 advanceTicks > 0 或在 PIE 中读取；同一组件传 120 ticks、0.0333 秒后成功返回 Position/Velocity，总数和返回数均 31、age 3.9959964752、localSpaceEmitter false。随后两次不推进读取的 age、particleCount 均相同；maxParticles=5 返回 5 个，总数仍为 31。NS_ScratchFlow 同样推进成功且 age/数量一致，位置分布对照见第 20 项。不存在发射器返回 EMITTER_NOT_FOUND 并列 Crowd，不存在属性返回 ATTRIBUTE_NOT_FOUND 并列可用属性，单传 tickDeltaSeconds 返回 INVALID_ARGUMENT。三个临时 Actor 全部删除并列表确认 0，未保存关卡，未写入四个受保护原系统；日志增量无 Ensure、Fatal、Array index out of bounds 或 Duplicate child。NS_DiTest 的空 ArrayFloat 运行验证见第 21 项。

23. Niagara 数据接口实例赋值与配置（2026-11-08，UE 5.8.4 Mac）：新增 `set_niagara_data_interface`（write）和 `get_niagara_data_interface`（read），位于 `McpNiagaraDataInterfaceConfig.cpp` 和独立 Extensions 记录。通过 `systemPath` 加 `moduleInput` / `userParameter` 二选一定位；模块路径第一版只支持发射器栈，要求 `emitterName`，用户参数路径不要求 emitter。类名复用第 21 项解析器；新建用户参数须给具体 DI 类，接受有/无 `User.` 前缀；已有非 DI 或不同类参数返回 PARAM_TYPE_MISMATCH，不替换。模块类须为输入声明类的子类。

   预校验：系统资产编辑器打开时返回 SYSTEM_EDITOR_OPEN。仅开放 CPF_Edit，排除 EditConst、Deprecated、Transient；先把所有 properties 写到 transient 临时实例，失败不改资产。真实对象 Modify 后复制已经校验成功的属性，逐属性 PostEditChangeProperty，最后广播 OnChanged。ArrayFloat3 等 LWC 数组在 WITH_EDITORONLY_DATA 下靠该事件同步内部数据。现有属性写入器的 FSoftObjectProperty 分支已支持 SoftSourceActor 的 Actor 路径字符串，无需修改共用导入器；路径赋值本身不验证 Actor 是否存在。

   模块 override 通过已有模块/input 定位与已导出的 GetOrCreateStackFunctionInputOverridePin、SetDataInterfaceValueForFunctionInput 创建。同类 NiagaraNodeInput 原地更新 DI，不改图；不同输入先清理旧连接。GetDataInterface、GetStackFunctionInputOverridePin、RemoveNodesForStackFunctionInputOverridePin 和默认 pin 查询未导出，分别使用反射、只读图遍历、抽取现有 LinkInputToParameter 清理逻辑；不调用 placeholder manager。清理保留仍被其他输出连接使用的节点。OnChanged 来自 NiagaraCore 的已导出函数，因此仅 UE 5.8 增加该现有模块的链接依赖，没有新建模块或 Target；其他版本的新能力返回 NOT_SUPPORTED。

   编译与同步：模块路径 RequestCompile(false) 并等待完成，compileOk 判据与 Scratch apply 相同。数组内容不进入编译哈希，因此再经已导出的 GetResolvedRuntimeInstanceForEditorDataInterfaceInstance 定位运行时实例，Modify(false) 后 CopyTo，并按解析名称同步相应脚本 CachedDefaultDataInterfaces。两份数据均可序列化；本次 ArrayFloat 的保存/重启实测通过。仅编译及同步成功时按 save（省略为 true）保存；失败保留内存修改、不保存。用户参数直接更新 exposed store，不主动编译。get 不创建 override、不编译、不保存，返回 inline/linked/default、连接参数、类和可编辑属性；数组最多返回 256 项并带真实长度。无法解析为具体编辑器实例的复杂连接/默认值返回 valuesAvailable=false 和 null，不伪造运行时值。

   已实测（安装 scratch-8d，BuildId 55116800，编辑器世界，非 PIE）：只写 NS_DiTest，Crowd 的 HlslDiTest。每次数值测试新建临时 NiagaraActor，激活后以 advanceTicks=120、tickDeltaSeconds=0.0333 读取 Position，均为 31/31 个粒子，Z 的最小/最大值相同。结果：
   - ArrayFloat FloatData=[1,2,3]，Length 写入 1000+Count：Z 全部 1003，compileOk=true、compiledScripts=[]、runtimeSynced=true。
   - 不改图（完整 Scratch 图读回一致），仅 FloatData 改为 5 项：Z 全部 1005；仍使用 RequestCompile(false)，运行时实例更新数 1、缓存条目更新数 2，无需 RequestCompile(true)。本项只证明本次路径有效，不声称所有 DI 均无需强制编译。
   - FloatData=[10,20,30]，`float V; Arr.Get(1,V);` 写入 1000+V：Z 全部 1020。保存后 get 为 inline 且数组正确；用户重启后再次 get 仍为 [10,20,30]，不再赋值直接运行仍为 Z=1020。
   - 新用户参数 DiArr（ArrayFloat，[1,2,3,4]）在不传 emitterName 时创建成功、compileRequired=false；经现有 bind_parameter_to_source（网关 edit_niagara_system/edit=bind_parameter）绑定 HlslDiTest.Arr，get 返回 linked、User.DiArr 和四项数据，Length 得 Z 全部 1004。
   - 新输入 Arr3（ArrayFloat3），FloatData=[[1,2,3],[4,5,6]] 读回一致，Length 得 Z 全部 1002。没有以 Get 单独核对向量各分量。
   - 新 Spline 输入：临时 Actor 上两个线性点间距 300，create_spline 读回长度 300；SoftSourceActor 设为其对象路径，`Spline.GetSplineLength(L)` 写入 1000+L，Z 全部 1300。临时 Actor 引用使用 save=false；测后清空引用、删除样条并保存系统，未将临时引用留在磁盘。
   - 拒绝测试：Arr3 指定 ArrayFloat → PARAM_TYPE_MISMATCH（必须是输入类的子类）；InternalFloatData → INVALID_PROPERTY（列出 FloatData、GpuSyncMode、MaxElements）；非 DI ScalarCheck → PARAM_TYPE_MISMATCH；先给合法 FloatData 再给 NoSuchProperty → INVALID_PROPERTY；FloatData 给非数组字符串 → INVALID_PROPERTY_VALUE。全部拒绝，之后 get 的完整数据与之前相同，FloatData 仍为两个原向量。

   测后 HLSL 恢复为 Arr.Length，Arr 仍 linked 到 User.DiArr，最后另建临时 Actor 验证 Z 全部 1004；Arr3 保留两个向量，Spline.SoftSourceActor=null，ScalarCheck 为负面测试新增 Float 输入。每个临时 Actor 均删除回执 existsAfter=false，最终按 EW_DI_ 查得 0；未保存关卡，未写 NS_CrowdFlow、NS_CrowdRanks、NS_ScratchFlow、NS_ScratchHlsl。两段测试日志增量均无 Ensure、Fatal、Array index out of bounds 或 Sample Spline Duplicate child；创建样条的首次请求因 points JSON 层级错误在 schema 校验阶段被拒绝，修正为 position 后成功，没有对应资产/Actor 副作用。本次不需要代码修复或 scratch-9/10 构建。

   用户最终确认：NS_DiTest 系统编辑器和 HlslDiTest 模块图表均能打开，Unreal Editor 未退出。但打开后在本地 2026-11-08 00:10:57 的日志中新增 NiagaraStackEntry.cpp:832 的 Duplicate child Distance Distribution Mode ensure，与第 20 项已记录的签名一致；00:11:12 的 CrashReportClientEditor 诊断报告显示 EXC_BAD_ACCESS，故障线程包含 FScheduler::StopWorkers。MCP 此后仍可响应。该 ensure 的可见堆栈在 Niagara/引擎内部，没有 EngineWire 帧，但没有单独对未修改系统做本轮对照，不能凭此证明触发与插件或测试改动完全无关。用户看到白色报错窗口，其外观未直接检查，可能对应上述报告程序；未修改隐私设置或引擎。此前“日志无新增”的结论仅限数值测试阶段，不能延伸到这次 UI 打开检查。

   未验证：PIE、Windows、GPU、局部空间/LWC；超过 256 项的截断实测；其他 DI 类、ArrayFloat3 各分量数值调用、复杂非用户参数连接、共享/嵌套动态输入清理；仅改内容后同步的跨发射器/多脚本压力测试；用户参数单独保存重启、Spline/ArrayFloat3 的独立保存重启、持久关卡 Actor 软引用的重启解析。本次 ArrayFloat 的持久化结果不推广到所有数据接口。

   事务与回滚（scratch-10，BuildId 55116800，Mac 双架构）：写入在单独的编辑器事务中进行，先快照图、DI 实例、运行时与编译缓存副本和撤销历史；已消除事务上下文 ensure。已实测：正常设置并保存后包不脏；错误属性预校验返回 INVALID_PROPERTY 且不改数据和脏标记；干净状态下同步失败返回 DI_SYNC_FAILED，rolledBack、graphRestored、runtimeAndCacheRestored、undoHistoryRestored 全为 true，节点 37→37；回滚后撤销只撤销前一次成功操作（[31,32]→[21,22]），不崩溃；回滚后可重新创建 DI；成功后撤销恢复 linked→User.DiArr；重复设置复用同一实例；用户参数 [1,2,3]→[5,6,7,8] 复用实例且无需编译；多 DI 系统写入无 DI_SNAPSHOT_FAILED。日志新增 Ensure/Fatal/越界/Duplicate child 均为 0。

   已知限制：刚撤销、包已脏且存在重做记录时如再发生同步失败，重新编译后的 DI 条目数可能与调用前不同，此时如实返回 rolledBack=false、runtimeAndCacheRestored=false，residualChanges 写明 “Compiled DI entry count differs from the original.”；图和撤销历史已恢复，包保持脏，重做可用。恢复办法（重新 set 或撤销/重做）未实测。未实测：DI_OVERRIDE_LINKS_REMAIN、CopyTo 中途失败、实例创建失败、单独编译失败路径（未做故障注入）。

24. 仅删除未引用的 Niagara 用户参数（2026-11-08，UE 5.8 Mac）：新增 `remove_niagara_user_parameter`，独立 Extensions 注册，实现在 `McpNiagaraUserParameterRemoval.cpp`。参数仅 `systemPath`、`name`（接受 `X` / `User.X`）；effect=destructive，网关要求 describe 返回的一次性 explicit consentGrant。不提供 force、rename 或 save。被引用的参数请在编辑器里手动删除或改名；需要迁移时先添加新参数，手动更新引用，再删旧参数。

   接口边界：核对本机 UE 5.8.4 头文件与 dylib 导出后，经 `FNiagaraParameterStore&` 的 `RemoveParameter(const FNiagaraVariableBase&)` 正常虚调用，保留引擎 UserRedirection store 的重定向处理；没有限定调用基类来绕过 override。没有复制/抽取未导出的 ViewModel 代码，也没有新模块/Target。原生面板的 `FNiagaraSystemViewModel::DeleteParameter` / `RenameParameter` 及 `DeleteUserParameterReferences` / `ReplaceUserParameterReferences` 未导出，故本能力不支持引用同步、改名或强制删除。

   预检只读：系统 spawn/update、全部 stateful CPU 发射器脚本（含 event/stage）、系统内局部 Scratch 图表，以及递归调用的模块/动态输入脚本（通过已导出的 GetFunctionScriptSource 跟随 SelectedScriptVersion）；检查所有节点引脚名字和默认值、相邻链接引脚（返回模块输入路径）、节点的反射 binding/name/string/struct/array 属性（包含 Custom HLSL 源文本），以及系统内 renderer/DI 的对应绑定属性。命中返回 PARAMETER_IN_USE 和 references[]，不调用 Modify、重编译或标脏。缺失脚本源或无法检查的发射器返回 REFERENCE_SCAN_INCOMPLETE；当前系统编辑器打开或 PIE 时拒绝；GPU 发射器在写入前返回 NOT_SUPPORTED，本版不声称验收 GPU shader 编译。

   名字按命名空间 token 匹配，不把 User.X2 当成 User.X；同时匹配 User.X.Member。扫描是保守检查：注释、默认值或非活动脚本中的同名引用也可能阻止删除；不是 HLSL 编译器或跨资产引用索引，动态拼接/宏/外部 include 的语义引用不能保证识别，且不检查其他资产或关卡组件的覆盖参数。不可把 references=[] 推广为工程范围没有引用。

   写入：一个 FScopedTransaction，Modify 系统、原 DI 和系统/发射器脚本后删除；强制请求编译并等待，标脏但绝不自动保存。返回 undo（一个步骤）、compileOk、compiledScripts 和 warnings。撤销应恢复参数及原 DI 实例，重做再次删除；运行时/缓存编译状态必须在撤销/重做后另行读回核对。失败或编译异常不自动重发，不声称恢复原状态。

   DI 核对：返回删除前后 exposed DI 数、runtime resolved DI 数、cached default DI 数和仍指向被删参数的条目数；检查 Name/CompileName 及 runtime 的 ResolvedVariable/ParameterStoreVariable、cached 的 RegisteredParameterMapRead/Write。确认 exposed 数减少正确且被删参数无残留，否则给出 warnings。模块内联 DI 也进入编译条目，总编译条目数不应直接等于 exposed DI 数；此检查不证明其他 DI 的绑定/内容都正确。

   验证状态：已加入契约检查和引用 token/链接路径、错误恢复指引的引擎 Automation 测试。scratch-11 构建、安装、临时 Float/DI 的删除与撤销/重做、引用拒绝、缺 consent 和不存在参数、A-2 清理的实际结果以任务报告为准；未实测前不标记通过。Windows 未编译。

   scratch-11 重启后实测：TmpDelF 删除 → undo 恢复 → redo 删除通过；TmpRefF 绑定 SpawnRate.SpawnRate 后删除返回 PARAMETER_IN_USE，references 指到该输入，拒绝前后 Niagara info 相同；撤销独立临时绑定事务后 SpawnRate 恢复 8，再删除通过。TmpDi=[11,22,33] 未引用时删除，exposed DI 3→2，编译 runtime/cache 条目各为 10、被删参数残留 0；undo 后 get 完整恢复原数组，再删除通过。全部临时参数/链接清理。DiRollbackTest 预检 references=[]，删除并编译通过，exposed DI 2→1、残留 0；Arr 始终 linked User.DiArr，FloatData=[1,2,3,4]，列表仅剩 User.DiArr。单独保存 NS_DiTest 后 dirtyContent=[]。runtime/cache 总数包含内联 DI，不与 exposed 总数直接比较。

   错误路径：缺 consent 正确返回 CONSENT_REQUIRED 和 describe nextCall；不存在参数返回 PARAM_NOT_FOUND，但最初 scratch-11 缺少 nextCall，引用拒绝同样缺少。已定位网关原有 handler-error 分支不附指导，仅对新增 remove 动作复用 DescribeGuidance，保留其他动作行为；补丁构建与重启后的回执验收以任务报告为准，尚未运行前不记为通过。撤销时观察到 cached DI 数可变为 0，下一次删除重编译后恢复 10；没有把 undo 的参数/内容恢复等同于编译缓存完全恢复。

本仓库不上传测试资产、私有场景、模型、专用动画、截图、认证文件或本机完整日志。
