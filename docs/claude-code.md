# Claude Code 配置

Claude Code（命令行与桌面版 Code 标签共用同一份配置）可直接连接 Native MCP，不需要上游 Node 代理。本仓库的 scripts/mcp-headers.cjs 通过 headersHelper 在每次连接时从工程读取令牌，配置里不保存令牌副本。需要 Node 18+。

## 前提

1. UE 已启用 EngineWire 和 Native MCP Server 并重启，状态栏显示 MCP :3000。
2. 目标工程存在 Saved/MCP/capability-token。
3. 把 scripts/mcp-headers.cjs 放到固定位置（仓库克隆目录或单独复制一份），不要放在临时目录。

## 添加

推荐不写工程路径，helper 自动连接当前打开的工程：先找监听 MCP 端口（默认 3000）的进程，确认是 UnrealEditor，再从它的启动命令行取 .uproject，读取该工程的令牌。打开哪个工程就连哪个，换工程不用改配置。下面的 helper 路径换成实际路径。

Windows PowerShell：

```powershell
$helper = 'D:/tools/enginewire/scripts/mcp-headers.cjs'
$cfg = [ordered]@{ type = 'http'; url = 'http://127.0.0.1:3000/mcp'; headersHelper = "node `"$helper`"" } | ConvertTo-Json -Compress
claude mcp add-json unreal $cfg --scope user
```

PowerShell 手写 JSON 引号容易出错，用 ConvertTo-Json 生成更稳妥。

Mac 终端：

```sh
claude mcp add-json unreal '{"type":"http","url":"http://127.0.0.1:3000/mcp","headersHelper":"node \"/Users/you/tools/enginewire/scripts/mcp-headers.cjs\""}' --scope user
```

若 Claude Code 找不到 node，把 node 换成 which node 输出的绝对路径。没有 Node 时用同目录的 scripts/mcp-headers.py，macOS 自带的 /usr/bin/python3 即可：

```sh
claude mcp add-json unreal '{"type":"http","url":"http://127.0.0.1:3000/mcp","headersHelper":"/usr/bin/python3 \"/Users/you/tools/enginewire/scripts/mcp-headers.py\""}' --scope user
```

### 固定工程

要固定连某一个工程，在 helper 后加 .uproject 或工程目录作为参数（路径含中文或空格时保留引号），或设置 UE_PROJECT_PATH 环境变量。此时打开别的工程会因令牌不符返回 401。

```powershell
$cfg = [ordered]@{ type = 'http'; url = 'http://127.0.0.1:3000/mcp'; headersHelper = "node `"$helper`" `"D:/UE/MyProject/MyProject.uproject`"" } | ConvertTo-Json -Compress
```

### 自动识别的限制

- Windows：命令行带 .uproject 时读该工程的 `Saved\MCP`。编辑器不带工程启动、自己打开上次的工程时（日志里是 Loading recent project file），Saved 目录和令牌在 `%LOCALAPPDATA%\UnrealEngine\<主版本.次版本>\Saved\MCP`，helper 按编辑器的 Build.version 改读这里；Node 与 Python helper 均已在 UE 5.8.4 实测。只开项目浏览器、尚未选工程时不行。
- Mac：在 Finder 双击 .uproject 时命令行不带工程路径，helper 改从编辑器打开的文件反推工程，所以工程要有自己的模块或工程目录下的插件。纯蓝图工程配引擎目录插件时可能认不出，此时在 helper 后面写上 .uproject 路径。
- 同时开多个编辑器时不要依赖自动识别。Mac（UE 5.8.4 + 5.4.4）实测两个编辑器都能监听 3000，所有连接都进后打开的那个；令牌不匹配时返回 401。helper 发现多个监听者会直接报错。Windows 的行为未重新验证。
- Claude Code 在会话开始或 /mcp 重连时调用 helper。换工程后要新开会话或执行 /mcp 重连，才会读到新工程的令牌。
- Windows 自动识别已实测（含中文路径）。Mac（UE 5.8.4）已实测 Finder 双击打开、工程带 C++ 模块的情况，Python 和 Node helper 都读到正确令牌；命令行打开时命令行带工程路径，这条路径未单独运行 helper。

## 作用域

- --scope user：所有目录可用；配合自动识别，切换 UE 工程不用改配置。
- --scope project：写入当前目录的 .mcp.json，适合每个 UE 工程各自固定配置。该文件含本机绝对路径，不要提交到公共仓库。
- 已用固定工程配置的，换工程时需要改 headersHelper 中的工程路径（先 claude mcp remove unreal -s user 再重新添加），或改成上面的自动识别写法。

## 作用域令牌

Saved/MCP/capability-token 是旧式全权限（Admin）令牌，插件日志会提示已弃用。helper 优先读取 Saved/MCP/scoped-capability-token，不存在时才回退旧令牌。作用域令牌只能取 Read、Write、Destructive，不能取 Admin；编辑器内 Python 只需 Write。

1. 关闭 UE。插件只在启动时读取令牌配置。
2. 生成 64 位十六进制令牌，写入 Saved/MCP/scoped-capability-token，权限 600。
3. 备份 Config/DefaultGame.ini，在已有的 [/Script/EngineWire.EngineWireSettings] 段内加一行，Token 换成同一个值：

```ini
+ScopedCapabilityTokens=(Profile="claude-code",Token="<64位十六进制>",Scopes=(Read,Write,Destructive))
```

令牌因此写在工程配置里：分享或提交工程前删掉这一行。不要放在 Saved/Config/<平台>Editor/Game.ini：Mac 上实测编辑器每次退出都会删除该文件；而且 Saved 下的 ini 不处理 + / - 数组前缀。

4. 启动 UE 后重新连接 MCP。日志不再出现 deprecated all-or-nothing capability token 警告即生效。

旧令牌文件仍然有效，插件会在文件缺失时重新生成，不要靠删除它来吊销。

## 验证

```sh
claude mcp get unreal
```

显示 Status: Connected 即连接成功。已打开的 Claude Code 会话需重开或在会话中执行 /mcp 重新连接，之后才会出现 unreal 工具。首先只读查询，例如“列出当前关卡的 Actor 统计”，确认 worldName 是目标工程后再写入。

## 不建议

- claude mcp add --transport http ... --header "X-MCP-Capability-Token: ..."：令牌以明文写入 ~/.claude.json，重新生成令牌后也会失效。
- 在终端直接运行 helper 查看输出：输出含认证头。

## 故障定位

Failed to connect：确认 UE 已启动且 3000 端口在监听，先启动 UE 再连接。401：工程路径指向了别的工程，或令牌已重新生成；自动识别时多为换了工程但没重连 MCP。helper 报 Cannot read MCP capability token 时，冒号后面是原因：Nothing is listening on port 表示 UE 或 Native MCP 未启动；Port owner is not UnrealEditor 表示端口被别的程序占用；Editor command line has no .uproject 表示编辑器未带工程参数启动，且引擎用户目录下也没有 `Saved\MCP`，改用固定工程写法；找不到令牌文件表示工程路径错误或 Native MCP 未启动过。工具调用方式见 agent-guide.md。
