# 下次大更新待办

2026-11-08 记录，留到下次大更新时一起处理，现在不改代码。各项的现状都对照当日的 `main`（UE 5.8）和 `ue54` 分支核对过。

**优先级**：以 UE 5.8（`main`）为主。今后很少再用 5.4，只涉及 `ue54` 的项目标为低优先级，用到 5.4 时再处理。

## 上游状态与升级决定

- 上游：[ChiR24/Unreal_mcp](https://github.com/ChiR24/Unreal_mcp)。本仓库固定在 v0.6.0（2026-10-27 正式版）。
- 截至 2026-11-08，上游已有 v0.6.1-beta-a（11-06，164 个提交，改动 767 个文件）和 v0.6.1-beta-b（11-08），均为预发布。
- 决定：**暂不升级**。等 v0.6.1 出正式版，再照升 v0.6.0 时的办法，逐条核对 [UPSTREAM.md](../UPSTREAM.md) 里的修改。
- 升级时要移植的本仓库自有功能（上游没有）：Chaos Geometry Collection 破碎（第 16 项），Niagara Scratch / Custom HLSL / 数据接口 / 粒子快照 / 用户参数 / LocalSpace（第 19–25 项），Niagara 模拟控制（CPU/GPU 切换、逐帧锁定、仿真阶段，第 26 项；改了 `EngineWireSubsystem.h` 的 friend 和截图回执的 `_mcpCarry`）。上游复制发射器时，如果发射器用到系统级 Scratch 模块，会直接报 `UNSUPPORTED_EMITTER` 拒绝复制（`McpNiagaraEmitterCopy.cpp`）。
- 上游 beta-b 仍未修复、升级后需要重新打的补丁：中文分类名读写（第 8 项，上游没有 EditorCategoryUtils 换回英文的逻辑）；Windows 上 StateTree 的 LNK1194（上游仍直接引用 `OnRequestCompile`）；UE 5.4 `.uplugin` 里对 PropertyBindingUtils 的可选引用（上游仍有），仅影响 5.4，低优先级。第 7 项传输层关闭竞态：上游加了 `WaitForCompletion`，但仍保留 `Kill(true)`，要逐行核对。第 9–15 项和上游的 graph-as-code 修复（下划线 id 等）可能有重叠，要逐条对比。
- 上游 v0.6.1 的迁移注意点：Movie Render Queue、Takes、MetaSound 改为必需插件；写入的返回里只带 lint 错误，不再带警告；超过 1023 字符的字符串、个数不对的向量或颜色数组会被拒绝；`add_keyframe` 必须带 `property`；没有 `write` 权限的作用域令牌，不能再执行带 `path` 的 `screenshot`、`export_level` 等。

## 本仓库待修

1. **多个编辑器同时监听 3000 端口**：`McpNativeTransportRunLoop.cpp:85` 和 `McpBridgeWebSocketServer.cpp:44` 调用 `SetReuseAddr(true)`，在 Mac 上会同时设置 `SO_REUSEPORT`（第 18 项）。同一工程开两个编辑器时，令牌相同，请求可能进到另一个编辑器。helper 和 `mcp-smoke.py` 会拦，其他客户端不会。修复方向：不设 `SO_REUSEPORT`，端口被占用时启动失败并写日志。Windows 上的行为也要验证。
2. **（低优先级，仅 5.4）GC 缩略图渲染断言的修复没进 5.4**：`NumTransforms == Transforms.Num()` 这个断言的修复在 `main`（`McpAnimGeometryCollectionSupport.cpp`），`ue54` 没有。5.4 存在同样风险，目前是从源码推断的，尚未实测。
3. **截图目录写死**：`McpUiScreenshot.cpp` 默认写到 `Saved/Screenshots/WindowsEditor`，Mac 上也是这个路径。应改为按平台取目录。
4. **蓝图文本的已知限制**（上游也有）：转接点（Reroute）写回后不保留；非纯的 SphereTraceForObjects 被内联进 BreakHitResult，Branch 的 else 接 DoOnce.Reset 在文本中没有体现，写回时 `MakeArray("ObjectTypeQuery6")` 连不上 ObjectTypes，返回 CODE_INVALID。
5. **UPSTREAM.md 第 17 项过时**：其中写的"源码在 `build/ue58`，未合并到 `main`"已不符合现状，`main` 现在就是 5.8 主线。

## 可单独搬过来的上游修复（不必整体升级）

改动都很小，按上游 v0.6.1-beta-a/b 的发布说明，找到对应提交后摘取：

- 没有 Play 会话时模拟按键会让编辑器崩溃（上游 issue #685）。
- 模拟的游戏按键会漏进编辑器面板（#686）。
- 超长的 tag、名字或 key 会让编辑器崩溃（beta-b）。
- 向量和颜色数组个数不对时被补零或截断，却仍返回成功（beta-b）。
- 新建的 Niagara 发射器不保留传入的名字（#669）。

## 尚未验证

- Niagara Scratch 系列（第 19–25 项）只在 Mac UE 5.8 上测过；Windows 还没编译，5.4 没有这些功能。
- 编辑器长时间运行后退出的稳定性，Windows 上没有专门观察过。
- 插件装在引擎目录时的情况；Fracture 和 PlanarCut 真正缺失时能否启动。
- Niagara 模拟控制（第 26 项）只在 Mac UE 5.8 编译实测；PIE 中逐帧、局部空间、多组件 componentName 和 Windows 编译未验证。
