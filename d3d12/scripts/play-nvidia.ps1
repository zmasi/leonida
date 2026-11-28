# Starts the title on an NVIDIA card: Series S, the game's own 720p picture taken to 1080p by DLSS, the F1
# settings menu in the game (measured on an RTX 5070: about 15-17 fps before generated frames).
#   scripts\play-nvidia.ps1 -Exe <path of your own copy's executable>
# DLSS and DLSS frame generation need a few NVIDIA files (about 70 MB) that this repository does not carry:
# run Get-NVIDIA-DLSS.cmd in the repository's folder once, and they are fetched from NVIDIA's GitHub into
# nvidia-sdk\. Without them the game runs with its picture stretched to the screen instead.
# The layer is built here if it is not built yet, and built again with DLSS once those files are there.
# In the game: F1 = settings (arrows pick and change, Enter saves; kept in leonida-settings.txt next to this
# folder), F3 = the numbers panel.
# -NoSmoothMotion  run target\<build>\leonida.exe itself. By default a copy named leonida_sm.exe is started instead,
#                  so a Smooth Motion profile set for that exe in the NVIDIA App applies every time.
# -NoDlss          no DLSS: the picture is stretched (fast, softer)
# -SeriesX         the Series X profile instead (much slower). Without -Dlss or -FrameGen the title's own upscaler
#                  makes its 3840x2160 output, as before.
# -Dlss            DLSS makes the output, from a render height by mode: quality (2/3 of the output's lines: 1440 on
#                  Series X, the game's own 720 on Series S), balanced (0.6: 1296), performance (1/2: 1080) or ultra
#                  (1/3: 720). On Series X the title is made to upscale through FidelityFX as on Series S
#                  (LEONIDA_FSR_UPSCALER), which is where DLSS takes over.
# -FrameGen        DLSS frame generation: 2 (twice the frames shown), up to 6 where the card does more (RTX 40: 2).
#                  It takes depth and motion vectors from the DLSS upscale, so on Series X it turns -Dlss quality on
#                  unless a mode is given.
# -Aspect          the window's shape: 21:9, 32:9, 16:9, W:H or auto (the screen's). The game still draws 16:9.
# -PresentScale    fit (the 16:9 picture centred, black bars beside it: the default with -Aspect) or stretch
# -Quality         fewer stretched/jumping bodies, slower (relay stage 1, no card steps)
# -RayTracing      the title's real ray tracing (the structure builder runs). Shaders build for minutes the
#                  first time, and DLSS frame generation is left off with it (the two together do not work yet).
# -Resolution      the older path: the game renders at this height (2160, 1440, 1080, 720, 540 or 480) and the
#                  title's own upscaler does the rest, without DLSS. The play-S-*.ps1 and play-X-*.ps1 scripts use it.
# -Adapter         on a PC with more than one card, the one to use: part of its name (-Adapter 5060) or its index
#                  (-Adapter '#1'). Default: Windows' default card.
# -Build           target\diag (the default, the build all measurements are of) or target\release
# -DlssDir         the folder with nvngx_dlss.dll, if not nvidia-sdk\DLSS\lib\Windows_x86_64\rel
# -StreamlineDir   the folder with sl.interposer.dll (frame generation), if not nvidia-sdk\Streamline\bin\x64
# -NoPrecompile    start at once. By default every shader of the title is translated into the cache and the
#                  pipelines earlier runs drew with are made before the title starts (minutes the first time
#                  and after a change of the layer, seconds otherwise), so that the world does not come up
#                  half drawn or stall while they are made.
# -Log             the layer's log goes to a file in %LOCALAPPDATA%\Leonida\logs instead of this window
# -Capture         F9 in the game saves that frame and the pictures it drew, in %LOCALAPPDATA%\Leonida\captures
# -WindowsBarMB    maximum MiB of title memory windows in CPU-mapped VRAM (by default 3072 on PCs with under
#                  24 GB of RAM, else 0; -WindowsBarMB 0 turns it off).
# -More            other settings: -More @{LEONIDA_PANEL='1'}
# -DryRun          prints the settings this start would run with (after the F1 menu's saved ones) and stops: nothing
#                  is built or started
# Run it from a normal window, not as administrator: the driver then keeps its shader cache elsewhere.
param(
    [Parameter(Mandatory)][string]$Exe,
    [switch]$NoSmoothMotion,
    [switch]$NoDlss,
    [switch]$SeriesX,
    [switch]$Quality,
    [switch]$RayTracing,
    [ValidateSet('2160', '1440', '1080', '720', '540', '480')][string]$Resolution,
    [ValidateSet('quality', 'balanced', 'performance', 'ultra')][string]$Dlss,
    [ValidateRange(2, 6)][int]$FrameGen,
    [ValidatePattern('^(auto|\d+(\.\d+)?([:x/]\d+(\.\d+)?)?)$')][string]$Aspect,
    [ValidateSet('fit', 'stretch')][string]$PresentScale,
    [string]$Adapter,
    [ValidateSet('diag', 'release')][string]$Build = 'diag',
    [string]$DlssDir,
    [string]$StreamlineDir,
    [switch]$NoPrecompile,
    [switch]$Log,
    [switch]$Capture,
    [hashtable]$More = @{},
    [uint32]$WindowsBarMB,
    [switch]$DryRun
)
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not (Test-Path $Exe)) { Write-Host "No $Exe."; return }
if ($Dlss -and $Resolution) { Write-Host '-Dlss sets the render height by its mode: leave -Resolution out with it.'; return }
if ($Dlss -and $NoDlss) { Write-Host '-Dlss and -NoDlss: one or the other.'; return }
$Exe = (Resolve-Path $Exe).Path

