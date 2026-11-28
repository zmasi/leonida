# Builds conform_native. Run from anywhere:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\conformance\native\scripts\build.ps1
$ErrorActionPreference = 'Stop'

# The CMake Visual Studio includes, then one on PATH that has the Visual Studio generators (MSYS's does not), as in
# scripts\build_runtime.ps1; then the CMake installer's default folder. Empty when there is none.
function Find-CMake([string]$VisualStudio) {
    $ErrorActionPreference = 'Continue'
    if ($VisualStudio) {
        $bundled = Join-Path $VisualStudio 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
        if (Test-Path -LiteralPath $bundled -PathType Leaf) { return $bundled }
    }
    foreach ($command in @(Get-Command cmake -CommandType Application -All -ErrorAction SilentlyContinue)) {
        if ((& $command.Path --help 2>$null) -match 'Visual Studio') { return $command.Path }
    }
    $installed = Join-Path $env:ProgramFiles 'CMake\bin\cmake.exe'
    if (Test-Path -LiteralPath $installed -PathType Leaf) { return $installed }
    return ''
}

$root = Split-Path -Parent $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = ''
if (Test-Path -LiteralPath $vswhere -PathType Leaf) {
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
$cmake = Find-CMake $vs
if (-not $cmake) {
    Write-Output "CMake was not found: install Visual Studio's C++ CMake tools or CMake"
    exit 1
}
$source = $root
$build = Join-Path $root 'build'

# No -G: CMake takes the newest Visual Studio it knows, so the script names no version. CMAKE_GENERATOR would replace
# that choice, with one that may not take -A.
$generator = $env:CMAKE_GENERATOR
$env:CMAKE_GENERATOR = $null
try {
    & $cmake -S $source -B $build -A x64
} finally {
    $env:CMAKE_GENERATOR = $generator
}
if ($LASTEXITCODE -ne 0) { Write-Output "CONFIGURE EXIT $LASTEXITCODE"; exit $LASTEXITCODE }

& $cmake --build $build --config Release --parallel
$buildExit = $LASTEXITCODE
Write-Output "BUILD EXIT $buildExit"
exit $buildExit
