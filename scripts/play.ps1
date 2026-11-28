# Starts the title on Leonida's Vulkan renderer with the settings that work today.
#
#   powershell -ExecutionPolicy Bypass -File scripts\play.ps1 -Game <game folder> -Work <make_game work folder>
#
# Every setting is an environment variable the runtime reads (docs/port_status.md); one already set in the shell wins
# over the default here. Nothing is downloaded and nothing goes online.
param(
    [Parameter(Mandatory = $true)][string]$Game,      # the folder with game_scarlett_master.exe
    [Parameter(Mandatory = $true)][string]$Work,      # make_game.py's --work folder (its shaders\spirv is used)
    [string]$Overlay = "",                            # folder for the title's writable files (default: <Work>\overlay)
    [string]$Saves = "",                              # save folder (default: <Work>\saves)
    [string]$DisplayMode = "2560x1440",
    [string]$Aspect = "",                             # LEONIDA_ASPECT: 21:9, 32:9, 43:18, auto (docs/ultrawide.md)
    [string]$PresentScale = "",                       # LEONIDA_PRESENT_SCALE: fit or stretch (fit with -Aspect)
    [string]$Build = "",                              # the runtime build folder (default: runtime\build-vk\RelWithDebInfo)
    [switch]$Trace                                    # write a startup trace beside the logs
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
if (-not $Build) { $Build = Join-Path $repo "runtime\build-vk\RelWithDebInfo" }
if (-not $Overlay) { $Overlay = Join-Path $Work "overlay" }
if (-not $Saves) { $Saves = Join-Path $Work "saves" }
$exe = Join-Path $Build "gta6rt.exe"
$title = Join-Path $Game "game_scarlett_master.exe"
foreach ($path in @($exe, $title, (Join-Path $Work "shaders\spirv\index.json"))) {
    if (-not (Test-Path $path)) { throw "missing: $path (build the runtime and run make_game.py first)" }
}
New-Item -ItemType Directory -Force $Overlay, $Saves | Out-Null

function Set-Default([string]$Name, [string]$Value) {
    if (-not [Environment]::GetEnvironmentVariable($Name)) { [Environment]::SetEnvironmentVariable($Name, $Value) }
}
Set-Default "LEONIDA_D3D12" "vulkan"
Set-Default "LEONIDA_SHADERS" (Join-Path $Work "shaders\spirv")
Set-Default "LEONIDA_SAVE_DIR" $Saves
Set-Default "LEONIDA_DISPLAY_MODE" $DisplayMode
if ($Aspect) { Set-Default "LEONIDA_ASPECT" $Aspect }
if ($PresentScale) { Set-Default "LEONIDA_PRESENT_SCALE" $PresentScale }
Set-Default "LEONIDA_KEYBOARD_PAD" "1"          # keyboard and mouse as a pad while the window has focus
# The title's soft asserts (an int3 the console build carries on from) are stepped over and logged; the one known
# site is game+0x4ffd879 after the start screen (docs/port_status.md, open items).
Set-Default "LEONIDA_SKIP_INT3" "1"
$tables = Join-Path $repo "victory\crates\victory-formats\src\bcpack-tables.bin"
if (Test-Path $tables) { Set-Default "LEONIDA_BCPACK_TABLES" $tables }

$logs = Join-Path $Work "logs"
New-Item -ItemType Directory -Force $logs | Out-Null
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$runArgs = @("--run", "`"$title`"", "--game-dir", "`"$Game`"", "--overlay", "`"$Overlay`"")
# LEONIDA_SKIP_INT3 needs the trace's exception handler, so a trace is always written; -Trace only says so.
$runArgs += @("--trace", "`"$(Join-Path $logs "trace-$stamp.jsonl")`"")
if ($Trace) { Write-Host "trace: $(Join-Path $logs "trace-$stamp.jsonl")" }
Write-Host "Leonida: $title"
Write-Host "log: $(Join-Path $logs "run-$stamp.log")"
$process = Start-Process -FilePath $exe -ArgumentList $runArgs -WorkingDirectory $Build -NoNewWindow -PassThru `
    -RedirectStandardError (Join-Path $logs "run-$stamp.log") -RedirectStandardOutput (Join-Path $logs "out-$stamp.log")
$process.WaitForExit()
Write-Host ("exit code 0x{0:X8}" -f $process.ExitCode)
