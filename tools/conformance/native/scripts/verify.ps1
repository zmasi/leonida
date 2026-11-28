param(
    [string]$ExpectedAdapter = 'RTX 5070 Ti',
    [string]$SecondAdapterPattern = 'adapter: AMD Radeon',
    [int]$SecondAdapterIndex = 0
)
# Re-checks every claim the README makes about the native renderer.
# Run: powershell -NoProfile -ExecutionPolicy Bypass -File verify.ps1
# Exit code 0 means every claim held.

$ErrorActionPreference = 'Continue'
$root_dir = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root_dir 'build/Release/conform_native.exe'
$root = Join-Path $root_dir 'out'
$script:broken = 0

function Check([bool]$condition, [string]$label) {
    if ($condition) {
        Write-Output "ok      $label"
    } else {
        Write-Output "BROKEN  $label"
        $script:broken += 1
    }
}

if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) { Write-Output "BROKEN  $exe does not exist; build first"; exit 1 }
if (Test-Path -LiteralPath $root) { Remove-Item -LiteralPath $root -Recurse -Force }

Write-Output '--- contract checks on the primary adapter ---'
& $exe "$root\a" | Out-Null
Check ($LASTEXITCODE -eq 0) 'run A exits 0 (all PixelChecks pass)'

Write-Output '--- determinism: two runs, byte compare ---'
& $exe "$root\b" | Out-Null
Check ($LASTEXITCODE -eq 0) 'run B exits 0'
$namesA = @(if (Test-Path -LiteralPath "$root\a" -PathType Container) { Get-ChildItem -LiteralPath "$root\a" | Select-Object -ExpandProperty Name | Sort-Object })
$namesB = @(if (Test-Path -LiteralPath "$root\b" -PathType Container) { Get-ChildItem -LiteralPath "$root\b" | Select-Object -ExpandProperty Name | Sort-Object })
Check ($namesA.Count -eq 7) "run A wrote 7 files (6 PNGs + manifest.json), found $($namesA.Count)"
Check (($namesA -join ',') -eq ($namesB -join ',')) "run B wrote identical filenames to run A ($($namesB.Count))"
foreach ($name in $namesA) {
    $fileA = "$root\a\$name"
    $fileB = "$root\b\$name"
    if (-not ((Test-Path -LiteralPath $fileA -PathType Leaf) -and (Test-Path -LiteralPath $fileB -PathType Leaf))) {
        Check $false "identical bytes: $name (missing in run A or B)"
        continue
    }
    $hashA = (Get-FileHash -LiteralPath $fileA -Algorithm SHA256).Hash
    $hashB = (Get-FileHash -LiteralPath $fileB -Algorithm SHA256).Hash
    Check ($hashA -eq $hashB) "identical bytes: $name $hashA"
}

Write-Output "--- second adapter (index $SecondAdapterIndex) ---"
$amdText = (& $exe "$root\amd" --adapter $SecondAdapterIndex 2>&1 | Out-String)
Check ($LASTEXITCODE -eq 0) "render on adapter $SecondAdapterIndex exits 0"
Check ($amdText -match $SecondAdapterPattern) "adapter $SecondAdapterIndex matches '$SecondAdapterPattern'"
Check ($amdText -match 'readback checks: 18 passed, 0 failed') "adapter $SecondAdapterIndex passes all 18 readback checks"
Check ($amdText -match 'png checks:      18 passed, 0 failed') "adapter $SecondAdapterIndex passes all 18 PNG checks"

Write-Output '--- failure paths ---'
[IO.Directory]::CreateDirectory($root) | Out-Null
Set-Content -LiteralPath "$root\blocked-by-file" -Value 'not a directory'
$fileText = (& $exe "$root\blocked-by-file" 2>&1 | Out-String)
Check ($LASTEXITCODE -eq 1) 'output path that is a file exits 1'
Check ($fileText -match 'create_directories') 'that failure names create_directories'
Check ($fileText -match 'blocked-by-file') 'that failure names the path'
Remove-Item -LiteralPath "$root\blocked-by-file" -Force

[IO.Directory]::CreateDirectory("$root\no-shaders") | Out-Null
$shaderText = (& $exe "$root\shader-failure" --shaders "$root\no-shaders" 2>&1 | Out-String)
Check ($LASTEXITCODE -eq 1) 'missing .cso files exit 1'
Check ($shaderText -match 'fullscreen_color.vs.cso') 'that failure names the missing file'
Check ($shaderText -match 'conform_shaders') 'that failure says which build target produces it'

[IO.Directory]::CreateDirectory("$root\png-blocked\clear.png") | Out-Null
$pngText = (& $exe "$root\png-blocked" 2>&1 | Out-String)
Check ($LASTEXITCODE -eq 1) 'unwritable PNG path exits 1'
Check ($pngText -match 'clear\.png') 'that failure names the PNG'
Check ($pngText -match 'RESULT: FAIL \(run did not complete\)') 'that run reports it did not complete'

$noArgText = (& $exe 2>&1 | Out-String)
Check ($LASTEXITCODE -eq 2) 'no arguments exits 2'
Check ($noArgText -match 'usage: conform_native') 'no arguments prints usage'

$badFlagText = (& $exe "$root\x" --nonsense 2>&1 | Out-String)
Check ($LASTEXITCODE -eq 2) 'unknown flag exits 2'

Write-Output '--- manifest ---'
$manifestPath = "$root\a\manifest.json"
if (Test-Path -LiteralPath $manifestPath -PathType Leaf) {
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    Check ($manifest.renderer -eq 'native') 'manifest renderer is native'
    Check ($manifest.adapter -match $ExpectedAdapter) 'manifest records the chosen adapter'
    Check ($manifest.scenes.Count -eq 6) 'manifest lists 6 scenes'
    $checkCount = 0
    foreach ($scene in $manifest.scenes) { $checkCount += $scene.checks.Count }
    Check ($checkCount -eq 18) 'manifest lists all 18 checks from scenes.h'
} else {
    Check $false 'manifest.json written in run A'
}

Write-Output ''
if ($script:broken -eq 0) {
    Write-Output 'VERIFY: PASS'
    exit 0
}
Write-Output "VERIFY: FAIL ($script:broken broken claim(s))"
exit 1
