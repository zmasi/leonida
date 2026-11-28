# Builds the layer the way scripts\play-nvidia.ps1 expects it (the diag profile with diagnostics: the F1 menu,
# the F3 panel and several NVIDIA settings are diagnostics switches).
#   scripts\build-nvidia.ps1            with DLSS when nvidia-sdk\DLSS is there (Get-NVIDIA-DLSS.cmd fetches it),
#                                       without it otherwise
#   scripts\build-nvidia.ps1 -NoDlss    without DLSS even then
#   scripts\build-nvidia.ps1 -DlssSdk <your checkout of github.com/NVIDIA/DLSS>
# A build with DLSS links NVIDIA's loader library: it is for your own use, not for passing on.
param(
    [string]$DlssSdk,
    [switch]$NoDlss
)
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $DlssSdk) { $DlssSdk = Join-Path $root 'nvidia-sdk\DLSS' }
# mimalloc is the process allocator: with the system heap a quarter of the submitting thread's running time was
# in the heap (profile of 2026-11-27), and the same view took 95 ms a frame against 78 with it.
$features = 'diagnostics,mimalloc'
if (-not $NoDlss -and (Test-Path (Join-Path $DlssSdk 'lib\Windows_x86_64\x64\nvsdk_ngx_d.lib'))) {
    $env:LEONIDA_DLSS_SDK = (Resolve-Path $DlssSdk).Path
    $features = 'diagnostics,dlss,mimalloc'
    Write-Host "Building with DLSS (SDK: $env:LEONIDA_DLSS_SDK)"
} else {
    Write-Host 'Building without DLSS (Get-NVIDIA-DLSS.cmd fetches its files; without them the picture is stretched to the screen)'
}
Push-Location $root
try { cargo build --locked --profile diag -p leonida --features $features } finally { Pop-Location }
if ($LASTEXITCODE -eq 0) { Set-Content -Path (Join-Path $root 'target\diag\built-with.txt') -Value $features -Encoding ascii }
if ($LASTEXITCODE -eq 0) { Write-Host "Built: $(Join-Path $root 'target\diag\leonida.exe'). Next: scripts\play-nvidia.ps1 -Exe <your copy's executable>" }
