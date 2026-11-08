<#
Build EngineWire for UE 5.8 on Windows x64. Run from a checkout of main (UE 5.8).

  powershell -ExecutionPolicy Bypass -File scripts\build-ue58-win.ps1 -CheckOnly
  powershell -ExecutionPolicy Bypass -File scripts\build-ue58-win.ps1
  powershell -ExecutionPolicy Bypass -File scripts\build-ue58-win.ps1 -Zip -Version v0.7.0-ue58

Options:
  -Engine <dir>    UE 5.8 root (the folder holding Engine\). Found from the registry or common paths when omitted.
  -Out <dir>       New package directory. Default: %USERPROFILE%\Builds\EngineWire-UE<ver>-Win64-<rev>-<time>.
  -CheckOnly       Report engine, descriptor and toolchain; build nothing.
  -Zip             After a good build, write EngineWire-<Version>-UE<ver>-Win64.zip and SHA256SUMS-Win64.txt next to -Out.
  -Version <tag>   Name used in the zip (default: dev-<rev>).

Never edits the engine, Visual Studio, the Windows SDK or an installed plugin. Installing is install-enginewire.ps1.
Messages are English on purpose: Windows PowerShell 5.1 misreads non-ASCII text in a script file without a BOM.
Status: written on a Mac on 2026-11-08 and not yet run on Windows. Fix and commit to a branch if it misbehaves.
#>
param(
    [string]$Engine = '',
    [string]$Out = '',
    [switch]$CheckOnly,
    [switch]$Zip,
    [string]$Version = ''
)
$ErrorActionPreference = 'Stop'
function Stop-Build([string]$Message) { Write-Host "STOP: $Message" -ForegroundColor Red; exit 1 }

$Repo = Split-Path -Parent $PSScriptRoot
$Plugin = Join-Path $Repo 'plugins\EngineWire\EngineWire.uplugin'
if (-not (Test-Path $Plugin)) { Stop-Build "plugin descriptor not found: $Plugin" }

# 1. Find the engine.
function Find-Engine {
    foreach ($key in 'HKLM:\SOFTWARE\EpicGames\Unreal Engine\5.8', 'HKLM:\SOFTWARE\WOW6432Node\EpicGames\Unreal Engine\5.8') {
        $item = Get-ItemProperty -Path $key -Name InstalledDirectory -ErrorAction SilentlyContinue
        if ($item -and (Test-Path $item.InstalledDirectory)) { return $item.InstalledDirectory }
    }
    foreach ($candidate in "$env:ProgramFiles\Epic Games\UE_5.8", 'D:\Epic Games\UE_5.8', 'D:\UE_5.8', 'E:\Epic Games\UE_5.8') {
        if (Test-Path (Join-Path $candidate 'Engine\Build\Build.version')) { return $candidate }
    }
    return ''
}
if (-not $Engine) { $Engine = Find-Engine }
if (-not $Engine) { Stop-Build 'UE 5.8 not found. Pass -Engine "<UE_5.8 folder>".' }
$VersionFile = Join-Path $Engine 'Engine\Build\Build.version'
$RunUAT = Join-Path $Engine 'Engine\Build\BatchFiles\RunUAT.bat'
if (-not (Test-Path $VersionFile) -or -not (Test-Path $RunUAT)) { Stop-Build "engine is incomplete: $Engine" }

$v = Get-Content $VersionFile -Raw | ConvertFrom-Json
$EngineVer = "$($v.MajorVersion).$($v.MinorVersion).$($v.PatchVersion)"
$PluginVer = (Get-Content $Plugin -Raw | ConvertFrom-Json).EngineVersion

# 2. The descriptor must target this engine (main is 5.8; the ue54 branch is 5.4).
$engineMinor = ($EngineVer -split '\.')[0..1] -join '.'
$pluginMinor = ($PluginVer -split '\.')[0..1] -join '.'
if ($engineMinor -ne $pluginMinor) { Stop-Build "descriptor targets UE $PluginVer but the engine is UE $EngineVer. Use the matching branch." }

$Rev = (git -C $Repo rev-parse --short HEAD).Trim()
$Branch = (git -C $Repo rev-parse --abbrev-ref HEAD).Trim()
$Dirty = @(git -C $Repo status --porcelain -- plugins).Count
Write-Host "Engine:  UE $EngineVer (compatible changelist $($v.CompatibleChangelist)) at $Engine"
Write-Host "Source:  $Branch @ $Rev, $Dirty changed plugin files"
Write-Host "Verified on Windows before: UE 5.8.4, MSVC 14.44.35229, Windows SDK 10.0.22621.0"
if ($EngineVer -ne '5.8.4') { Write-Host "NOTE: this engine differs from the 5.8.4 the packages were tested on." -ForegroundColor Yellow }

