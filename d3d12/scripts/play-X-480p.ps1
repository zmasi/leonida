# Series X, rendering at 480p (then the title's own upscaler).
#   scripts\play-X-480p.ps1 -Exe <path of your own copy's executable> [other play-nvidia.ps1 switches, e.g. -Quality]
& "$PSScriptRoot\play-nvidia.ps1" -SeriesX -Resolution 480 @args