# NVIDIA's own files: where they are (Get-NVIDIA-DLSS.cmd puts them in nvidia-sdk\). Nothing is downloaded here.
if (-not $DlssDir) { $DlssDir = $env:LEONIDA_DLSS_DIR }
if (-not $StreamlineDir) { $StreamlineDir = $env:LEONIDA_STREAMLINE_DIR }
$sdkDlss = Join-Path $root 'nvidia-sdk\DLSS'
$sdkDlssLib = Join-Path $sdkDlss 'lib\Windows_x86_64\x64\nvsdk_ngx_d.lib'
$sdkStreamline = Join-Path $root 'nvidia-sdk\Streamline\bin\x64'
# DLSS by default on Series S; on Series X when -Dlss or -FrameGen asks for it.
$wantDlss = -not $NoDlss -and -not $Resolution -and (-not $SeriesX -or $Dlss -or $FrameGen)
if (-not $DlssDir -and (Test-Path (Join-Path $sdkDlss 'lib\Windows_x86_64\rel\nvngx_dlss.dll'))) { $DlssDir = Join-Path $sdkDlss 'lib\Windows_x86_64\rel' }
if (-not $StreamlineDir -and (Test-Path (Join-Path $sdkStreamline 'sl.interposer.dll'))) { $StreamlineDir = $sdkStreamline }
if ($wantDlss -and -not $DlssDir) {
    Write-Host 'No DLSS files here: the picture is stretched to the screen instead. Get-NVIDIA-DLSS.cmd (in the'
    Write-Host "repository's folder) fetches them from NVIDIA once, about 70 MB, with frame generation's."
}

# The build: made here if there is none, and made again with DLSS once its SDK is there (the diag build only).
$leonida = Join-Path $root "target\$Build\leonida.exe"
$built = Join-Path $root "target\$Build\built-with.txt"
$hasDlss = (Test-Path $built) -and ((Get-Content $built -Raw) -match 'dlss')
$canDlss = $wantDlss -and (Test-Path $sdkDlssLib)
if (-not $DryRun -and $Build -eq 'diag' -and (-not (Test-Path $leonida) -or ($canDlss -and -not $hasDlss -and (Test-Path $built)))) {
    if (-not (Get-Command cargo -ErrorAction SilentlyContinue)) { Write-Host 'No build and no cargo: install Rust (README.md), then start this again.'; return }
    Write-Host 'Building the layer (a few minutes the first time) ...'
    & (Join-Path $PSScriptRoot 'build-nvidia.ps1')
    $hasDlss = (Test-Path $built) -and ((Get-Content $built -Raw) -match 'dlss')
}
if (-not $DryRun -and -not (Test-Path $leonida)) { Write-Host "No $leonida. Build it first (scripts\build-nvidia.ps1)."; return }
# A build nobody noted the making of (cargo by hand) is taken at its word: with DLSS if the files are there.
# $dlssCan: DLSS can run (its files and a build with it), whoever asks for it: this start, or the F1 menu later.
$dlssCan = -not $NoDlss -and $DlssDir -and ($hasDlss -or -not (Test-Path $built))
$dlssOn = $wantDlss -and $dlssCan

