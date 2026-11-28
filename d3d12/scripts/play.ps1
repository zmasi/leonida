# Starts the title with the settings this fork is played with (README.md, "How to run it").
#   scripts\play.ps1 -Exe <path of your own copy's executable>
# -Safe         without the newer, faster ways (the first thing to try when something is wrong)
# -RayTracing   the title's own ray-traced indirect light: lit interiors, about a third slower
# -Build        target\diag (the default, the build all measurements are of) or target\release
# -More         other settings: -More @{LEONIDA_PANEL='1'}
# Run it from a normal window, not as administrator: the driver then keeps its shader cache elsewhere.
param(
    [Parameter(Mandatory)][string]$Exe,
    [switch]$Safe,
    [switch]$RayTracing,
    [ValidateSet('diag', 'release')][string]$Build = 'diag',
    [hashtable]$More = @{}
)
$settings = @{
    LEONIDA_TEXTURE_NO_WAIT = '1'; LEONIDA_RELAY = '2'; LEONIDA_WIDE_BACK = '1'; LEONIDA_CARD_REHASH_RUNS = '1'
    LEONIDA_JOIN_COPIES = '1'; LEONIDA_INDIRECT_WINDOW = '1'; LEONIDA_RELAY_COPY = '1'; LEONIDA_CARD_STEPS = '2'
    LEONIDA_INDIRECT_BATCH = '2'; LEONIDA_MAP_THREADS = '1'; LEONIDA_TITLE_PRIORITIES = '0'; LEONIDA_CARD_AWAKE = '1'
    LEONIDA_SHOW_AFTER = '1'; LEONIDA_AMBIENT = '0.8'; LEONIDA_D3D12_DRED = 'light'; LEONIDA_SKIP_PIPELINES = 'none'
}
if ($Safe) {
    $settings.LEONIDA_UNDER_WIDE = '0'; $settings.LEONIDA_LAND_ECHO = '0'
} else {
    $settings.LEONIDA_THIN = '1'; $settings.LEONIDA_JOIN_PARTS = '8'; $settings.LEONIDA_UPLOAD_AHEAD = '1'
}
if ($RayTracing) { $settings.LEONIDA_BVH_BUILDER = '1' }
foreach ($k in $More.Keys) { $settings[$k] = $More[$k] }

$leonida = Join-Path $PSScriptRoot "..\target\$Build\leonida.exe"
if (-not (Test-Path $leonida)) { Write-Host "No $leonida. Build it first (README.md)."; return }
if (-not (Test-Path $Exe)) { Write-Host "No $Exe."; return }
# (Other settings left over in this window go; where your files are stays.)
$paths = 'LEONIDA_CONFIG', 'LEONIDA_GAME_DIR', 'LEONIDA_CACHE_DIR', 'LEONIDA_DATA_DIR'
Get-ChildItem Env:LEONIDA_* | Where-Object { $_.Name -notin $paths } | ForEach-Object { Remove-Item "Env:$($_.Name)" }
foreach ($k in $settings.Keys) { Set-Item "Env:$k" $settings[$k] }
$Exe = (Resolve-Path $Exe).Path
Push-Location (Join-Path $PSScriptRoot '..')   # leonida.toml is looked for here
try { & $leonida run $Exe --execute } finally { Pop-Location }
