<#
.SYNOPSIS
    Boot the retail executable under gta6rt on native Windows, one game process at a time, and print a digest.

.DESCRIPTION
    The native counterpart of scripts/linux-boot/: sets the switches both machines boot with (see
    docs/re/howto-blank-to-story-load.md, section 1), runs `gta6rt --run` with --trace and --trace-d3d12 for
    -Seconds, stops it, runs tools/boot_trace.py and prints a short digest: exit, game/session state over time,
    PresentX count, device-fault lines, capture means. Paste numbers from the digest into a RESULT on the
    coordination board, never the logs: they can quote the game (AGENTS.md, "No game data").

    Only one game process runs at a time: the script holds a named mutex and also waits while any gta6rt.exe is
    running (another agent's boot, or one started by hand).

    Every default switch is used only when the variable is not already set in this shell; other LEONIDA_*
    variables of the shell pass through; -SetEnv overrides all of them. -DryRun lists the result. Outputs go to -OutDir (never inside the repository):
        <OutDir>\<Name>.log          stderr of gta6rt: [rt] diagnostics and [d3d12-trace] lines
        <OutDir>\<Name>.stdout.log   stdout of gta6rt
        <OutDir>\<Name>.jsonl        startup trace (--trace)
        <OutDir>\<Name>.summary.txt  tools/boot_trace.py report
        <OutDir>\<Name>.digest.txt   the digest printed at the end
        <OutDir>\cap_<Name>\         swapchain captures (PPM) and index.txt with mean RGB per frame

.PARAMETER Name
    Run name; output files are named after it. Default: boot-<yyyyMMdd-HHmmss>.

.PARAMETER Seconds
    How long the game runs before it is stopped. Default 180.

.PARAMETER GameDir
    Your package folder (MicrosoftGame.Config and the .rpf archives as plain files). Default: $env:LEONIDA_GAME_DIR,
    else game_path.txt beside gta6rt.exe (written by make_game.py's runtime stage).

.PARAMETER Exe
    game_scarlett_master.exe. Default: $env:LEONIDA_EXE, else <GameDir>\game_scarlett_master.exe.

.PARAMETER Runtime
    gta6rt.exe (gta6rt_tls_host.dll must sit beside it). Default: $env:GTA6RT, else
    <repo>\runtime\build\RelWithDebInfo\gta6rt.exe.

.PARAMETER Shaders
    The translated library, <work>\shaders\spirv. Default: $env:LEONIDA_SHADERS; when neither is given the
    runtime reads shader_path.txt beside gta6rt.exe.

.PARAMETER OutDir
    Folder for logs, traces and captures. Default: $env:LEONIDA_RUNS, else $HOME\leonida-runs.

.PARAMETER Overlay
    Writable folder for everything the title writes (saves, caches). Default <OutDir>\overlay.

.PARAMETER InputScript
    LEONIDA_INPUT_SCRIPT: timed pad presses, seconds since the pad's first poll (button@seconds, comma-separated;
    buttons a b x y start back lb rb up down left right). Each press is held 300 ms; button@seconds:hold holds it
    longer, e.g. b@1100:3 for a title that reads its pad only every few seconds. Sets LEONIDA_KEYBOARD_PAD=always. Builds older than
    main @ 8146ef690 ignore a script longer than 63 characters. Default 'a@45,a@60,a@75,a@90'.

.PARAMETER NoInput
    No input script (LEONIDA_KEYBOARD_PAD then stays as this shell has it: LEONIDA_KEYBOARD_PAD=1 to play by
    hand with the window focused).

.PARAMETER Stacks
    LEONIDA_STACKS: whole seconds at which every thread's call chain is printed to the log, e.g. '30,60'.

.PARAMETER SetEnv
    Extra or overriding environment variables as a hashtable, when called from a PowerShell prompt:
    & .\scripts\windows-boot\boot.ps1 -SetEnv @{ LEONIDA_PEEK_SECONDS = '2' }. A value of '' removes the variable.

.PARAMETER EnvPairs
    Any further arguments, each KEY=VALUE, as scripts/linux-boot/run-boot.sh takes them. These work with
    powershell -File too:  ... -Name x LEONIDA_PRESENT_PLANE1=0 "LEONIDA_PEEK=<rva>,<rva>".
    Applied after -SetEnv. KEY= removes the variable for this boot.

.PARAMETER NoCapture
    Do not set LEONIDA_CAPTURE.

.PARAMETER ExtraArgs
    More gta6rt arguments, appended after the standard ones.

.PARAMETER FirstRunUi
    Launch with the offline first-run UI profile instead of the generic runtime profile.

.PARAMETER Python
    Python 3.12+ used for tools/boot_trace.py. Default: python, else py -3.

.PARAMETER SummaryLines
    How many lines of the boot_trace.py report to print. Default 40; 0 prints none.

.PARAMETER LockWaitSeconds
    How long to wait for another boot to finish. Default 3600.

.PARAMETER DryRun
    Print the environment and the command line, check the paths, launch nothing.

.PARAMETER DigestOnly
    Launch nothing; write the summary and digest again from an earlier run's files (-Name, -OutDir).

.EXAMPLE
    powershell -NoProfile -File scripts\windows-boot\boot.ps1 -DryRun -GameDir D:\GTA6pkg -Shaders E:\leonida-work\shaders\spirv

.EXAMPLE
    powershell -NoProfile -File scripts\windows-boot\boot.ps1 -Name first -Seconds 200 -GameDir D:\GTA6pkg -Shaders E:\leonida-work\shaders\spirv

.EXAMPLE
    powershell -NoProfile -File scripts\windows-boot\boot.ps1 -Name stall -Seconds 90 -Stacks 40,80 "LEONIDA_PEEK=<rva>,<rva>"

.EXAMPLE
    powershell -NoProfile -File scripts\windows-boot\boot.ps1 -DigestOnly -Name first
#>
[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$Name = "",
    [int]$Seconds = 180,
    [string]$GameDir = "",
    [string]$Exe = "",
    [string]$Runtime = "",
    [string]$Shaders = "",
    [string]$OutDir = "",
    [string]$Overlay = "",
    [string]$InputScript = "a@45,a@60,a@75,a@90",
    [string]$Stacks = "",
    [hashtable]$SetEnv = @{},
    [switch]$NoCapture,
    [switch]$NoInput,
    [string[]]$ExtraArgs = @(),
    [switch]$FirstRunUi,
    [string]$Python = "",
    [int]$SummaryLines = 40,
    [int]$LockWaitSeconds = 3600,
    [switch]$DryRun,
    [switch]$DigestOnly,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$EnvPairs = @()
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = "Stop"

$RepoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\.."))
$ExeName = "game_scarlett_master.exe"

function Get-FirstValue {
    param([object[]]$Candidates)
    foreach ($c in $Candidates) { if ($c -and ([string]$c).Trim()) { return ([string]$c).Trim() } }
    return ""
}

function Read-SideFile {
    # One line from a text file make_game.py writes beside gta6rt.exe (game_path.txt, shader_path.txt).
    param([string]$Path)
    if (Test-Path -LiteralPath $Path -PathType Leaf) {
        $line = Get-Content -LiteralPath $Path -TotalCount 1
        if ($line) { return ([string]$line).Trim() }
    }
    return ""
}

function ConvertTo-ProcessArgument {
    # Quote one argument the way the MSVC CRT splits a command line (backslashes before a quote are doubled).
    param([string]$Value)
    if ($Value -ne "" -and $Value -notmatch '[\s"]') { return $Value }
    $sb = New-Object System.Text.StringBuilder
    [void]$sb.Append('"')
    $slashes = 0
    foreach ($ch in $Value.ToCharArray()) {
        if ($ch -eq '\') { $slashes++; continue }
        if ($ch -eq '"') { [void]$sb.Append('\', 2 * $slashes + 1) } elseif ($slashes) { [void]$sb.Append('\', $slashes) }
        $slashes = 0
        [void]$sb.Append($ch)
    }
    if ($slashes) { [void]$sb.Append('\', 2 * $slashes) }
    [void]$sb.Append('"')
    return $sb.ToString()
}

function Test-InsideFolder {
    param([string]$Path, [string]$Folder)
    $p = [System.IO.Path]::GetFullPath($Path).TrimEnd('\') + '\'
    $f = [System.IO.Path]::GetFullPath($Folder).TrimEnd('\') + '\'
    return $p.StartsWith($f, [System.StringComparison]::OrdinalIgnoreCase)
}

function Resolve-Python {
    param([string]$Requested)
    if ($Requested) { return @($Requested) }
    if (Get-Command python -ErrorAction SilentlyContinue) { return @("python") }
    if (Get-Command py -ErrorAction SilentlyContinue) { return @("py", "-3") }
    return @()
}

# ------------------------------------------------------------------------------------------------ paths

if (-not $Name) { $Name = "boot-" + (Get-Date -Format "yyyyMMdd-HHmmss") }
if ($NoInput) { $InputScript = "" }
if ($Name -notmatch '^[A-Za-z0-9._-]+$') { throw "-Name may hold letters, digits, '.', '_' and '-' only: $Name" }

$Runtime = Get-FirstValue @($Runtime, $env:GTA6RT, (Join-Path $RepoRoot "runtime\build\RelWithDebInfo\gta6rt.exe"))
$RuntimeDir = Split-Path -Parent $Runtime
$GameDir = Get-FirstValue @($GameDir, $env:LEONIDA_GAME_DIR, (Read-SideFile (Join-Path $RuntimeDir "game_path.txt")))
$Exe = Get-FirstValue @($Exe, $env:LEONIDA_EXE, $(if ($GameDir) { Join-Path $GameDir $ExeName } else { "" }))
$Shaders = Get-FirstValue @($Shaders, $env:LEONIDA_SHADERS)
$OutDir = Get-FirstValue @($OutDir, $env:LEONIDA_RUNS, (Join-Path $HOME "leonida-runs"))
$OutDir = [System.IO.Path]::GetFullPath($OutDir)
$Overlay = Get-FirstValue @($Overlay, (Join-Path $OutDir "overlay"))
$Capture = Join-Path $OutDir ("cap_" + $Name)
$LogFile = Join-Path $OutDir ($Name + ".log")
$StdoutFile = Join-Path $OutDir ($Name + ".stdout.log")
$TraceFile = Join-Path $OutDir ($Name + ".jsonl")
$SummaryFile = Join-Path $OutDir ($Name + ".summary.txt")
$DigestFile = Join-Path $OutDir ($Name + ".digest.txt")

$problems = New-Object System.Collections.Generic.List[string]
if (Test-InsideFolder $OutDir $RepoRoot) { $problems.Add("-OutDir $OutDir is inside the repository; logs and captures must stay outside it") }
if (-not (Test-Path -LiteralPath $Runtime -PathType Leaf)) { $problems.Add("no gta6rt.exe at $Runtime (build it, or pass -Runtime / set GTA6RT)") }
elseif (-not (Test-Path -LiteralPath (Join-Path $RuntimeDir "gta6rt_tls_host.dll") -PathType Leaf)) {
    $problems.Add("gta6rt_tls_host.dll is missing beside $Runtime (build the gta6rt target; without it the title exits at once)")
}
if (-not $GameDir) { $problems.Add("no game folder: pass -GameDir or set LEONIDA_GAME_DIR") }
elseif (-not (Test-Path -LiteralPath (Join-Path $GameDir "MicrosoftGame.Config") -PathType Leaf)) {
    $problems.Add("$GameDir has no MicrosoftGame.Config: -GameDir is the folder with it and the .rpf archives")
}
if (-not $Exe -or -not (Test-Path -LiteralPath $Exe -PathType Leaf)) { $problems.Add("no $ExeName at '$Exe' (pass -Exe)") }
if ($Shaders) {
    if (-not (Test-Path -LiteralPath (Join-Path $Shaders "index.json") -PathType Leaf)) {
        $problems.Add("$Shaders has no index.json: -Shaders is <work>\shaders\spirv after make_game.py --stages check,shaders")
    } elseif (-not (Test-Path -LiteralPath (Join-Path $Shaders "exe\index.json") -PathType Leaf)) {
        Write-Warning "$Shaders has no exe\index.json: the title's 10 embedded shaders are missing and frames cannot draw (JOIN.md, 'The executable trap')"
    }
} elseif (-not (Read-SideFile (Join-Path $RuntimeDir "shader_path.txt"))) {
    $problems.Add("no shader library: pass -Shaders <work>\shaders\spirv or set LEONIDA_SHADERS")
}

# ------------------------------------------------------------------------------------------ environment

# Defaults: used only when this shell has not set the variable. They match the Linux harness (scripts/linux-boot/).
$defaults = [ordered]@{
    LEONIDA_D3D12                     = "vulkan"
    LEONIDA_DEVICE_TYPE               = "6"            # Series S identity; the runtime answers the ConsoleMode registry read with it
    LEONIDA_ARENA_MB                  = "2048"
    LEONIDA_PIPELINE_CACHE_BYTES      = "4294967296"
    LEONIDA_STARTUP_CACHE_CHECKPOINTS = "1"
    LEONIDA_PIPELINE_CACHE_DIR        = (Join-Path $OutDir "pipeline-cache")
    # LEONIDA_PEEK: the game-state and session-state RVAs (measured locally, not kept in the repository) when
    # LEONIDA_GAME_STATE_RVA and LEONIDA_SESSION_STATE_RVA name them; see below.
    LEONIDA_PEEK_SECONDS              = "5"
    LEONIDA_DEVICE_FAULT              = "1"            # device-loss report (VK_EXT_device_fault, checkpoints)
    LEONIDA_CAPTURE_EVERY             = "60"
    LEONIDA_CAPTURE_LIMIT             = "40"
    DISABLE_RTSS_LAYER                = "1"            # third-party Vulkan overlay layers off
    DISABLE_VULKAN_OBS_CAPTURE        = "1"
}
if ($InputScript) { $defaults["LEONIDA_KEYBOARD_PAD"] = "always" }
$gameStateRva = "$env:LEONIDA_GAME_STATE_RVA".ToLowerInvariant() -replace '^0x', ''
$sessionStateRva = "$env:LEONIDA_SESSION_STATE_RVA".ToLowerInvariant() -replace '^0x', ''
if ($gameStateRva -and $sessionStateRva) { $defaults["LEONIDA_PEEK"] = "0x$gameStateRva,0x$sessionStateRva" }
else {
    # The two RVAs were built in until they were taken out of the repository; say so rather than report "?" silently.
    Write-Warning ("LEONIDA_GAME_STATE_RVA and LEONIDA_SESSION_STATE_RVA are not both set: no LEONIDA_PEEK by default, " +
                   "so the RESULT game and session states read '?' (scripts/windows-boot/README.md)")
}
# Per-run values: always this script's (from its parameters), whatever the shell has.
$perRun = [ordered]@{
    LEONIDA_CAPTURE      = $(if ($NoCapture) { "" } else { $Capture })
    LEONIDA_INPUT_SCRIPT = $InputScript
}
if ($Shaders) { $perRun["LEONIDA_SHADERS"] = $Shaders }
if ($Stacks) { $perRun["LEONIDA_STACKS"] = $Stacks }

$bootEnv = [ordered]@{}
foreach ($k in $defaults.Keys) {
    $cur = [Environment]::GetEnvironmentVariable($k)
    if ($cur) { $bootEnv[$k] = $cur } else { $bootEnv[$k] = $defaults[$k] }
}
foreach ($k in $perRun.Keys) { $bootEnv[$k] = $perRun[$k] }
# Other LEONIDA_* switches already set in this shell are passed through as they are; they are listed below.
foreach ($item in (Get-ChildItem Env: | Where-Object { $_.Name -like "LEONIDA_*" })) {
    if (-not $bootEnv.Contains($item.Name)) { $bootEnv[$item.Name] = $item.Value }
}
foreach ($k in $SetEnv.Keys) { $bootEnv[[string]$k] = [string]$SetEnv[$k] }
foreach ($pair in $EnvPairs) {
    if ($pair -notmatch '^([A-Za-z_][A-Za-z0-9_]*)=(.*)$') { throw "not a parameter and not KEY=VALUE: '$pair' (Get-Help $PSCommandPath -Full)" }
    $bootEnv[$Matches[1]] = $Matches[2]
}
if ($bootEnv["LEONIDA_CAPTURE"]) { $Capture = $bootEnv["LEONIDA_CAPTURE"] }
# The journal `gta6rt --precompile-pipelines` fills beside gta6rt.exe (when it runs without LEONIDA_PIPELINE_CACHE_DIR)
# is read here as the runtime's read-only seed (LEONIDA_PIPELINE_JOURNAL_SEED_DIR, docs/d3d12_vulkan.md), so the pipelines
# it holds are not compiled again in the boot's own cache folder. Only when this shell or -SetEnv/KEY= did not decide, and
# when the two journals are distinct: the runtime turns its journal off for a seed that is, holds or sits in its writable
# journal (pipeline_cache_journal.h, resolve_roots). A relative cache folder is resolved by the runtime against its own
# working folder, so it gets no seed here; a name that cannot be resolved gets none either, rather than a stop.
if (-not $bootEnv.Contains("LEONIDA_PIPELINE_JOURNAL_SEED_DIR")) {
    # Windows' final path of a folder (GetFinalPathNameByHandle) sees through junctions, symbolic links, substituted
    # and mapped drives, short 8.3 names and the \\?\ and \\.\ prefixes, as the runtime's std::filesystem checks do.
    # A folder that does not exist yet is named from its deepest existing parent. Without the helper (a compile error,
    # a restricted session), the comparison falls back to the spelled paths.
    function Get-FinalFolderPath([string]$path) {
        $plain = ($path -replace '^\\\\[?.]\\UNC\\', '\\' -replace '^\\\\[?.]\\', '').TrimEnd('\', '/')
        try {
            if (-not ('LeonidaFinalPath' -as [type])) {
                Add-Type -ErrorAction Stop -TypeDefinition @'
using System; using System.Runtime.InteropServices; using System.Text; using Microsoft.Win32.SafeHandles;
public static class LeonidaFinalPath {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern SafeFileHandle CreateFileW(string name, uint access, uint share, IntPtr sa, uint disposition, uint flags, IntPtr template);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern uint GetFinalPathNameByHandleW(SafeFileHandle handle, StringBuilder buffer, uint length, uint flags);
    public static string Of(string path) {
        // No access asked, all sharing, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS (needed to open a folder).
        using (SafeFileHandle h = CreateFileW(path, 0, 7, IntPtr.Zero, 3, 0x02000000, IntPtr.Zero)) {
            if (h.IsInvalid) return null;
            StringBuilder b = new StringBuilder(32768);
            uint n = GetFinalPathNameByHandleW(h, b, (uint)b.Capacity, 0);
            return (n == 0 || n >= b.Capacity) ? null : b.ToString();
        }
    }
}
'@
            }
            $existing = $path.TrimEnd('\', '/'); $tail = @()
            while ($existing -and -not [System.IO.Directory]::Exists($existing)) {
                $tail = @([System.IO.Path]::GetFileName($existing)) + $tail
                $existing = [System.IO.Path]::GetDirectoryName($existing)
            }
            if (-not $existing) { return $plain }
            $final = [LeonidaFinalPath]::Of($existing)
            if (-not $final) { return $plain }
            $final = $final -replace '^\\\\\?\\UNC\\', '\\' -replace '^\\\\\?\\', ''
            foreach ($part in $tail) { $final = [System.IO.Path]::Combine($final, $part) }
            return $final.TrimEnd('\', '/')
        } catch { return $plain }
    }
    $seed = $null
    try {
        $runtimeFull = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($RuntimeDir)
        $precompiled = [System.IO.Path]::GetFullPath([System.IO.Path]::Combine($runtimeFull, "pipeline_cache.d")).TrimEnd('\', '/')
        if (Test-Path -LiteralPath $precompiled -PathType Container) {
            $cacheDir = $bootEnv["LEONIDA_PIPELINE_CACHE_DIR"]
            $ownJournal = $null
            if (-not $cacheDir) { $ownJournal = $precompiled }   # the runtime then keeps its journal beside gta6rt.exe
            elseif ($cacheDir -match '^([A-Za-z]:[\\/]|[\\/]{2})') {
                $ownJournal = [System.IO.Path]::GetFullPath([System.IO.Path]::Combine($cacheDir, "pipeline_cache.d")).TrimEnd('\', '/')
            }
            if ($ownJournal) {
                $a = Get-FinalFolderPath $ownJournal
                $b = Get-FinalFolderPath $precompiled
                $c = [System.StringComparison]::OrdinalIgnoreCase
                $overlap = $a.Equals($b, $c) -or $a.StartsWith($b + '\', $c) -or $b.StartsWith($a + '\', $c)
                if (-not $overlap) { $seed = $precompiled }
            }
        }
    } catch { $seed = $null }
    if ($seed) { $bootEnv["LEONIDA_PIPELINE_JOURNAL_SEED_DIR"] = $seed }
}

$launchMode = if ($FirstRunUi) { "--first-run-ui" } else { "--run" }
$gameArgs = @($launchMode, $Exe, "--game-dir", $GameDir, "--overlay", $Overlay, "--trace", $TraceFile, "--trace-d3d12") + $ExtraArgs
$argLine = ($gameArgs | ForEach-Object { ConvertTo-ProcessArgument ([string]$_) }) -join " "

if (-not $DigestOnly) {
    Write-Output "[boot] $Name  ${Seconds}s  repo $RepoRoot"
    Write-Output "[boot] gta6rt  $Runtime"
    Write-Output "[boot] out     $OutDir"
    foreach ($k in $bootEnv.Keys) { if ($bootEnv[$k] -ne "") { Write-Output ("[boot] env     {0}={1}" -f $k, $bootEnv[$k]) } }
    Write-Output "[boot] command $(ConvertTo-ProcessArgument $Runtime) $argLine"
}

if ($DigestOnly) {
    if (-not (Test-Path -LiteralPath $LogFile)) { Write-Output "[boot] no $LogFile to digest"; exit 2 }
    $problems.Clear()
}
if ($problems.Count) {
    foreach ($p in $problems) { Write-Output "[boot] problem: $p" }
    if (-not $DryRun) { exit 2 }
}
if ($DryRun) { Write-Output "[boot] dry run: nothing launched"; exit $(if ($problems.Count) { 2 } else { 0 }) }

$verdict = "digest of an earlier run (-DigestOnly)"
$exitCode = 0
if (-not $DigestOnly) {

    # --------------------------------------------------------------------------------------------- the lock

    $mutex = New-Object System.Threading.Mutex($false, "Local\LeonidaGameBoot")
    $owned = $false
    $saved = @{}
    $exitCode = 0
    try {
        $deadline = (Get-Date).AddSeconds($LockWaitSeconds)
        $said = $false
        while (-not $owned) {
            try { $owned = $mutex.WaitOne(5000) } catch [System.Threading.AbandonedMutexException] { $owned = $true }
            if ($owned -and (Get-Process -Name gta6rt -ErrorAction SilentlyContinue)) {
                # A game started by hand or by another script that does not take this lock: wait for it too.
                $mutex.ReleaseMutex(); $owned = $false; Start-Sleep -Seconds 5
            }
            if (-not $owned) {
                if (-not $said) { Write-Output "[boot] waiting: another game process is running (one at a time)"; $said = $true }
                if ((Get-Date) -gt $deadline) { Write-Output "[boot] gave up waiting after $LockWaitSeconds s"; exit 3 }
            }
        }

        foreach ($d in @($OutDir, $Overlay)) { New-Item -ItemType Directory -Force -Path $d | Out-Null }
        if ($bootEnv["LEONIDA_PIPELINE_CACHE_DIR"]) {
            New-Item -ItemType Directory -Force -Path $bootEnv["LEONIDA_PIPELINE_CACHE_DIR"] | Out-Null
        }
        if ($bootEnv["LEONIDA_CAPTURE"]) {
            # A capture folder reused by the same -Name: drop the earlier run's frames and index, nothing else.
            New-Item -ItemType Directory -Force -Path $Capture | Out-Null
            Get-ChildItem -LiteralPath $Capture -File | Where-Object { $_.Name -like "frame_*.ppm" -or $_.Name -like "*index.txt" } |
                Remove-Item -Force -Confirm:$false
        }
        foreach ($f in @($LogFile, $StdoutFile, $TraceFile, $SummaryFile, $DigestFile)) {
            if (Test-Path -LiteralPath $f) { Remove-Item -LiteralPath $f -Force -Confirm:$false }
        }

        foreach ($k in $bootEnv.Keys) {
            $saved[$k] = [Environment]::GetEnvironmentVariable($k)
            $v = $bootEnv[$k]; if ($v -eq "") { $v = $null }
            [Environment]::SetEnvironmentVariable($k, $v)
        }

        # ------------------------------------------------------------------------------------------- the boot
        $started = Get-Date
        $proc = Start-Process -FilePath $Runtime -ArgumentList $argLine -WorkingDirectory $RepoRoot -NoNewWindow -PassThru `
            -RedirectStandardError $LogFile -RedirectStandardOutput $StdoutFile
        $handle = $proc.Handle   # keeps the exit code readable after the process ends
        $stopped = $false
        if (-not $proc.WaitForExit($Seconds * 1000)) {
            $stopped = $true
            try { Stop-Process -Id $proc.Id -Force -Confirm:$false } catch { }
            [void]$proc.WaitForExit(20000)
        }
        $ran = [int]((Get-Date) - $started).TotalSeconds
        $rc = $null
        try { $rc = $proc.ExitCode } catch { }
        if ($stopped) { $verdict = "ran its full time limit ($ran s), stopped"; $exitCode = 0 }
        elseif ($rc -eq $null) { $verdict = "exited by itself after $ran s, exit code unknown"; $exitCode = 1 }
        else { $verdict = ("exited by itself after {0} s, exit code {1} (0x{2:x8})" -f $ran, $rc, ([int64]$rc -band 0xffffffffL)); $exitCode = [int]$rc }
    } finally {
        foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) }
        if ($owned) { $mutex.ReleaseMutex() }
        $mutex.Dispose()
    }
}

# -------------------------------------------------------------------------------------------- the digest

$py = @(Resolve-Python $Python)
if ($py.Count) {
    $pyArgs = @()
    if ($py.Count -gt 1) { $pyArgs += $py[1..($py.Count - 1)] }
    $pyArgs += @("-I", (Join-Path $RepoRoot "tools\boot_trace.py"), $TraceFile, "--d3d12", $LogFile)
    # Windows PowerShell 5.1 turns a native program's stderr into error records; with "Stop" that would end the script.
    $ErrorActionPreference = "Continue"
    & $py[0] @pyArgs 2>&1 | ForEach-Object { "$_" } | Out-File -FilePath $SummaryFile -Encoding utf8
    $ErrorActionPreference = "Stop"
} else {
    "no python found for tools/boot_trace.py" | Out-File -FilePath $SummaryFile -Encoding utf8
}

$peekRx = [regex]'\[rt\] peek t=(\d+)s:(.*)$'
$valRx = [regex]'0x([0-9a-f]+)=([0-9a-f?]+)'
$states = New-Object System.Collections.Generic.List[string]
$lastPeek = ""; $prevPair = ""
$presentOk = 0; $presentFailed = 0; $presses = 0
$shaderLine = ""; $faults = New-Object System.Collections.Generic.List[string]
$wrapped = 0
if (Test-Path -LiteralPath $LogFile) {
    foreach ($line in [System.IO.File]::ReadLines($LogFile)) {
        if ($line.Contains("XboxPresentation::PresentX")) {
            if ($line.EndsWith(" ok")) { $presentOk++ } elseif ($line.EndsWith(" failed")) { $presentFailed++ }
            continue
        }
        if ($line.Contains("[rt] peek t=")) {
            $m = $peekRx.Match($line)
            if ($m.Success) {
                $lastPeek = $line.Substring($line.IndexOf("[rt] peek"))
                $game = "?"; $session = "?"
                foreach ($v in $valRx.Matches($m.Groups[2].Value)) {
                    $rva = $v.Groups[1].Value; $val = $v.Groups[2].Value
                    if ($gameStateRva -and $rva -eq $gameStateRva) { $game = $val }
                    elseif ($sessionStateRva -and $rva -eq $sessionStateRva -and $val -ne "?") {
                        $low = $val.PadLeft(16, '0').Substring(8).TrimStart('0')   # the session state is the low dword
                        $session = $(if ($low) { $low } else { "0" })
                    }
                }
                $pair = "game=$game session=$session"
                if ($pair -ne $prevPair) { $states.Add("t=$($m.Groups[1].Value)s $pair"); $prevPair = $pair }
            }
            continue
        }
        if ($line.Contains("[rt] gameinput: press ")) {
            # Buttons only: with the keyboard pad, mouse movement is logged as right-stick presses (rsup, rsdown...).
            if ($line -notmatch 'gameinput: press [lr]s') { $presses++ }
            continue
        }
        if ($line.Contains("[rt] device-fault:") -and -not $line.Contains("device-fault: enabled")) { if ($faults.Count -lt 8) { $faults.Add($line.Trim()) }; continue }
        if ($line.Contains("VK_ERROR_DEVICE_LOST") -and $faults.Count -lt 8) { $faults.Add($line.Trim()); continue }
        if (-not $shaderLine -and $line.Contains(" translated shaders (")) { $shaderLine = $line.Substring([Math]::Max(0, $line.IndexOf("[rt]"))).Trim() }
        if ($line.Contains("wrapped V#")) { $wrapped++ }
    }
}

$capText = "no captures"
$capIndex = Join-Path $Capture "index.txt"
if (Test-Path -LiteralPath $capIndex) {
    $frames = 0; $nonBlack = 0; $last = ""; $maxSum = -1.0; $maxText = ""
    $meanRx = [regex]'mean rgb ([0-9.]+) ([0-9.]+) ([0-9.]+)'
    foreach ($line in [System.IO.File]::ReadLines($capIndex)) {
        $m = $meanRx.Match($line)
        if (-not $m.Success) { continue }
        $frames++
        $r = [double]::Parse($m.Groups[1].Value, [Globalization.CultureInfo]::InvariantCulture)
        $g = [double]::Parse($m.Groups[2].Value, [Globalization.CultureInfo]::InvariantCulture)
        $b = [double]::Parse($m.Groups[3].Value, [Globalization.CultureInfo]::InvariantCulture)
        $sum = $r + $g + $b
        if ($sum -gt 0) { $nonBlack++ }
        $last = "$($m.Groups[1].Value) $($m.Groups[2].Value) $($m.Groups[3].Value)"
        if ($sum -gt $maxSum) { $maxSum = $sum; $maxText = $last }
    }
    $capText = "$frames captured, $nonBlack non-black; last mean rgb $last; brightest $maxText"
}

$digest = New-Object System.Collections.Generic.List[string]
$digest.Add("== boot ${Name}: $verdict")
$digest.Add("shaders:  $(if ($shaderLine) { $shaderLine } else { 'no translated-shaders line (Vulkan device not reached?)' })")
$digest.Add("states:   $(if ($states.Count) { $states -join ' | ' } else { 'no peek lines' })")
$digest.Add("presents: PresentX ok $presentOk, failed $presentFailed")
$digest.Add("input:    $presses presses logged")
$digest.Add("faults:   $(if ($faults.Count) { $faults.Count.ToString() + ' device-fault lines' } else { 'none' })")
foreach ($f in $faults) { $digest.Add("          $f") }
if ($wrapped) { $digest.Add("wrapped:  $wrapped lines mention wrapped V#s") }
$digest.Add("frames:   $capText")
$digest.Add("files:    $LogFile, $TraceFile, $SummaryFile")
$digest | Out-File -FilePath $DigestFile -Encoding utf8
$digest
if ($SummaryLines -gt 0 -and (Test-Path -LiteralPath $SummaryFile)) {
    Write-Output "-- boot_trace.py (first $SummaryLines lines of $SummaryFile)"
    Get-Content -LiteralPath $SummaryFile -TotalCount $SummaryLines
}
exit $exitCode