# 3. Toolchain report. Reports only; UBT picks the toolchain and says so in the log.
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (Test-Path $vswhere) {
    $vsPath = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1)
    if ($vsPath) {
        Write-Host "Visual Studio: $vsPath"
        $msvc = Get-ChildItem (Join-Path $vsPath 'VC\Tools\MSVC') -Directory -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Name
        Write-Host "MSVC toolsets: $($msvc -join ', ')"
    } else { Write-Host 'WARNING: no Visual Studio with the C++ x64 tools found.' -ForegroundColor Yellow }
} else { Write-Host 'WARNING: vswhere.exe not found; is Visual Studio installed?' -ForegroundColor Yellow }
$sdkRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\Include'
if (Test-Path $sdkRoot) { Write-Host "Windows SDKs: $((Get-ChildItem $sdkRoot -Directory | Select-Object -ExpandProperty Name) -join ', ')" }

if ($CheckOnly) { Write-Host ''; Write-Host 'Checks passed. Nothing was built.'; exit 0 }

# 4. Build into a new directory, keep the full log.
$Stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$BuildsRoot = Join-Path $env:USERPROFILE 'Builds'
if (-not $Out) { $Out = Join-Path $BuildsRoot "EngineWire-UE$EngineVer-Win64-$Rev-$Stamp" }
if (Test-Path $Out) { Stop-Build "output already exists; each build needs a new directory: $Out" }
$LogDir = Join-Path $BuildsRoot 'logs'
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
$Log = Join-Path $LogDir "ue58-win-buildplugin-$Stamp.log"
Write-Host ''; Write-Host '== BuildPlugin =='; Write-Host "Output: $Out"; Write-Host "Log:    $Log"
& cmd /c "`"$RunUAT`" BuildPlugin `"-Plugin=$Plugin`" `"-Package=$Out`" -TargetPlatforms=Win64 > `"$Log`" 2>&1"
$rc = $LASTEXITCODE
$text = Get-Content $Log

# Our own diagnostics: compiler errors and warnings in EngineWire sources, minus engine deprecation noise (C4996).
$ours = $text | Where-Object { $_ -match 'EngineWire' -and $_ -match '(error|warning) (C\d+|LNK\d+)' -and $_ -notmatch 'C4996' } | Sort-Object -Unique
if ($rc -ne 0) {
    Write-Host ''; Write-Host "== Build failed (exit $rc). Distinct errors: ==" -ForegroundColor Red
    $text | Where-Object { $_ -match ': (fatal )?error (C\d+|LNK\d+)|error MSB|LINK : fatal' } | ForEach-Object { $_.Trim() } | Sort-Object -Unique | Select-Object -First 60
    $niagara = $ours | Where-Object { $_ -match 'NiagaraAuthoring' }
    if ($niagara) { Write-Host ''; Write-Host "Of these, $(@($niagara).Count) lines are in Domains\NiagaraAuthoring (first MSVC compile of UPSTREAM 19-25)." }
    Write-Host "Full log: $Log"
    exit $rc
}

# 5. Inspect the result.
Write-Host ''; Write-Host '== Result =='
$dlls = Get-ChildItem (Join-Path $Out 'Binaries\Win64') -Filter '*.dll' -ErrorAction SilentlyContinue
if (-not $dlls) { Stop-Build "build reported success but produced no DLL under $Out\Binaries\Win64" }
$dlls | ForEach-Object { Write-Host "$($_.Name)  $([math]::Round($_.Length / 1MB, 1)) MB" }
$modules = Get-ChildItem (Join-Path $Out 'Binaries\Win64') -Filter '*.modules' | Select-Object -First 1
if ($modules) { Write-Host "BuildId: $((Get-Content $modules.FullName -Raw | ConvertFrom-Json).BuildId)" }
if ($ours) { Write-Host ''; Write-Host "EngineWire warnings (C4996 excluded): $(@($ours).Count)"; $ours | Select-Object -First 30 }
else { Write-Host 'EngineWire warnings (C4996 excluded): 0' }
Write-Host "Package ready: $Out"

# 6. Optional release zip. Same layout as the Mac zip: the zip root is EngineWire\. No PDB, no Intermediate.
if ($Zip) {
    if (-not $Version) { $Version = "dev-$Rev" }
    $ReleaseDir = Join-Path $BuildsRoot "release-$Version"
    $Stage = Join-Path $ReleaseDir 'stage\EngineWire'
    if (Test-Path $Stage) { Stop-Build "staging folder already exists: $Stage" }
    New-Item -ItemType Directory -Force -Path $Stage | Out-Null
    & robocopy $Out $Stage /E /XD Intermediate Saved /XF *.pdb /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { Stop-Build "robocopy failed staging the zip (exit $LASTEXITCODE)" }
    $ZipName = "EngineWire-$Version-UE$EngineVer-Win64.zip"
    $ZipPath = Join-Path $ReleaseDir $ZipName
    Compress-Archive -Path $Stage -DestinationPath $ZipPath -CompressionLevel Optimal
    $hash = (Get-FileHash $ZipPath -Algorithm SHA256).Hash.ToLower()
    "$hash  $ZipName" | Set-Content -Encoding ascii (Join-Path $ReleaseDir 'SHA256SUMS-Win64.txt')
    Write-Host "Zip: $ZipPath"; Write-Host "SHA256: $hash"
}
