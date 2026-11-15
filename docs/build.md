# 编译

`main` 是 UE 5.8，编译见后面的 UE 5.8 小节。下面 UE 5.4 的步骤只适用于 `ue54` 分支。

## UE 5.4（`ue54` 分支）

Windows 需要 UE 5.4、VS2022 C++ 和匹配 SDK；Mac 需要真实 Mac、UE 5.4 与 UE 支持的 Xcode。Windows 不能直接产出这里需要的 Mac 插件二进制。

先克隆仓库，把下面路径换成实际绝对路径。输出目录必须是新的独立目录。

Windows PowerShell：

```powershell
& 'C:/Program Files/Epic Games/UE_5.4/Engine/Build/BatchFiles/RunUAT.bat' BuildPlugin '-Plugin=D:/tools/enginewire/plugins/EngineWire/EngineWire.uplugin' '-Package=D:/Builds/EngineWire-UE54-Win64' -TargetPlatforms=Win64 -VS2022
```

Mac 终端：

```sh
xcode-select -p
"/Users/Shared/Epic Games/UE_5.4/Engine/Build/BatchFiles/RunUAT.sh" BuildPlugin -Plugin="$HOME/tools/enginewire/plugins/EngineWire/EngineWire.uplugin" -Package="$HOME/Builds/EngineWire-UE54-Mac" -TargetPlatforms=Mac
```

2026-10-23 已在 Apple Silicon、UE 5.4.4、Xcode 27.0 上用上面的命令编译通过。UAT 默认按 arm64+x64 编译，lipo -info 显示两个 dylib 均为 x86_64 arm64。实际加载并验证 MCP 后再打包上传。不要把 Windows DLL 复制为 Mac 产物。

Xcode 版本高于 UE 允许范围时，UBT 报 Platform Mac is not a valid platform to build。用下面的命令查看允许范围：

```sh
"/Users/Shared/Epic Games/UE_5.4/Engine/Build/BatchFiles/RunUAT.sh" Turnkey -command=VerifySdk -platform=Mac -UpdateIfNeeded=false
```

Status=Invalid 且 Current_Sdk 高于 MaxAllowed_Sdk 时，二选一：安装范围内的 Xcode 并用 xcode-select 切换；或备份 Engine/Config/Apple/Apple_SDK.json 后调高 MaxVersion（Xcode 27 用 27.9.0）。后者未经 Epic 验证，属于本机引擎配置，不随本仓库分发。

UE 5.4 构建会带上 Chaos 破碎命令（UPSTREAM.md 第 16 项），需要引擎里的 Fracture 与 PlanarCut 插件（Experimental，引擎自带）；找不到时这些命令编译为 NOT_SUPPORTED，不影响其余部分。修改 MCP/Extensions/*.capabilities.json 后先运行 `python3 scripts/gen-extension-capabilities.py` 再编译。

发布包保留 EngineWire.uplugin、Source、Config、Resources、上游 Content（如存在）、Binaries 下运行所需文件及许可证，排除 Intermediate、Saved、PDB、dSYM 和用户资产。ZIP 根目录应为 EngineWire，生成 SHA-256 校验。

若 UAT 被引擎内无关插件规则阻断，先查日志定位，不删除其他插件；当前 Windows 发布的回退构建情况见 UPSTREAM.md。

## UE 5.8（Mac）

从 `main` 分支运行。`scripts/build-ue58-mac.sh --check-only` 只做检查，不带参数则完成编译并检查产物。脚本不会修改引擎或 Xcode 配置，也不会安装插件。

已验证环境为 Apple Silicon、macOS 27.0.1、UE 5.8.4、Xcode 27.0。UE 5.8.4 自带的 `Apple_SDK.json` 上限为 27.9.0，VerifySdk 为 Valid，不需要改配置。Epic 文档推荐 Xcode 26.1.1，并写明 26.4 不兼容，Xcode 27 不在其推荐范围内。

BuildPlugin 产出 Universal 包。工程目录安装后编译工程，`Plugins/EngineWire` 里的 dylib 会被重新生成，大小约为一半，发布时要用 BuildPlugin 的输出。

从 5.4 复制的 C++ 工程要把 Target 文件改为 `BuildSettingsVersion.V7` 和 `EngineIncludeOrderVersion.Unreal5_8`，否则 UBT 因修改共享编译环境而失败。

## UE 5.8（Windows x64）

推荐用脚本：`powershell -ExecutionPolicy Bypass -File scripts\build-ue58-win.ps1`（`-CheckOnly` 只检查，`-Zip -Version <tag>` 打发布包），安装用 `scripts\install-enginewire.ps1`；完整步骤和 MSVC 报错处理见 [windows-update.md](windows-update.md)。两个脚本已于 2026-11-09 在 Windows 实测（UE 5.8.4，v0.7.0-ue58 即由此打包）。下面是等价的手工命令。

从 `main` 分支的仓库根目录运行。已验证环境为 UE 5.8.4（BuildId 55116800）、Visual Studio C++ 工具链 14.44.35229、Windows SDK 10.0.22621.0。MSVC 安装目录名可以是 `14.44.35207`，这次构建实际使用的工具链版本是 `14.44.35229`，不属于禁用版本。Mac 编译见上一节。

```powershell
$ue58 = '<UE_5.8 安装目录>'
$plugin = Join-Path (Get-Location).Path 'plugins/EngineWire/EngineWire.uplugin'
$package = Join-Path (Get-Location).Path 'build-output/EngineWire-UE58-Win64'
& (Join-Path $ue58 'Engine/Build/BatchFiles/RunUAT.bat') BuildPlugin "-Plugin=$plugin" "-Package=$package" -TargetPlatforms=Win64
```

每次构建使用新的输出目录。`main` 的描述符面向 UE 5.8；UE 5.4 的编译请检出 `ue54` 分支，按本文前面的 5.4 小节操作。
