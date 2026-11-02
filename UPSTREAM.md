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

本仓库不上传测试资产、私有场景、模型、专用动画、截图、认证文件或本机完整日志。
