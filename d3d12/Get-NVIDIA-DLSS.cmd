@echo off
rem Downloads the NVIDIA files that DLSS and DLSS frame generation need (about 70 MB) from NVIDIA's own
rem GitHub and puts them where the launcher looks for them (nvidia-sdk\ in this folder). Run it once.
rem They are NVIDIA's files under NVIDIA's licences, which are saved beside them; this repository does
rem not contain them. Without them the game still runs: its picture is stretched instead of going
rem through DLSS, and there is no DLSS frame generation.
rem   Get-NVIDIA-DLSS.cmd                 DLSS and frame generation
rem   Get-NVIDIA-DLSS.cmd -NoStreamline   DLSS only (about 60 MB)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\get-nvidia-sdks.ps1" %*
echo.
pause