$settings = @{
    # The play set. Textures are waited for: one loaded once at the start could stay empty for the run otherwise.
    LEONIDA_TEXTURE_NO_WAIT = '0'; LEONIDA_RELAY = '2'; LEONIDA_CARD_STEPS = '2'
    LEONIDA_WIDE_BACK = '1'; LEONIDA_CARD_REHASH_RUNS = '1'
    LEONIDA_JOIN_COPIES = '1'; LEONIDA_INDIRECT_WINDOW = '1'; LEONIDA_RELAY_COPY = '1'
    # Copy-backs caught half-written are fetched again (torn.rs).
    LEONIDA_FETCH_CHECK = '1'
    # Leonida: indirect batches end at every dispatch (INDIRECT_BATCH=1, not 2). With 2 a batch opened before the
    # title's culling and tessellation dispatches clamped their arguments before they were written: the sea's surface
    # and its tiled resolve went missing for a frame (sand or black blocks), 6 times in 271 frames; with 1, none in 441.
    LEONIDA_INDIRECT_BATCH = '1'; LEONIDA_MAP_THREADS = '1'; LEONIDA_TITLE_PRIORITIES = '0'; LEONIDA_CARD_AWAKE = '1'
    # Leonida: the light stand-in stays on while the rays are off (below). Off, with every ray missing, shade went
    # black and the sky white (run n1, 453 frames); on, the same view is lit as on the console. What the title left
    # is kept up to 2 times the estimate (ambient.rs).
    LEONIDA_SHOW_AFTER = '1'; LEONIDA_AMBIENT = '0.8'; LEONIDA_AMBIENT_MOST = '2'; LEONIDA_D3D12_DRED = 'light'
    LEONIDA_THIN = '1'; LEONIDA_JOIN_PARTS = '8'; LEONIDA_UPLOAD_AHEAD = '1'
    # Fog bounded by 1.5 times the light around; a NaN a shader writes is made 0 before blur and the upscaler spread it.
    LEONIDA_AMBIENT_AIR = '1.5'; LEONIDA_NAN_SCRUB = '1'
    # Leonida: the samplers the tonemap's programs (PS_Composite*) carry in their code filter linearly where they
    # would point-sample. Without it the colour grading came out in bands (a 2000s camera look): the tonemap reads
    # its 33^3 identity grading tables at a log-encoded colour, and read unfiltered they hand back 33 steps over 14
    # stops, about five flat levels per doubling of brightness (runs p3, p4, c1: 700-1,400 colours in a sky region;
    # run s1 with every program's made linear: 28,000-32,000). What read them unfiltered was a pipeline restored
    # from the pipeline library with another run's sampler places; such pipelines are kept by those places since,
    # or left to the driver (pipeline/places.rs), so this is a second guard for the tonemap alone.
    # It was '1' (every program) until 2026-11-27. The title point-samples on purpose in 1,468 of its program names:
    # the pass that puts the low-resolution fog on the picture picks its four texels by depth one by one, and read
    # linearly each of them is a mix across the silhouette (pipeline/host.rs LinearScope; the expected cause of the
    # light rim around trees, wires and signs in fog, not confirmed in a run yet). -More
    # @{LEONIDA_STATIC_SAMPLERS_LINEAR='1'} is the setting from before, '0' none at all.
    LEONIDA_STATIC_SAMPLERS_LINEAR = 'PS_Composite'
    # Pixel programs keep the console's early depth test (emit/mod.rs): transparent surfaces behind a wall no
    # longer write depth and motion for the wall's pixels. On since 2026-11-27 (a start with it: the picture as
    # before at the dock; the glass door's squares not looked at yet). '0' in -More: the late test, as before.
    LEONIDA_EARLY_DEPTH = '1'
    # A copy at its place in a list into bytes no shader wrote lately is the title's own: what reads it in the same
    # submission does not wait for every flight to land (host.rs, placed_is_titles). '0' in -More: waited for.
    LEONIDA_PLACED_TITLE = '1'
    # Wait for new shaders rather than skip their draws (each pipeline is waited for at most once). The F1 menu's
    # SHADER WAIT MS changes it: 0 never waits, things then show up as their shaders are done.
    LEONIDA_PIPELINE_WAIT_MS = '10000'
    # NVIDIA caps the shader-visible view heap at 1,000,000: without more dynamic slots thousands of draws a frame
    # are dropped (descriptor/host.rs dynamic_slots).
    LEONIDA_DYNAMIC_SLOTS = '524288'
    # Memory compares on the card instead of the CPU.
    # Leonida: every title buffer of 64 KiB and more gets its copy in the card's memory (not only those of 1 MiB and
    # more): the frame is GPU-bound, and shaders read the small ones over the bus otherwise. Static scene, Series X
    # 1440p: 120.7 -> 94.7 ms a frame, the card's time 64.1 -> 33.9 ms (2026-11-27, two runs each way).
    LEONIDA_CARD_BUFFERS_MIN_MB = '1'; LEONIDA_CARD_BUFFERS_MIN_KB = '64'; LEONIDA_CARD_BUFFERS_MB = '1536'
    # Leonida: a small fill of upload memory the title wrote itself goes on without waiting for the flights whose
    # shaders are declared to write around it (host.rs fill_is_titles). Each such wait emptied the pipeline, about
    # 75 ms, every few frames. A/B over moves, 7 phases of 30 s each way (2026-11-27): mean 116.8 -> 104.9 ms, median
    # 112.3 -> 102.3, p90 158 -> 130; waits behind placed copies 266 -> 5. The standard moves captured with it on:
    # 227 frames, none with a one-frame dip or spike, lighting as before (run x34).
    LEONIDA_FILL_TITLE = '1'
    # Leonida: what shaders write into host copies of memory the title asked for write-combined (1 MiB and more) is
    # not copied back into title memory: the title does not read such memory on the CPU (after trustno1-coder/
    # VIctory-NVIDIA 50c15a1 and 2f81d0c, GPL-3.0). Standing at the save, A/B of 20 s phases, three of each of 8
    # combinations (2026-11-27): median 91.0 -> 80.2-82.5 ms a frame; pieces landed a frame 1,416 -> 341, their
    # pages with a new byte 41 -> 6.6 MB, "copying back" 30-40 -> 10-12 ms. The standard moves captured with it on:
    # 338 frames, one with a one-frame dip or spike, the picture as before (run x36).
    LEONIDA_WC_NO_BACK = '1'
}
$console = if ($SeriesX) { 'series-x' } else { 'series-s' }
# The render scale of each DLSS mode (rendered lines over output lines; the layer picks DLSS's own mode by it).
$dlssScale = @{ quality = '0.6667'; balanced = '0.6'; performance = '0.5'; ultra = '0.3333' }
if ($DlssDir) { $settings.LEONIDA_DLSS_DIR = $DlssDir }
if ($Resolution) {
    # The older path: a render scale and the title's own upscaler (rendered height over output height).
    $outHeight = if ($SeriesX) { 2160 } else { 1080 }
    # Series X at 1440 lines or fewer: the title's own output is 2560x1440 (LEONIDA_OUTPUT, the loader's hooks.rs),
    # not 3840x2160 shrunk into the window afterwards. The frame takes as long (it is not the pixels': 2026-11-27),
    # the picture was the same in the runs of that day, and the card and the PC hold about 1.5 GB less.
    # -More @{LEONIDA_OUTPUT='3840x2160'} is the 4K output as before.
    if ($SeriesX -and [int]$Resolution -le 1440 -and -not $More.ContainsKey('LEONIDA_OUTPUT')) {
        $settings.LEONIDA_OUTPUT = '2560x1440'
        $outHeight = 1440
    }
    if ([int]$Resolution -gt $outHeight) { Write-Host "That console outputs ${outHeight}p: ${Resolution}p is above it."; return }
    $settings.LEONIDA_RENDER_SCALE = ([int]$Resolution / $outHeight).ToString('0.####', [Globalization.CultureInfo]::InvariantCulture)
    $settings.LEONIDA_OWN_UPSCALER = '1'
} elseif ($dlssOn) {
    $settings.LEONIDA_UPSCALE = 'dlss'
    # Series S renders 2/3 of its output by itself: quality needs no scale there. Series X renders 1.0 through
    # FidelityFX unless told (the scale is set below, quality if no mode was given).
    if ($Dlss -and ($SeriesX -or $Dlss -ne 'quality')) { $settings.LEONIDA_RENDER_SCALE = $dlssScale[$Dlss] }
} elseif (-not $SeriesX) {
    $settings.LEONIDA_UPSCALE = 'stretch'
    if ($Dlss -and $Dlss -ne 'quality') { $settings.LEONIDA_RENDER_SCALE = $dlssScale[$Dlss] }
}
if ($Dlss -and -not $dlssOn) { Write-Host "-Dlss ${Dlss}: no DLSS here (its files, or a build with it): the title's picture is not made by DLSS." }
if ($FrameGen) { $settings.LEONIDA_FRAME_GEN = "$FrameGen" }
if ($Aspect) { $settings.LEONIDA_ASPECT = $Aspect }
if ($PresentScale) { $settings.LEONIDA_PRESENT_SCALE = $PresentScale }
if ($StreamlineDir) { $settings.LEONIDA_STREAMLINE_DIR = $StreamlineDir }
if ($Quality) { $settings.LEONIDA_RELAY = '1'; $settings.LEONIDA_CARD_STEPS = '0' }
if ($RayTracing) {
    # The title's own rays light the picture: no stand-in beside them.
    $settings.LEONIDA_BVH_BUILDER = '1'; $settings.LEONIDA_AMBIENT = '0'; $settings.Remove('LEONIDA_AMBIENT_MOST')
} else {
    # Leonida: ray tracing mostly off, as before the renderer changed. Ray walks end at the root (emit/intersect.rs),
    # and rt_off_hashes.txt lists the 1,000 CS_RaytracingResolve and 34 traced-shadow shaders, which are skipped:
    # run with every ray a miss they made rooms white or black and took the card 33-50 ms a frame (about 10 without).
    # The screen-probe and visibility traces keep running: skipping those too left their outputs as garbage.
    $settings.LEONIDA_BVH_MISS = '1'
    # Leonida: the resolve and ray-activation passes run again (every ray a miss), as klbf-lab 64ff6a0 found: with
    # them skipped, foliage in shade, blinds and the deck at the save showed magenta blocks and stair-stepped bars.
    # Series X 1440p, the standard moves: magenta 0.00 per mille in 316 frames (bars in every turn before), frames
    # 60-91 ms, brightness as before (2026-11-27). LEONIDA_SKIP_RT_RESOLVE=1 skips them as before (rt_off_hashes.txt).
    $settings.LEONIDA_RT_RESOLVE = '1'; $settings.LEONIDA_ACTIVATE_RAYS = '1'
    $rtList = Join-Path $PSScriptRoot 'rt_off_hashes.txt'
    if ($env:LEONIDA_SKIP_RT_RESOLVE -eq '1' -and (Test-Path $rtList)) { $settings.LEONIDA_SKIP_PIPELINES = (Get-Content $rtList -Raw).Trim() }
}
# Leonida: the water's CS_TransmissionVolumetrics program 6f225b641c5b0708 translates since the wave-wide form
# (docs/findings.md), but that form puts 115 group barriers in its outer loop (an exchange between the two host
# waves of each console wave at every branch on EXEC): one indirect dispatch of it took 76-80 ms of an RTX 4060 Ti's
# frame, the next heaviest command 3 ms (GPU timing run, 2026-11-27; frames 180-260 ms against about 95 without it).
# Its dispatches were dropped for that. With the paired-lane form (emit/paired.rs, after Vice-Tay 5e588e8: a host
# thread runs two console lanes, so the halves never exchange) it has 5 group barriers, none in a loop, and is no
# longer among the 40 costliest commands of the timing run (under 0.3 ms a frame), so it runs again.
# It is dropped again all the same, until its picture is right: within the hour it ran by default, a dark band
# with a hard horizontal edge and blocky fringes covered the lower third of the screen near water (the user's
# screenshot, 2026-11-27), as if everything under a water line were inside the volume. LEONIDA_WATER_TRANSMISSION=1
# runs it.
if ($env:LEONIDA_WATER_TRANSMISSION -ne '1') {
    # The whole family (the water's CS_TransmissionVolumetrics, findings.md): another configuration binds another
    # variant (1440p output with render scale 1.0: 5dc3b25986622949, whose paired form left a band of blocks over the
    # lower fifth of the picture, 2026-11-27).
    $skip = @('6f225b641c5b0708', '5dc3b25986622949', '926c46bab3b86bd6', '2889c21328c69e58', '6871d6e4b3ac9a10', '888af37a584acf98', 'a10179ab7eda0dc3')
    if ($settings.LEONIDA_SKIP_PIPELINES) { $skip = @($settings.LEONIDA_SKIP_PIPELINES) + $skip }
    $settings.LEONIDA_SKIP_PIPELINES = $skip -join ','
}
if ($Adapter) { $settings.LEONIDA_ADAPTER = $Adapter }
if ($PSBoundParameters.ContainsKey('WindowsBarMB')) {
    $settings.LEONIDA_WINDOWS_BAR_MB = $WindowsBarMB.ToString([Globalization.CultureInfo]::InvariantCulture)
} else {
    # Without -WindowsBarMB, PCs with under 24 GB of RAM get 3072 (PR #6). Windows lets a card share at most half
    # the RAM, and the windows filled that on a 16 GB PC (RTX 3060) about a minute into the world (0x8007000E, then
    # a driver fault). 32 GB and more never got there, so they keep the runtime default (0, off).
    # Under 48 GB since 2026-11-27: on a 32 GB PC the layer held 28 GB (19 of it in memory), the PC had 745 MB
    # free and read 100-200 pages a second from the page file; with the windows in the card's memory it held 24.7
    # GB and the PC had 3 GB free. The frame took as long either way.
    $ram = try { (Get-CimInstance Win32_ComputerSystem -ErrorAction Stop).TotalPhysicalMemory } catch { 0 }
    if ($ram -gt 0 -and $ram -lt 48GB) {
        $settings.LEONIDA_WINDOWS_BAR_MB = '3072'
        Write-Host "$([math]::Round($ram / 1GB)) GB of RAM: title memory windows in the card's memory the CPU maps, up to 3 GB (-WindowsBarMB 0 turns this off)"
    }
}

