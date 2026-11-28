# From trustno1-coder/VIctory-NVIDIA (GPL-3.0-or-later), scripts/get-d3d12.ps1 of v2026.11.27.9.
# Fetches Microsoft's newer Direct3D 12 runtime (the D3D12 Agility SDK, D3D12Core.dll, from Microsoft's own
# NuGet package Microsoft.Direct3D.D3D12, about 22 MB) into a D3D12 folder: by default the one beside
# target\diag\leonida.exe, the build scripts\play-nvidia.ps1 runs; -To names another. Windows 10's own Direct3D 12
# stops at shader model 6.5, and every shader the layer translates is 6.6: without this runtime each pipeline is
# refused and the game closes within seconds. leonida.exe asks Windows for this runtime (it exports
# D3D12SDKVersion, set to the version of the file there, and D3D12SDKPath = .\D3D12\); Windows uses it only where
# its own is older. It is Microsoft's, under Microsoft's
# licence (saved beside it: read it); this repository does not carry it. The package and the DLL are checked
# against the SHA-256 of the copies tested, and the DLL against Microsoft's signature.
param(
    [string]$Version = '1.614.1',
    [string]$To = ''
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
$dir = if ($To) { $To } else { Join-Path (Resolve-Path (Join-Path $PSScriptRoot '..')).Path 'target\diag\D3D12' }
# The SHA-256 of the copies tested: D3D12Core.dll 1.614.1.0 first, since launchers take the first hash after
# that name as the DLL's (VILauncher's "Fix and verify" took the package's and then refused every package).
$core = '8A23D826B25B4329522FF451CB52B7F2B34D7F2913CFEB878371CE8BD765FE2D'
# The package it comes in, Microsoft.Direct3D.D3D12 1.614.1.
$package = '9880AA91602DD51DD6CF7911A2BCA7A2323513B15338573CDE014B3356EEAFF2'
$url = "https://api.nuget.org/v3-flatcontainer/microsoft.direct3d.d3d12/$Version/microsoft.direct3d.d3d12.$Version.nupkg"

$target = Join-Path $dir 'D3D12Core.dll'
if ((Test-Path $target) -and (Get-FileHash $target -Algorithm SHA256).Hash -eq $core) {
    Write-Host "Microsoft's Direct3D 12 runtime $Version is already in $dir"
    exit 0
}
Write-Host "Microsoft's Direct3D 12 runtime $Version (the Agility SDK), from nuget.org (about 22 MB):"
New-Item -ItemType Directory -Force $dir | Out-Null
$part = Join-Path $dir "microsoft.direct3d.d3d12.$Version.nupkg.part"
try {
    if (Get-Command curl.exe -ErrorAction SilentlyContinue) {
        & curl.exe --location --fail --silent --show-error --progress-bar --output $part $url
        if ($LASTEXITCODE -ne 0) { throw "could not get $url" }
    } else {
        Invoke-WebRequest -Uri $url -OutFile $part -UseBasicParsing
    }
    $hash = (Get-FileHash $part -Algorithm SHA256).Hash
    if ($hash -ne $package) { throw "the package is not the one this build was tested with (SHA-256 $hash); not kept" }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($part)
    try {
        $entry = $zip.Entries | Where-Object FullName -eq 'build/native/bin/x64/D3D12Core.dll'
        if (-not $entry) { throw 'the package has no build/native/bin/x64/D3D12Core.dll' }
        $dll = "$target.part"
        [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $dll, $true)
        $lic = $zip.Entries | Where-Object FullName -eq 'LICENSE.txt'
        if ($lic) { [IO.Compression.ZipFileExtensions]::ExtractToFile($lic, (Join-Path $dir 'microsoft-d3d12-license.txt'), $true) }
    } finally { $zip.Dispose() }
    $hash = (Get-FileHash $dll -Algorithm SHA256).Hash
    if ($hash -ne $core) { Remove-Item $dll -Force; throw "D3D12Core.dll is not the file this build was tested with (SHA-256 $hash); not kept" }
    $sig = Get-AuthenticodeSignature $dll
    if ($sig.Status -ne 'Valid' -or $sig.SignerCertificate.Subject -notlike 'CN=Microsoft Corporation,*') {
        Remove-Item $dll -Force; throw "D3D12Core.dll is not validly signed by Microsoft ($($sig.Status)); not kept"
    }
    Move-Item $dll $target -Force
} finally {
    Remove-Item $part -Force -ErrorAction SilentlyContinue
}
Write-Host ('  D3D12Core.dll  {0:N1} MB, signed by Microsoft' -f ((Get-Item $target).Length / 1MB))
Write-Host "  in $dir (licence: microsoft-d3d12-license.txt beside it)"
