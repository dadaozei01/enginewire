# 使用约定

读取 docs/agent-guide.md 后再操作 UE。不要上传用户工程、测试资产、截图、Saved、令牌或带私有路径的日志。新增插件兼容修改应保留 MIT 与上游来源。

优先已验证 EngineWire MCP，成熟批量任务可用编辑器内 UE Python，Remote Execution 备用；不安装按钮或为已有 MCP 强制开启 Python 远程。共享规范见 https://github.com/dadaozei01/ue-ai-bridge-protocol/blob/main/docs/agent-entry.md 。

合理使用批量操作与必要的验证，不强制省流量或额度。Mac 已编译并验证加载、只读 MCP 与部分写入（蓝图创建与复制、组件、批量写图表、删节点与函数、生成 Actor、编辑器内 Python、PIE 与截图，见 UPSTREAM.md），其他写入首次使用后检查结果；不能把 Windows 文件标为 Mac 构建。

蓝图排版按 [Agent 手册](docs/agent-guide.md#蓝图排版与验收) 执行。新增节点和连线同批提交。局部 `arrange_nodes` 用未选节点作固定锚点，批量插入才允许旧节点局部让位。有视觉异常时，按正常编辑缩放复核节点边缘、进线净空和白线。
