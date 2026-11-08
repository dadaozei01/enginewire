<#
Install a built EngineWire package into one UE 5.8 project (or the engine) on Windows, with a backup.

  # project-level (recommended)
  powershell -ExecutionPolicy Bypass -File scripts\install-enginewire.ps1 -Package <package dir> -Project "D:\UE\MyGame\MyGame.uproject"
  # engine-level
  powershell -ExecutionPolicy Bypass -File scripts\install-enginewire.ps1 -Package <package dir> -Engine "C:\Program Files\Epic Games\UE_5.8"
  # report only
  ... -WhatIf

The editor must be closed: this script never closes or starts it. The old plugin folder is copied to
%USERPROFILE%\Builds\EngineWire-backup-<time> before anything changes. Only one install location should exist;
the script refuses when the other location also has EngineWire (pass -AllowDuplicate to override knowingly).
Messages are English on purpose (Windows PowerShell 5.1 and non-ASCII script text).
Status: written on a Mac on 2026-11-08 and not yet run on Windows. Fix and commit to a branch if it misbehaves.
#>
param(
    [Parameter(Mandatory = $true)][string]$Package,
    [string]$Project = '',
    [string]$Engine = '',
    [switch]$AllowDuplicate,
    [switch]$WhatIf
)
$ErrorActionPreference = 'Stop'
function Stop-Install([string]$Message) { Write-Host "STOP: $Message" -ForegroundColor Red; exit 1 }

# 1. The package must be a Win64 EngineWire build.
$Package = (Resolve-Path $Package).Path
if (-not (Test-Path (Join-Path $Package 'EngineWire.uplugin'))) { Stop-Install "no EngineWire.uplugin directly under $Package (nested one level too deep?)" }
$pkgDll = Get-ChildItem (Join-Path $Package 'Binaries\Win64') -Filter 'UnrealEditor-EngineWire*.dll' -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $pkgDll) { Stop-Install "no Win64 EngineWire DLL in $Package\Binaries\Win64 (is this a Mac package?)" }
$pkgVer = (Get-Content (Join-Path $Package 'EngineWire.uplugin') -Raw | ConvertFrom-Json).EngineVersion
Write-Host "Package: $Package (EngineVersion $pkgVer)"

# 2. Exactly one target.
if (($Project -and $Engine) -or (-not $Project -and -not $Engine)) { Stop-Install 'pass exactly one of -Project <.uproject> or -Engine <UE folder>.' }
if ($Project) {
    if (-not (Test-Path $Project) -or $Project -notlike '*.uproject') { Stop-Install "not a .uproject: $Project" }
    $Target = Join-Path (Split-Path -Parent (Resolve-Path $Project).Path) 'Plugins\EngineWire'
    $assoc = (Get-Content $Project -Raw | ConvertFrom-Json).EngineAssociation
    Write-Host "Project: $Project (EngineAssociation $assoc)"
} else {
    if (-not (Test-Path (Join-Path $Engine 'Engine\Build\Build.version'))) { Stop-Install "not a UE folder: $Engine" }
    $Target = Join-Path $Engine 'Engine\Plugins\Marketplace\EngineWire'
}
Write-Host "Target:  $Target"

# 3. The editor must be closed (a loaded DLL cannot be replaced reliably).
$editors = Get-CimInstance Win32_Process -Filter "Name LIKE 'UnrealEditor%'" -ErrorAction SilentlyContinue
if ($editors) {
    $editors | ForEach-Object { Write-Host "Running: PID $($_.ProcessId) $($_.CommandLine)" }
    Stop-Install 'close every Unreal Editor first (ask the user), then run this again.'
}

# 4. Warn about a second install location for the same project or engine.
$others = @()
if ($Project) {
    $engineGuess = @("$env:ProgramFiles\Epic Games\UE_5.8\Engine\Plugins\Marketplace\EngineWire")
    $others = $engineGuess | Where-Object { Test-Path $_ }
} else {
    Write-Host 'Engine-level install: projects that also carry Plugins\EngineWire will load that copy instead. Check them.'
}
if ($others -and -not $AllowDuplicate) { Stop-Install "EngineWire is also installed at: $($others -join ', '). Keep one location, or pass -AllowDuplicate." }

if ($WhatIf) { Write-Host 'WhatIf: checks passed, nothing changed.'; exit 0 }

# 5. Back up, then mirror the package into place.
if (Test-Path $Target) {
    $Backup = Join-Path $env:USERPROFILE ("Builds\EngineWire-backup-" + (Get-Date -Format 'yyyyMMdd-HHmmss'))
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Backup) | Out-Null
    & robocopy $Target $Backup /E /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { Stop-Install "backup failed (robocopy exit $LASTEXITCODE). Nothing changed." }
    Write-Host "Backup:  $Backup"
} else {
    New-Item -ItemType Directory -Force -Path $Target | Out-Null
}
& robocopy $Package $Target /MIR /XD Saved /NFL /NDL /NJH /NJS /NP | Out-Null
if ($LASTEXITCODE -ge 8) { Stop-Install "copy failed (robocopy exit $LASTEXITCODE). Restore from the backup above." }
Get-ChildItem $Target -Recurse -Include *.dll, *.exe -ErrorAction SilentlyContinue | Unblock-File -ErrorAction SilentlyContinue

# 6. Verify.
$installed = Join-Path $Target ("Binaries\Win64\" + $pkgDll.Name)
if ((Get-FileHash $installed).Hash -ne (Get-FileHash $pkgDll.FullName).Hash) { Stop-Install "installed DLL differs from the package: $installed" }
if (Test-Path (Join-Path $Target 'EngineWire\EngineWire.uplugin')) { Stop-Install "nested folder detected: $Target\EngineWire" }
Write-Host 'Installed and verified (DLL hash matches the package).' -ForegroundColor Green
Write-Host 'Next: the user opens the project; check the status shows MCP :3000, then reconnect the MCP client.'