# What the F1 menu saved (leonida-settings.txt, git-ignored), over the settings above. An empty value unsets one.
$menuFile = Join-Path $root 'leonida-settings.txt'
$smooth = -not $NoSmoothMotion
if (Test-Path $menuFile) {
    foreach ($line in Get-Content $menuFile) {
        if ($line -notmatch '^\s*(LEONIDA_[A-Z0-9_]+)\s*=(.*)$') { continue }
        $k = $Matches[1]; $v = $Matches[2].Trim()
        if ($k -eq 'LEONIDA_MENU_SMOOTH_MOTION') { $smooth = $v -ne '0'; continue }
        if ($k -eq 'LEONIDA_MENU_CONSOLE') { if ($v) { $console = $v }; continue }
        if ($v) { $settings[$k] = $v } else { $settings.Remove($k) }
    }
}
# Leonida: the runs write down the pipelines the title creates (pipeline/known.rs), for the precompile below.
if (-not $NoPrecompile) { $settings.LEONIDA_PIPELINE_BOOK = '1' }
foreach ($k in $More.Keys) { $settings[$k] = $More[$k] }
# DLSS only where it can be: a menu choice of it without the files or the build falls back to the stretch
# (Series X: to the title's own upscaler, which is sharper than a stretch to 3840x2160).
if ($settings.LEONIDA_UPSCALE -eq 'dlss' -and -not $dlssCan) {
    if ($console -eq 'series-x') { $settings.Remove('LEONIDA_UPSCALE') } else { $settings.LEONIDA_UPSCALE = 'stretch' }
}
# DLSS frame generation and real ray tracing do not work together yet (0.1 frames a second): with ray tracing
# on, frame generation is left off and Smooth Motion can do the extra frames.
if ($settings.LEONIDA_BVH_BUILDER -eq '1' -and $settings.LEONIDA_FRAME_GEN -and $settings.LEONIDA_FG_WITH_RT -ne '1') {
    Write-Host 'Ray tracing is on: DLSS frame generation is left off (the two together do not work yet).'
    $settings.Remove('LEONIDA_FRAME_GEN')
}
if ($settings.LEONIDA_FRAME_GEN -and -not $StreamlineDir) {
    Write-Host 'Frame generation needs the Streamline files (Get-NVIDIA-DLSS.cmd): left off.'
    $settings.Remove('LEONIDA_FRAME_GEN')
}
# Series X: the title upscales with its own upscaler, as passes that hand nothing over. DLSS (or the stretch)
# can stand in for its FidelityFX upscale instead, which the title is then made to use (LEONIDA_FSR_UPSCALER, the
# loader's hooks.rs): only when -Dlss asks for it, because that path lights nothing yet (the world is black with
# the HUD over it, 2026-11-27). Frame generation alone keeps the title's own upscaler: its depth and motion
# vectors are the G-buffer pass's (leonida-gpu's fg.rs, LEONIDA_FG_GBUFFER).
if ($console -eq 'series-x') {
    if ($settings.LEONIDA_UPSCALE -eq 'dlss' -and -not $Dlss -and -not $More.ContainsKey('LEONIDA_UPSCALE')) {
        # Asked for by -FrameGen alone ($wantDlss above), not by -Dlss: the title's own upscaler stays.
        $settings.Remove('LEONIDA_UPSCALE')
        if (-not $settings.LEONIDA_RENDER_SCALE) { $settings.LEONIDA_RENDER_SCALE = $dlssScale.quality }
        $settings.LEONIDA_OWN_UPSCALER = '1'
    }
    if ($settings.LEONIDA_UPSCALE -in 'dlss', 'stretch') {
        Write-Host 'Series X with DLSS: experimental, the world is not lit on this path yet (leave -Dlss out for the picture).'
        $settings.LEONIDA_FSR_UPSCALER = '1'; $settings.Remove('LEONIDA_OWN_UPSCALER')
        # Through FidelityFX the title renders 1.0 of its output there: 1440 lines (quality) unless a scale is set.
        if (-not $settings.LEONIDA_RENDER_SCALE) { $settings.LEONIDA_RENDER_SCALE = $dlssScale.quality }
    } else {
        $settings.Remove('LEONIDA_FSR_UPSCALER')
    }
}
# Smooth Motion is a second frame generator: off while DLSS frame generation is on.
if ($settings.LEONIDA_FRAME_GEN -and $settings.LEONIDA_FRAME_GEN -ne '1') { $smooth = $false }
# For the menu: what it shows as the current values, where it saves, and how it starts the game again.
$settings.LEONIDA_MENU_SMOOTH_MOTION = if ($smooth) { '1' } else { '0' }
$settings.LEONIDA_MENU_CONSOLE = $console
$settings.LEONIDA_MENU_FILE = $menuFile
$settings.LEONIDA_MENU_PLAY = $MyInvocation.MyCommand.Path
$again = @("-Exe '$($Exe -replace "'", "''")'", "-Build $Build")
foreach ($s in 'NoSmoothMotion', 'NoDlss', 'SeriesX', 'Quality', 'RayTracing', 'NoPrecompile', 'Log', 'Capture') {
    if ($PSBoundParameters[$s]) { $again += "-$s" }
}
foreach ($s in 'Resolution', 'Dlss', 'FrameGen', 'Aspect', 'PresentScale', 'Adapter', 'DlssDir', 'StreamlineDir') {
    if ($PSBoundParameters[$s]) { $again += "-$s '$($PSBoundParameters[$s] -replace "'", "''")'" }
}
if ($PSBoundParameters.ContainsKey('WindowsBarMB')) { $again += "-WindowsBarMB $WindowsBarMB" }
$settings.LEONIDA_MENU_ARGS = $again -join ' '

$scaled = if ($settings.LEONIDA_RENDER_SCALE) { " from render scale $($settings.LEONIDA_RENDER_SCALE)" } else { '' }
# With the title's own upscaler forced, DLSS and the stretch are never asked: the title upscales by itself.
$own = $settings.LEONIDA_OWN_UPSCALER -and $settings.LEONIDA_OWN_UPSCALER -ne '0'
$picture = if ($own -and $settings.LEONIDA_RENDER_SCALE) { "render scale $($settings.LEONIDA_RENDER_SCALE), the title's upscaler" }
    elseif ($own) { "the title's upscaler" }
    elseif ($settings.LEONIDA_UPSCALE -eq 'dlss') { "DLSS$scaled" }
    elseif ($settings.LEONIDA_UPSCALE -eq 'stretch') { "stretched$scaled (no DLSS)" }
    elseif ($settings.LEONIDA_RENDER_SCALE) { "render scale $($settings.LEONIDA_RENDER_SCALE), the title's upscaler" }
    else { "the title's upscaler" }
$extras = ''
if ($settings.LEONIDA_ASPECT) { $extras += ", window $($settings.LEONIDA_ASPECT)" }
if ($settings.LEONIDA_FRAME_GEN) { $extras += ", frame generation $($settings.LEONIDA_FRAME_GEN)x" }
if ($settings.LEONIDA_BVH_BUILDER -eq '1') { $extras += ', ray tracing' }
Write-Host "Series $(if ($console -eq 'series-x') { 'X' } else { 'S' }), $picture$extras. F1 in the game: settings."
if ($DryRun) {
    $settings.GetEnumerator() | Sort-Object Name | ForEach-Object { Write-Output "$($_.Name)=$($_.Value)" }
    return
}

if ($smooth) {
    # A copy under the name the NVIDIA App's Smooth Motion profile is set for, refreshed when the build changes.
    $sm = Join-Path (Split-Path $leonida) 'leonida_sm.exe'
    if (-not (Test-Path $sm) -or (Get-Item $sm).LastWriteTime -lt (Get-Item $leonida).LastWriteTime) {
        try { Copy-Item $leonida $sm -Force -ErrorAction Stop }
        catch {
            # Still held by a game that has not quite gone: Windows lets it be renamed out of the way.
            Rename-Item $sm "leonida_sm.exe.old-$(Get-Date -Format HHmmss)" -ErrorAction SilentlyContinue
            Copy-Item $leonida $sm -Force
        }
    }
    $leonida = $sm
}
Get-ChildItem (Join-Path (Split-Path $leonida) '*.old-*') -ErrorAction SilentlyContinue | Remove-Item -ErrorAction SilentlyContinue

# (Other settings left over in this window go; where your files are stays.)
$paths = 'LEONIDA_CONFIG', 'LEONIDA_GAME_DIR', 'LEONIDA_CACHE_DIR', 'LEONIDA_DATA_DIR'
Get-ChildItem Env:LEONIDA_* | Where-Object { $_.Name -notin $paths } | ForEach-Object { Remove-Item "Env:$($_.Name)" }
foreach ($k in $settings.Keys) { Set-Item "Env:$k" $settings[$k] }
# The title's own graphics settings (storage\settings\settings.xml in the data folder). It writes the file with its
# console defaults whenever it exits by itself: ray-traced dynamic shadows on, two samples of screen-space
# occlusion, motion blur, full detail and grass distances. -More @{LEONIDA_TITLE_GRAPHICS='performance'} writes the
# lighter set aki-runtime uses (Aki-Developer, Apply-LowVramSettings.ps1 and docs/session-2026-11-26.md of
# aki-gta6/aki-runtime: on its Series S profile the card's wait went from 50-69 ms to about 10) and keeps the first
# file beside it as settings.xml.original. Off by default: on the Series X profile here it changed nothing that
# could be measured at the save (three minutes of the idle camera each, 2026-11-27: 92.5 ms a frame and 6,353
# draws with the console's settings, 90.9-101.7 ms and 6,636-7,079 draws with the lighter set) and 338-350 draws a
# frame were dropped for want of a pipeline against 191.
# With another output size than the console's (LEONIDA_OUTPUT), the title's settings file is told that size as its
# UI and fullscreen resolution: left at 0x0 ("as the console") it lays the UI out for 3840x2160 and the weapon wheel
# and menus come out half again too big in a 2560x1440 picture (2026-11-27).
if (-not $DryRun -and $env:LEONIDA_DATA_DIR -and "$($settings.LEONIDA_OUTPUT)" -match '^(\d+)x(\d+)$') {
    $outW = $Matches[1]; $outH = $Matches[2]
    Get-ChildItem (Join-Path $env:LEONIDA_DATA_DIR '*\storage\settings\settings.xml') -ErrorAction SilentlyContinue | ForEach-Object {
        $file = $_.FullName
        $text = [IO.File]::ReadAllText($file)
        $new = $text
        foreach ($block in 'uiResolution', 'resolutionFullscreen') {
            $new = [regex]::Replace($new, "(<$block>\s*<width value=`")[^`"]*(`" />\s*<height value=`")[^`"]*(`" />)", "`${1}$outW`${2}$outH`${3}")
        }
        if ($new -ne $text) {
            if (-not (Test-Path "$file.original")) { Copy-Item $file "$file.original" }
            [IO.File]::WriteAllText($file, $new, (New-Object Text.UTF8Encoding $false))
            Write-Host "The title's UI and fullscreen resolution: ${outW}x$outH written to its settings (the first file is $file.original)."
        }
    }
}
$titleGraphics = if ($settings.LEONIDA_TITLE_GRAPHICS) { "$($settings.LEONIDA_TITLE_GRAPHICS)" } else { 'keep' }
if (-not $DryRun -and $titleGraphics -eq 'performance' -and $env:LEONIDA_DATA_DIR) {
    $lighter = [ordered]@{
        rtDynamicShadows = 'false'; rtLODLightShadows = 'false'; ssroSampleCount = '0'; motionBlur = 'false'
        shadowParticleShadows = 'false'; sharpenEnabled = 'false'; grassLod = '0.300000'; lodScale = '0.600000'
        vehicleHDCount = '0'; directionalScreenSpaceShadowQuality = '0.000000'; worldHeightShadowQuality = '0.000000'
        anisotropicFiltering = '16'; vSync = '0'
    }
    Get-ChildItem (Join-Path $env:LEONIDA_DATA_DIR '*\storage\settings\settings.xml') -ErrorAction SilentlyContinue | ForEach-Object {
        $file = $_.FullName
        $text = [IO.File]::ReadAllText($file)
        $new = $text
        foreach ($name in $lighter.Keys) {
            $new = [regex]::Replace($new, "(<$name value=`")[^`"]*(`" />)", "`${1}$($lighter[$name])`${2}")
        }
        if ($new -ne $text) {
            if (-not (Test-Path "$file.original")) { Copy-Item $file "$file.original" }
            [IO.File]::WriteAllText($file, $new, (New-Object Text.UTF8Encoding $false))
            Write-Host "The title's graphics settings: the lighter set written (the first file is $file.original)."
        }
    }
}
# A shader cache of its own: one an earlier version of this fork filled does not fit this renderer's pipelines.
$toml = Join-Path $root 'leonida.toml'
if (-not $env:LEONIDA_CACHE_DIR -and -not ((Test-Path $toml) -and (Select-String -Path $toml -Pattern '^\s*cache_dir\s*=' -Quiet))) {
    $env:LEONIDA_CACHE_DIR = Join-Path $env:LOCALAPPDATA 'Leonida\cache-nvidia-2'
}
# Leonida: shaders and pipelines are made before the title starts, not while it plays (as Hookshot's release
# builds do). Every shader of the title's shader archive is translated into the cache with the settings of this
# start: once per change of the translator or of a setting a translation depends on; a later start finds them all
# there. Then the host pipelines earlier runs drew with are made in processes of their own, so that the driver's
# cache on disk has them: after a change of the layer or of the driver, and for places first seen in the last run.
if (-not $NoPrecompile) {
    $archive = Join-Path (Split-Path $Exe) 'shaders.rpf'
    $from = if (Test-Path $archive) { $archive } else { Split-Path $Exe }
    Push-Location $root
    # Only what the two commands print themselves (their log goes to the error stream: quiet for these, and no
    # redirection of it, which Windows PowerShell 5.1 turns into errors).
    $logWas = $env:LEONIDA_LOG; $env:LEONIDA_LOG = 'error'
    $stopWas = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    try {
        Write-Host 'Precompiling shaders (minutes the first time, seconds after; -NoPrecompile skips it) ...'
        & $leonida shaders $from | Where-Object { $_ -match 'shaders in the cache|do not translate' } |
            ForEach-Object { Write-Host "  $_" }
        Write-Host 'Precompiling the pipelines earlier runs drew with ...'
        & $leonida warm --used | Where-Object { $_ -match 'pipelines known|^\d+ of |made in' } |
            ForEach-Object { Write-Host "  $_" }
    } finally {
        $ErrorActionPreference = $stopWas
        if ($null -eq $logWas) { Remove-Item Env:LEONIDA_LOG -ErrorAction SilentlyContinue } else { $env:LEONIDA_LOG = $logWas }
        Pop-Location
    }
}
$stamp = Get-Date -Format 'MMdd-HHmmss'
if ($Capture) {
    $env:LEONIDA_CAPTURE = Join-Path $env:LOCALAPPDATA "Leonida\captures\$stamp"
    $env:LEONIDA_DUMP_TARGETS = '1'
    New-Item -ItemType Directory -Force $env:LEONIDA_CAPTURE | Out-Null
    Write-Host "F9 saves a frame to $env:LEONIDA_CAPTURE"
}
$runArgs = @('run', "`"$Exe`"", '--execute', '--console', $console)
Push-Location $root   # leonida.toml is looked for here
try {
    if ($Log) {
        $logs = Join-Path $env:LOCALAPPDATA 'Leonida\logs'
        New-Item -ItemType Directory -Force $logs | Out-Null
        Get-ChildItem $logs -Filter 'play-*.log' | Sort-Object LastWriteTime -Descending | Select-Object -Skip 4 | Remove-Item -ErrorAction SilentlyContinue
        Write-Host "log: $logs\play-$stamp.log"
        Start-Process -FilePath $leonida -ArgumentList $runArgs -NoNewWindow -Wait -RedirectStandardError "$logs\play-$stamp.log"
    } else {
        Start-Process -FilePath $leonida -ArgumentList $runArgs -NoNewWindow -Wait
    }
} finally { Pop-Location }
