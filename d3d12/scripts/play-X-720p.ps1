# Series X, rendering at 720p (then the title's own upscaler).
#   scripts\play-X-720p.ps1 -Exe <path of your own copy's executable> [other play-nvidia.ps1 switches, e.g. -Quality]
& "$PSScriptRoot\play-nvidia.ps1" -SeriesX -Resolution 720 @args
