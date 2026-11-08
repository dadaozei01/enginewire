# Agent 正常使用 MCP 插件

统一规则：优先已验证的 EngineWire MCP；成熟批量任务可用编辑器内 UE Python；独立 Remote Execution 备用。MCP 提供 Python 执行能力时可沿现有连接执行，不必另开网络桥；不安装或依赖任务按钮。

跨 Agent 共享操作规范见 [ue-ai-bridge-protocol](https://github.com/dadaozei01/ue-ai-bridge-protocol/blob/main/docs/agent-entry.md)，本页负责 EngineWire 的具体调用方式。

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

## 故障定位

## Chaos 破碎（Geometry Collection）

本仓库新增（UPSTREAM.md 第 16 项），在 animation_physics 下，search「geometry collection」「voronoi fracture」「破碎」可找到。只在 UE 5.4 构建中提供，其他版本返回 NOT_SUPPORTED；切割与分组需工程启用 Fracture、PlanarCut 插件。

- create_geometry_collection：Fracture Mode 的 New。staticMeshPaths 一个或多个，name 默认 GC_<网格名>，path 默认网格所在目录；同名资产已存在时拒绝，不覆盖。spawnActor 可同时在关卡放 Actor（location、rotation、actorLabel、simulatePhysics）。
- fracture_geometry_collection：Uniform Voronoi（siteCount，可选 siteCountMax）或 Custom Voronoi（sites，资产空间坐标）；randomSeed（-1 随机并在回执返回）、chanceToFracture、groupFracture（默认 true）、grout、noise*、collisionSampleSpacing、boneIndices。已切过的 GC 默认拒绝（ALREADY_FRACTURED），预估碎块超过 maxPieces（默认 2000）拒绝（TOO_MANY_PIECES），都在切之前判断。要重切就新建一个 GC，不要对已切的再切。
- cluster_geometry_collection：Auto Cluster，clusterSizeMethod by_number / by_fraction / by_size / by_grid，默认对根节点分组。
- get_geometry_collection_info：只读，碎块数、簇数、层级与每层数量、包围盒、三角形数和物理设置。
- configure_geometry_collection：已放进关卡的 Actor，其组件在放置时拷贝了一份资产的伤害阈值、伤害模型和物理材质，之后改资产不会影响它（引擎行为）。用 actorName 指定时会同步该 Actor，其他 Actor 用 applyToActors 点名，没点名的列在 placedActorsNotUpdated 并提示。可设置：伤害阈值（每层一个）、damageModel、enableClustering、第一个尺寸档的碰撞类型与形状、Convex Overlap Removal 与凸包参数、质量、物理材质、Nanite。走细节面板同一条重建路径，先校验全部参数再写入。
- 写操作各为一个撤销步骤，之后保存资产；Actor 上组件级设置（碰撞配置、Simulate Physics）仍用 control_actor。Field 没有专门命令，用通用命令即可（Mac 已实测锚定生效）：control_actor spawn（spawnKind blueprint）放引擎自带的 /Engine/EditorResources/FieldNodes/ 下的场，如 FS_AnchorField_Generic（盒子缩放 1 时为 100 cm，用 Actor 缩放调大小）；control_actor edit_component 的 set_property 把 GC 组件的 InitializationFields 设为场 Actor 的对象路径数组；场的参数用 set_blueprint_variables。从上往下随时间释放需要运行时逻辑（蓝图 Timeline 或 Sequencer 移动场），尚未搭建。

MCP off：检查开关、重启及 UE 日志。401/403：检查工程身份、动态令牌和请求头。连接拒绝：确认端口和运行进程。工具不存在：重载 Agent 配置，确认使用匹配的 0.6 客户端/代理。不要输出令牌排错。
