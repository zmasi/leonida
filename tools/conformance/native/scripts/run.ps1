param(
  [string]$Output = '',
  [string[]]$Extra = @()
)
# Renders every scene into $Output (default: <repo>/tools/conformance/native/out/run1).
# A folder already at $Output is emptied first, so no PNG from an earlier run is left behind, but only when it holds
# nothing except what a run writes (<scene>.png and manifest.json). A mistyped -Output (a drive, the repository, a
# home folder, a folder of anything else) is refused with exit 2 instead of deleted, and nothing is touched at all
# when the renderer has not been built.
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot

function Refuse-Output([string]$Why) {
  Write-Output "-Output $Output $Why; not deleting it. Pass an empty folder or a previous run's output folder."
  Write-Output 'EXITCODE 2'
  exit 2
}

if ($Output.Length -gt 0 -and [string]::IsNullOrWhiteSpace($Output)) { Refuse-Output 'is blank' }
if ([string]::IsNullOrEmpty($Output)) { $Output = Join-Path $root 'out/run1' }
$exe = Join-Path $root 'build/Release/conform_native.exe'
if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
  Write-Output "$exe does not exist; build first (scripts\build.ps1)"
  Write-Output 'EXITCODE 1'
  exit 1
}

if (Test-Path -LiteralPath $Output) {
  $item = Get-Item -LiteralPath $Output -Force
  if (-not $item.PSIsContainer) { Refuse-Output 'is a file' }
  # A junction or symbolic link: a recursive Remove-Item on it can delete the contents of the folder it points to.
  if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { Refuse-Output 'is a link or junction' }
  if ($null -eq $item.Parent) { Refuse-Output 'is a drive root' }
  if ([IO.Path]::GetFullPath($item.FullName).TrimEnd('\', '/') -ieq [IO.Path]::GetFullPath($root).TrimEnd('\', '/')) {
    Refuse-Output 'is the native conformance folder'
  }
  $foreign = @(Get-ChildItem -LiteralPath $item.FullName -Force |
    Where-Object { $_.PSIsContainer -or -not ($_.Name -eq 'manifest.json' -or $_.Extension -eq '.png') })
  if ($foreign.Count -gt 0) {
    $names = ($foreign | Select-Object -First 3 | ForEach-Object { $_.Name }) -join ', '
    Refuse-Output "holds files a run does not write ($names)"
  }
  Remove-Item -LiteralPath $item.FullName -Recurse -Force
}
& $exe $Output @Extra
$rc = $LASTEXITCODE
Write-Output "EXITCODE $rc"
exit $rc
