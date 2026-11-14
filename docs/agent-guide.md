# Agent 正常使用 MCP 插件

统一规则：优先已验证的 EngineWire MCP；成熟批量任务可用编辑器内 UE Python；独立 Remote Execution 备用。MCP 提供 Python 执行能力时可沿现有连接执行，不必另开网络桥；不安装或依赖任务按钮。

小任务快速通道：建文件夹、移动、改名、复制资产或文件夹、Actor 改名或归入大纲文件夹这类规则明确的机械操作，按[共享协议第 0 节](https://github.com/dadaozei01/ue-ai-bridge-protocol/blob/main/docs/agent-entry.md)处理：不再读本页其余章节，两项及以上用一段 `system_control.execute_python` 脚本一次做完，单项用最顺手的 MCP，不逐项 search/describe。

跨 Agent 共享操作规范见 [ue-ai-bridge-protocol](https://github.com/dadaozei01/ue-ai-bridge-protocol/blob/main/docs/agent-entry.md)，本页负责 EngineWire 的具体调用方式。

## 官方 MCP 与 EngineWire 的分工（UE 5.8）

UE 5.8 官方 MCP（默认 8000/mcp，实验性、无鉴权）与 EngineWire 同时开着时，默认只用 EngineWire：官方工具集经 `list_editor_toolsets` / `call_editor_tool` 调用，不直连官方服务器，这样仍有令牌、consentGrant、撤销回执和警告。Niagara、Chaos、PIE、截图、编辑器重启、批量 Python 只走 EngineWire。蓝图、材质、网格、数据表等通用编辑，官方有正好对应的工具时经 `call_editor_tool` 调官方的，没有再用 EngineWire 自带能力。EngineWire 未安装或连不上时才直连官方，之后能接上 EngineWire 再读回或截图核对。一个任务只用一个入口，中途不换；一边报不支持，先在另一边查一次再下结论。升级引擎或启用新插件后，先用 `list_editor_toolsets` 看官方新增了哪些工具集。

## 连接

1. 确认目标工程和引擎版本，避免连错同时打开的 UE。先检查工程已有自动化入口，保留原有入口。
2. 启用 EngineWire 的 Native MCP Server 并重启 UE。默认端点：http://127.0.0.1:3000/mcp。8090/8091 是其他传输端口，不是这里的原生 HTTP。
3. 请求头 X-MCP-Capability-Token 从目标工程 Saved/MCP/capability-token 动态读取。不要打印、粘贴到对话或提交令牌；不要为连接开放公网。
4. 配置 Agent 的 HTTP MCP 客户端，重新加载客户端。首先只读核对目标工程绝对路径、实际引擎版本和编辑器 PID，必须唯一匹配用户指定目标；不能仅凭同名 Actor 判断。身份不能确认时停止写入并澄清。

## 工具调用

工具名可能带客户端前缀，入口为 unreal，四个操作是 search、describe、execute、configure。

1. search：用 2–4 个普通词定位能力，例如 Actor list、material create、blueprint graph。
2. describe：按搜索返回的 nextCall 查询真实参数 schema。
3. execute：按 describe.nextCall 调用，只传 schema 允许的参数，不臆造 action/subAction。
4. 如返回 consentGrant，按当前响应要求传递 capability、acknowledge、nonce；一次性使用，不缓存。这是技术机制，不能代替用户对任务的授权。
5. 读回执行结果，再进行必要的编译、保存和场景验证。configure 用于工具组配置，不是场景编辑。

原始 HTTP 响应可能为 JSON 或 SSE，优先使用标准 MCP 客户端。不要直接套用旧版本参数。

## 操作与验证

- 先查再改，新增测试使用独立路径，覆盖前检查已有资产。编辑 .uasset 必须通过 UE API，不直接修改二进制。
- 蓝图执行引脚水平对齐、流向从左到右、间距一致；纯数据节点放附近下方，保留用户手工排布。
- 材质批次失败可能已部分写入，先读回再继续；超时也不能直接重复写入。
- 优先原生 MCP 的明确能力，成熟批量 UE Python 也可使用。合理批量处理，不为强制省额度牺牲质量，也不做无意义重复查询。
- 不自动保存所有脏关卡；切关卡与 PIE 前检查状态，只停止自己启动的测试。
- 动画生成新资产，不改 Skeleton 参考姿势；以用户场景相机验证脸、轮廓与肢体，而不只看资源预览。
- 记录参数读回、编译、保存、运行、视觉效果分别是否通过。工具返回成功不等于最终效果通过。
- 长任务返回 Still running 与 task.taskId 时，用 manage_tools 的 get_task_result 读结果，不重发原调用；预计较慢时可在 options.timeoutMs 放宽等待。
- 资产改名：单个用 asset.rename；多个各自不同的新名字用 manage_asset maintain_content（maintenance=bulk_rename）的 renames 列表一次完成，改完 list 读回。改名会留下重定向器，需要清理时用 maintenance=fixup_redirectors。
- 重命名当前打开的关卡后，编辑器会切到 /Temp/Untitled_N 临时世界（Windows 实测），需提醒用户重新打开改名后的关卡。

## 先找已有能力

- 判断"没有这个能力"或改用 Python 自写前，换 2–3 组动词和对象再 search，或按 domain 过滤，或 describe 不带选择器逐层浏览。只搜一次就下结论容易漏。
- 动态效果验证先用：control_editor 的 step_frame（暂停后逐帧推进）、control_actor 的 sample_motion（按游戏时间采样位置、速度和指定属性，可定时模拟按键）、screenshot、inspect 的 describe_view 与 capture_passes。不在用户编辑器里挂临时 tick 回调。
- 改蓝图逻辑先用 manage_blueprint 的 read_graph_as_code 读成文本，改完用 write_graph_from_code 写回（Epic 5.8 蓝图 DSL 文本格式，0.6.0 起），少用逐节点建线；改完 compile 并检查无用节点。本仓库已修复中文界面、多输出函数、运算节点等读写问题（UPSTREAM.md 第 8–15 项）；写回时保留读出的 `; id` 注释，菜单里没有的节点（如 SpawnActor<蓝图类>）靠它复用原节点，去掉注释会被整体拒绝。写入失败会整体拒绝，不会写坏蓝图。
- Niagara：create_effect 建拖尾后读回 SpawnRate.SpawnRate，若为 0 用 set_parameter_value 重设；跟随拖尾（LocationBasedRibbon）的发射器静止且粒子无速度时长度为 0、看不到。看特效以 PIE 画面为准，截图回执里"视口不是实时"的提示在 PIE 下也会出现。用户开着某资产的 Niagara 编辑器时，其副本保存会覆盖 MCP 写入。

- Niagara Scratch（UE 5.8）：要写自定义模块用 create_niagara_scratch_module 建在系统上（才会出现在「本地模块」），edit 改图，apply 编译并读逐脚本错误，get 读图。先关掉该系统的 Niagara 编辑器。运算节点的两个数字输入都要连线，否则编译失败。Custom HLSL 用 add_node 的 kind custom_hlsl（hlsl、inputs、outputs），改代码用 set_hlsl：引脚名就是变量名，区分大小写，输出直接赋值、不写 return；属性和模块输入经 Map Get 连到输入引脚，输出连到 Map Set；引擎会改写的名字（含空格、保留字、节点两侧重名）直接被拒绝。HLSL 语法错误只出现在 apply 的 compiledScripts，判断成败看 compileOk，scratchScripts 仍显示 ok。数据接口函数节点还没有。Scratch 模块输入和 Custom HLSL 输入现在可用 `NiagaraDataInterfaceArrayFloat` / `ArrayFloat` 等已加载的具体数据接口类名；输出和 op/if/select 不支持。这些 Scratch 操作只声明类型；实例赋值与配置使用下文的 set/get_niagara_data_interface。已实测：未手动赋实例的 ArrayFloat 输入可编译；HlslDiTest 位于 Particle Update 栈末尾并启用，用 `Arr.Length(Count); Result = float3(0, 0, 1000.0 + Count);` 写入 Particles.Position，推进后 31 个粒子的 Z 全部恰好为 1000，表明此空数组 Length 调用运行并返回 0。ResetVariableToDefaultValue failed 警告未阻止本次编译/运行。DI 输出、未知类型及抽象基类拒绝；Map Set 非法类型的回归测试节点数和 Map 链不变。这次空数组测试的后续非空赋值、绑定和其他 DI 实测见下节；不要把单个 DI 的结果推广到所有 DI。

## 故障定位

## Chaos 破碎（Geometry Collection）

本仓库新增（UPSTREAM.md 第 16 项），在 animation_physics 下，search「geometry collection」「voronoi fracture」「破碎」可找到。UE 5.4 和 UE 5.8 发布版提供这些命令，其他版本返回 NOT_SUPPORTED；切割与分组需工程启用 Fracture、PlanarCut 插件。

- create_geometry_collection：Fracture Mode 的 New。staticMeshPaths 一个或多个，name 默认 GC_<网格名>，path 默认网格所在目录；同名资产已存在时拒绝，不覆盖。spawnActor 可同时在关卡放 Actor（location、rotation、actorLabel、simulatePhysics）。
- fracture_geometry_collection：Uniform Voronoi（siteCount，可选 siteCountMax）或 Custom Voronoi（sites，资产空间坐标）；randomSeed（-1 随机并在回执返回）、chanceToFracture、groupFracture（默认 true）、grout、noise*、collisionSampleSpacing、boneIndices。已切过的 GC 默认拒绝（ALREADY_FRACTURED），预估碎块超过 maxPieces（默认 2000）拒绝（TOO_MANY_PIECES），都在切之前判断。要重切就新建一个 GC，不要对已切的再切。
- cluster_geometry_collection：Auto Cluster，clusterSizeMethod by_number / by_fraction / by_size / by_grid，默认对根节点分组。
- get_geometry_collection_info：只读，碎块数、簇数、层级与每层数量、包围盒、三角形数和物理设置。
- configure_geometry_collection：已放进关卡的 Actor，其组件在放置时拷贝了一份资产的伤害阈值、伤害模型和物理材质，之后改资产不会影响它（引擎行为）。用 actorName 指定时会同步该 Actor，其他 Actor 用 applyToActors 点名，没点名的列在 placedActorsNotUpdated 并提示。可设置：伤害阈值（每层一个）、damageModel、enableClustering、第一个尺寸档的碰撞类型与形状、Convex Overlap Removal 与凸包参数、质量、物理材质、Nanite。走细节面板同一条重建路径，先校验全部参数再写入。
- 写操作各为一个撤销步骤，之后保存资产；Actor 上组件级设置（碰撞配置、Simulate Physics）仍用 control_actor。Field 没有专门命令，用通用命令即可（Mac 已实测锚定生效）：control_actor spawn（spawnKind blueprint）放引擎自带的 /Engine/EditorResources/FieldNodes/ 下的场，如 FS_AnchorField_Generic（盒子缩放 1 时为 100 cm，用 Actor 缩放调大小）；control_actor edit_component 的 set_property 把 GC 组件的 InitializationFields 设为场 Actor 的对象路径数组；场的参数用 set_blueprint_variables。从上往下随时间释放需要运行时逻辑（蓝图 Timeline 或 Sequencer 移动场），尚未搭建。

MCP off：检查开关、重启及 UE 日志。401/403：检查工程身份、动态令牌和请求头。连接拒绝：确认端口和运行进程。工具不存在：重载 Agent 配置，确认使用匹配的 0.6 客户端/代理。不要输出令牌排错。

## Niagara CPU 粒子快照（UE 5.8）

`get_niagara_particle_data`：先 search「niagara particle data」并 describe。必填 `actorName`、`emitterName`、`attributes`（如 `["Position", "Velocity"]`）；`maxParticles` 默认 100，范围 1–2000。PIE 运行时只查 PIE 世界，否则查编辑器世界；Actor 必须精确且唯一，且只有一个 Niagara 组件，回复含实际世界和组件名。只支持已有有效实例的 stateful CPU 发射器；普通读取须已有当前缓冲，显式 `advanceTicks > 0` 可先推进 pending spawn 或尚无缓冲的实例，GPU/stateless 返回 NOT_SUPPORTED，不激活、不重置、不保存。错误会列出可用 Actor/组件/发射器/属性名；未被使用的属性可能被编译器删掉。

回复 `particleCount` 为真实总数，`returnedCount` 为截断数，各属性为 `{name,type,values}`，支持 Float、Int、Bool、Vector2、Vector、Vector4、Color、Position、Quat。数组分量为 xy/xyz/xyzw、Color rgba；Position 用引擎 LWC 与局部发射器变换转到世界空间（UE 单位），其余向量保留模拟空间。同次各属性按粒子缓冲索引对应，跨调用不可当成稳定粒子 ID。

普通读取只在游戏线程等待已有异步模拟完成，不推进或 finalize。只有显式 `advanceTicks > 0` 才先调用 AdvanceSimulation；`advanceTicks` 上限 10000，`tickDeltaSeconds` 默认 1/60、须大于 SMALL_NUMBER 且 <= 1，不能单独传。暂停系统拒绝推进；推进前只要求组件激活、controller 有效、实例未完成且未暂停，不因 pending spawn 或无缓冲提前退出。若已有缓冲，先验证发射器、CPU 类型和可读属性；没有缓冲则推进后再做完整验证。推进后重新核对实例和 age。推进可能触发完成/池化回调，失败不撤回已推进的状态，因此不能盲目重试。纯观察请省略两个推进参数。实例已完成返回 SYSTEM_COMPLETE；未推进且 pending spawn/无缓冲返回 NO_SIMULATION_DATA，提示传 advanceTicks 或在 PIE 中读取。两个推进参数的默认值由 C++ 提供，不在 schema 中注入，确保单传 tickDeltaSeconds 仍在任何模拟操作前返回 INVALID_ARGUMENT。已实测（BP_Test 5.8.4 Mac，scratch-7，编辑器世界）：新组件不推进返回带上述提示的 NO_SIMULATION_DATA；120 ticks × 0.0333 秒后 Position/Velocity 读取成功，31 个粒子、age 3.9959964752、localSpaceEmitter false。两次不推进读取的 age 和总数不变；maxParticles=5 返回 5、总数仍为 31；不存在发射器/属性的错误列出可用名字，单传 tickDeltaSeconds 被 INVALID_ARGUMENT 拒绝。HLSL 与 NS_ScratchFlow 各 31 个粒子，Y/Z 均为 0，X 范围约 -373..355 与 -376..378，未固定随机种子，仅为分布相近，未做逐粒子比对。临时 Actor 均已删除，未保存关卡；日志无新增 Ensure/Fatal/越界。未验证：PIE、局部空间发射器、远离原点 LWC、GPU/stateless 的 NOT_SUPPORTED 实测、其余类型的逐项读取、异步压力、完成/池化回调、暂停/完成状态错误及 Windows。

## Niagara 数据接口实例（UE 5.8）

先 search「niagara data interface」并 describe。`set_niagara_data_interface` 是 write，`get_niagara_data_interface` 是 read；系统编辑器打开时两者均返回 SYSTEM_EDITOR_OPEN，先请用户关闭该资产标签页。必须有 systemPath；moduleInput（如 HlslDiTest.Arr）与 userParameter 二选一，C++ 校验；模块路径要求 emitterName，第一版仅支持发射器栈；用户参数不要求 emitter，名字接受 DiArr / User.DiArr。dataInterfaceType 接受已加载的具体 DI 类名/省略前缀名；新建用户参数必填，模块默认使用输入类型且只允许其子类，已有用户参数类不符则拒绝替换。

properties 是反射属性对象，只开放 Edit，排除 EditConst、Deprecated、Transient；先在 transient 实例试写全部属性，任一失败则不改资产并列出可用名字。ArrayFloat/ArrayFloat3 等写 FloatData，不写 InternalFloatData；向量数组可传 [[1,2,3],[4,5,6]]。SoftSourceActor 接受 Actor 对象路径字符串或 null，字符串导入不验证 Actor 存在。临时 Actor 引用测试用 save=false，结束前清引用；不把临时世界路径保存为正式配置。模块写后检查 compileOk、compiledScripts、runtimeSynced、saved，不能仅看外层 success；编译/同步失败保留内存修改、不保存。save 省略为 true。用户参数不主动编译。数组内容不参与编译哈希，setter 显式同步运行时和 CachedDefaultDataInterfaces；不要改成普通字符串 pin 默认值。

get 不创建 override、不编译或保存；返回 mode=inline/linked/default，linkedParameter、className、properties（name/type/value）和 valuesAvailable。数组最多读 256 项，arrayLengths 含 path、length、returnedLength、truncated；复杂连接/默认值无法解析时明确 valuesAvailable=false、value=null，不代表运行时是空或零。

已实测（scratch-8d，BP_Test 5.8.4 Mac，编辑器世界）：新建临时 NiagaraActor，推进 120×0.0333 秒，每次均 31 粒子。FloatData 三项 Length→Z=1003；不改图仅改五项→1005；[10,20,30] 的 Get(1)→1020，保存/用户重启后 get 和 Z=1020 均保持。新建用户 DiArr 四项、绑定 HlslDiTest.Arr→get 显示 linked/User.DiArr，Length→1004；ArrayFloat3 两个向量 Length→1002；临时线性样条长度300，SoftSourceActor 路径赋值后 GetSplineLength→1300。错误类、InternalFloatData、非 DI 输入、未知属性和错误值类型均拒绝，完整 get 读回不变。临时 Actor 全删，未保存关卡，日志增量无 Ensure/Fatal/越界。最终 HLSL 恢复为 User.DiArr 的 Length（Z=1004），样条软引用已清空。用户已确认系统编辑器与模块图表能打开，编辑器仍可响应；但打开后新增第20项相同的 Duplicate child Distance Distribution Mode ensure，随后 CrashReportClientEditor 在含 FScheduler::StopWorkers 的故障线程中崩溃。数值测试阶段无新增错误不能推广为 UI 打开无错误；本轮未修改引擎或报告隐私设置，详见第23项。
写入失败会回滚并在回复中报告 rolledBack 与 residualChanges；已知限制：撤销后包为脏且有重做记录时再发生同步失败，编译 DI 条目数可能无法恢复（rolledBack=false），恢复办法未实测，先用 get 读回再决定。事务与回滚以 scratch-10 实测，详见 UPSTREAM.md 第 23 项。

未验证：PIE/Windows/GPU、局部空间/远原点 LWC、超过256项的截断实测、其他 DI、ArrayFloat3 各分量调用、多发射器/多脚本内容同步压力、复杂绑定/共享动态输入清理、用户参数及 Spline/ArrayFloat3 的独立重启持久化、持久 Actor 软引用的重启解析。完整记录见 UPSTREAM 第23项。（Scratch 模块和粒子读取已在 Mac 上实测过 PIE、GPU Scratch、本地空间和远原点 LWC，见 UPSTREAM 第 25 项；上面列的是 DI 赋值本身在这些场景下尚未单独验证。）

## 删除未引用 Niagara 用户参数（UE 5.8）

先 search「remove niagara user parameter」并 describe，使用 `remove_niagara_user_parameter` 的 consentGrant 执行。只接收 `systemPath` 与 `name`（`X` 或 `User.X`），先关闭该系统的资产编辑器。预检返回 PARAMETER_IN_USE 时读 references[] 并在编辑器里手动处理；REFERENCE_SCAN_INCOMPLETE 表示源不可检查，禁止当成没有引用。没有 force 或改名能力：原生 ViewModel 引用同步接口未导出，不能靠直接改 store 安全替代。需要改名时先加新参数，手动迁移引用，再删旧参数。

发射器开关：`edit_niagara_system edit=set_emitter_properties` 的 `emitterProperties` 接受 `enabled`、`localSpace`、`determinism`（布尔）和 `moduleEnabled`（{模块名: 布尔}），会重编译；其他属性用 set_parameter_value 或 inspect.set_property。`create_effect kind=niagara_emitter` 会给空的精灵材质槽填默认精灵材质。截图看不到粒子时，先用 control_editor configure_editor setting=window unthrottle 再开视口 realtime。GPU 发射器的粒子数据读不到（NOT_SUPPORTED），只能截图核对。

成功删除为一个撤销步骤，请求编译并标脏，始终 saved=false。检查 compileOk、warnings、diEntriesBefore/After、diEntriesConsistent；原 DI 数据的撤销恢复和编译状态须读回验证。编译中的内联 DI 与用户 DI 是不同集合，不能用两个总数相等作为一致性判据。扫描为保守的本系统/递归脚本图与绑定检查，不是工程范围、外部 HLSL include 或宏的语义引用分析，详见 UPSTREAM 24。仅在用户授权时单独保存目标系统。

## Niagara 逐帧验证与仿真阶段（UE 5.8）

- 看 GPU 发射器的数值：`set_niagara_sim_target target=CPU`（建议在测试副本上），然后用 `step_niagara_simulation reset=true` 让放置的组件重新开始（切换并重编译后旧实例失效，直接读会 SYSTEM_NOT_ACTIVE），再用 `get_niagara_particle_data` 不带 advanceTicks 读。看完 `target=GPU` 切回。CPU 上用了 GPU 专用函数的仿真阶段或 HLSL 会编译失败，看回执 compiledScripts。
- 逐帧：`step_niagara_simulation frames=1` 推进后组件保持暂停，两次调用之间编辑器不会推进它；`frames=0` 只改暂停状态或截图，`pause=false` 让它继续跑。`screenshot` 传 true 或 control_editor 截图参数，同一回执返回暂停帧的图。GPU 发射器的 numParticles 是回读计数，暂停时可能为 0。截图前先 `configure_editor setting=window unthrottle`。
- 仿真阶段：`add_niagara_module moduleKind=simulation_stage stageName=X` 新建，`create_niagara_scratch_module stage=X` 把 Scratch 模块放进去，`set_niagara_simulation_stage` 设 numIterations / enabled。`get_niagara_info` 的模块带 stage、simulationStage、stageIndex，发射器带 simulationStages，渲染器带 facingMode、alignment、锁定轴和 bindings（这些字段在回执 details 里）。
- GPU 粒子：`get_niagara_particle_data` 对 GPU 发射器经 sim cache 读回（同步，回执 readbackMs），暂停帧也可读；Bool 属性读不回，改写成 Int。
- 发射器互读：Scratch 输入类型 `NiagaraDataInterfaceParticleRead`，`set_niagara_data_interface properties {"EmitterBinding":{"BindingMode":"Other","EmitterName":"<另一个发射器>"}}`（自读用 Self）。VM 的 HLSL 里 out 参数变量先声明再传，不要 `int X = 0;` 后再传。
- 邻域搜索：`setup_niagara_neighbor_grid`（仅 GPU 发射器）一次建网格、两个阶段和模块，输出 Particles.NeighborCount / Particles.Separation；再自己把 Separation 加到速度上，用 set_niagara_simulation_stage 调 Query 阶段迭代次数。单格上限 maxNeighborsPerCell，超出的粒子不进网格。
- Data Channel：`create_niagara_data_channel` 建资产，蓝图用 Write To Niagara Data Channel 写入可用；发射器里用 DataChannelRead 读取目前实测读到 0（UPSTREAM 第 27 项），命中点暂用用户参数数组传入。
