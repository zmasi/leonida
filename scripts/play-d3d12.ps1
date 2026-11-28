# Starts the title on Leonida's D3D12 runtime (d3d12/, from trustno1-coder/VIctory-NVIDIA) on Windows.
# The Vulkan renderer stays scripts\play.ps1; Linux uses scripts\play-linux.sh (docs/linux.md).
#
#   powershell -ExecutionPolicy Bypass -File scripts\play-d3d12.ps1 -Game <folder with game_scarlett_master.exe>
#
# By default the game runs as on an Xbox Series X, rendering 2560x1440 (render scale 2/3 of its 3840x2160 output)
# with its own temporal upscaler; -SeriesS runs the lighter Series S profile instead, where the game renders its own
# 720p picture and DLSS takes it to 1080p. -Resolution sets the rendered height (Series X: 1080 or 1440). DLSS and DLSS
# frame generation need a few of NVIDIA's own files that no repository may carry: d3d12\Get-NVIDIA-DLSS.cmd fetches
# them from NVIDIA's GitHub once (about 70 MB). Without them the picture is stretched to the screen instead.
# In the game: F1 = settings (frame generation, frame-rate cap, DLSS preset ...), F3 = the numbers panel.
#
# -SmoothMotion  starts leonida_sm.exe, the copy NVIDIA's Smooth Motion profile is set for (scripts\smooth-motion.ps1
#                -State on sets it): the driver shows twice the frames the game renders (RTX 40 and 50). Not the
#                default: the one start made that way on 2026-11-27 ended 2.4 s in with the title's own breakpoint
#                (0x80000003 in its main loop), and the next start without it ran. Not looked into yet.
# -NoDlss        the picture is stretched instead of going through DLSS
# -Dlss          DLSS makes the output, on Series X too: quality (rendered at 1440 lines of the 2160), balanced
#                (1296), performance (1080) or ultra (720). Leave -Resolution out with it.
# -FrameGen      DLSS frame generation: 2 shows twice the frames rendered (up to 6 where the card does more). On
#                Series X it needs the DLSS upscale and turns -Dlss quality on unless a mode is given.
# -Aspect        the window's shape: 21:9, 32:9, 16:9, W:H or auto (the screen's). The game still draws 16:9, shown
#                centred with black bars (-PresentScale fit, the default with -Aspect) or stretched (stretch).
# -Resolution    the older path: the game renders at this height and the title's own upscaler does the rest
# -RayTracing    the title's real ray tracing (slower; shaders build for minutes the first time)
# -Capture       F9 in the game saves that frame and the pictures it drew
# -NoPrecompile  start at once: by default every shader is translated and the pipelines earlier runs drew with are
#                made before the title starts (minutes the first time, seconds after)
# -Log           the runtime's log goes to a file under %LOCALAPPDATA%\Leonida\logs
# -DryRun        prints the settings this start would run with and stops: nothing is built or started
#
# Builds d3d12/ when it is missing (d3d12\scripts\build-nvidia.ps1: the diag profile, with DLSS when NVIDIA's files
# are there), keeps the caches and saves under -Data, and hands everything else to d3d12\scripts\play-nvidia.ps1:
# -SeriesX, -Quality, -Adapter and -More pass through. Nothing goes online.
param(
    [Parameter(Mandatory = $true)][string]$Game,
    [string]$Data = (Join-Path $env:LOCALAPPDATA "Leonida\d3d12"),
    [switch]$SeriesX,
    [switch]$SeriesS,
    [switch]$Quality,
    [ValidateSet('', '2160', '1440', '1080', '720', '540', '480')][string]$Resolution = '',
    [ValidateSet('', 'quality', 'balanced', 'performance', 'ultra')][string]$Dlss = '',
    [ValidateSet(0, 2, 3, 4, 5, 6)][int]$FrameGen = 0,
    [string]$Aspect = '',
    [ValidateSet('', 'fit', 'stretch')][string]$PresentScale = '',
    [string]$Adapter = '',
    [hashtable]$More = @{},
    [switch]$Rebuild,
    [switch]$SmoothMotion,
    [switch]$NoSmoothMotion,
    [switch]$NoDlss,
    [switch]$RayTracing,
    [switch]$Capture,
    [switch]$NoPrecompile,
    [switch]$Log,
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$runtime = Join-Path $repo "d3d12"
$exe = Join-Path $runtime "target\diag\leonida.exe"
$title = Join-Path $Game "game_scarlett_master.exe"
if (-not (Test-Path $title)) { throw "missing: $title" }
if (-not $DryRun -and ($Rebuild -or -not (Test-Path $exe))) {
    & (Join-Path $runtime "scripts\build-nvidia.ps1")
    if ($LASTEXITCODE -ne 0) { throw "cargo build failed ($LASTEXITCODE)" }
}
# A shader cache of its own: one filled before the renderer changed (cache\) does not fit its pipelines.
New-Item -ItemType Directory -Force (Join-Path $Data "cache-2"), (Join-Path $Data "data") | Out-Null
$env:LEONIDA_GAME_DIR = (Resolve-Path $Game).Path
$env:LEONIDA_CACHE_DIR = Join-Path $Data "cache-2"
$env:LEONIDA_DATA_DIR = Join-Path $Data "data"

# Leonida: Smooth Motion only with -SmoothMotion (it was the default for one start, which the title did not
# survive; -NoSmoothMotion is kept and means the same as leaving -SmoothMotion out).
$launch = @{ Exe = $title; Build = 'diag'; NoSmoothMotion = (-not $SmoothMotion); More = $More }
# Leonida: the Series X profile is the default (the user's call: 1080p-1440p as on the bigger consoles).
if (-not $SeriesS) {
    $launch.SeriesX = $true
    # 1440 lines and the title's own upscaler, unless DLSS (which frame generation brings with it) makes the output.
    if (-not $Resolution -and -not $Dlss -and -not $FrameGen) { $Resolution = '1440' }
}
if ($Dlss) { $launch.Dlss = $Dlss }
if ($FrameGen) { $launch.FrameGen = $FrameGen }
if ($Aspect) { $launch.Aspect = $Aspect }
if ($PresentScale) { $launch.PresentScale = $PresentScale }
foreach ($switch in 'Quality', 'NoDlss', 'RayTracing', 'Capture', 'NoPrecompile', 'Log', 'DryRun') {
    if ($PSBoundParameters[$switch]) { $launch[$switch] = $true }
}
if ($Resolution) { $launch.Resolution = $Resolution }
if ($Adapter) { $launch.Adapter = $Adapter }
& (Join-Path $runtime "scripts\play-nvidia.ps1") @launch
