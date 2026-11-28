@echo off
rem Downloads Microsoft's newer Direct3D 12 runtime (the D3D12 Agility SDK, about 22 MB, from nuget.org) into
rem target\diag\D3D12 beside leonida.exe. Windows 10 needs it: its own Direct3D 12 stops at shader model 6.5 and
rem refuses every translated pipeline. Windows 11 does not (leonida.exe uses Windows' own runtime where it is newer).
rem   Get-D3D12.cmd               into target\diag\D3D12
rem   Get-D3D12.cmd -To FOLDER   into another folder (the D3D12 folder beside your leonida.exe)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\get-d3d12.ps1" %*
echo.
pause
