# Native Windows boot harness

`boot.ps1` boots the retail executable under `gta6rt` on native Windows the way the Linux harness
([`scripts/linux-boot/`](../linux-boot/)) does under Proton: same switches, one game process at a time, a trace of
every boot and a short digest at the end. Windows PowerShell 5.1 is enough. New here? the repository README
covers the setup that comes before this.

## Run it

```powershell
# What would run, with every path checked; launches nothing.
powershell -NoProfile -File scripts\windows-boot\boot.ps1 -DryRun -GameDir D:\GTA6pkg -Shaders E:\leonida-work\shaders\spirv

# A 200 s boot with the default unattended presses (a@45,a@60,a@75,a@90).
powershell -NoProfile -File scripts\windows-boot\boot.ps1 -Name first -Seconds 200 -GameDir D:\GTA6pkg -Shaders E:\leonida-work\shaders\spirv

# Extra or changed switches: trailing KEY=VALUE arguments (KEY= removes one), as run-boot.sh takes them.
powershell -NoProfile -File scripts\windows-boot\boot.ps1 -Name split -Seconds 120 LEONIDA_PRESENT_PLANE1=0 "LEONIDA_PEEK=<rva>,<rva>,<rva>"

# Every parameter.
Get-Help -Full .\scripts\windows-boot\boot.ps1
```

Set `LEONIDA_GAME_DIR`, `LEONIDA_SHADERS` (and `GTA6RT`, `LEONIDA_RUNS` if you use other places) once in your
profile and the parameters can be left out. Defaults: `gta6rt` from `runtime\build\RelWithDebInfo\` of this
checkout, the executable from the game folder (`-Exe` or `LEONIDA_EXE` when it lives elsewhere), outputs in `$HOME\leonida-runs`.
The script refuses an output folder inside the repository.

## What it sets

Only when your shell has not set them already (`-DryRun` prints the final list):

| Variable | Default | Why |
|---|---|---|
| `LEONIDA_D3D12` | `vulkan` | the runtime's D3D12-on-Vulkan backend |
| `LEONIDA_DEVICE_TYPE` | `6` | Series S identity; the runtime answers the title's `ConsoleMode` registry read with it (1920x1080) |
| `LEONIDA_ARENA_MB` | `2048` | as on Linux |
| `LEONIDA_PIPELINE_CACHE_BYTES` | `4294967296` | pipeline cache size limit |
| `LEONIDA_STARTUP_CACHE_CHECKPOINTS` | `1` | save the pipeline cache during startup |
| `LEONIDA_PIPELINE_CACHE_DIR` | `<out>\pipeline-cache` | one warmed cache shared by every build you boot |
| `LEONIDA_PIPELINE_JOURNAL_SEED_DIR` | `pipeline_cache.d` beside `gta6rt.exe`, when it exists | the journal `gta6rt --precompile-pipelines` fills there when it runs without `LEONIDA_PIPELINE_CACHE_DIR`, read-only, so its pipelines are not compiled again. Not set when the boot's own journal (`<cache folder>\pipeline_cache.d`, or beside `gta6rt.exe` when the cache folder is empty) is that folder or one holds the other, compared through Windows' final paths (so a junction, symbolic link, substituted or mapped drive, 8.3 name or `\\?\` / `\\.\` prefix counts as the same folder), or when the cache folder is a relative path. `LEONIDA_PIPELINE_JOURNAL_SEED_DIR=` (empty) turns the seed off. If the runtime still logs `[rt] d3d12: invalid or overlapping compute journal directories; journal disabled`, set it so. On NVIDIA, run the pre-compile with the same `LEONIDA_DEVICE_FAULT` as the boot (this script sets `1`): the diagnostics config is part of the journal's file names |
| `LEONIDA_PEEK` / `_SECONDS` | the game-state and session-state RVAs from `LEONIDA_GAME_STATE_RVA` and `LEONIDA_SESSION_STATE_RVA` (measured locally, not kept here; unset: no peek) / `5` | game state and session state (low dword), the two numbers every RESULT reports |
| `LEONIDA_DEVICE_FAULT` | `1` | on device loss: `VK_EXT_device_fault` info, checkpoints, last replay steps |
| `LEONIDA_KEYBOARD_PAD` | `always` (with an input script) | keyboard as a pad even unfocused, so the script's presses arrive |
| `LEONIDA_CAPTURE_EVERY` / `_LIMIT` | `60` / `40` | a swapchain read-back every 60 presents, at most 40 |
| `DISABLE_RTSS_LAYER`, `DISABLE_VULKAN_OBS_CAPTURE` | `1` | keep third-party Vulkan overlay layers out of the process |

Per run, from the parameters: `LEONIDA_SHADERS` (`-Shaders`), `LEONIDA_CAPTURE` (`<out>\cap_<name>`, `-NoCapture`
turns it off), `LEONIDA_INPUT_SCRIPT` (`-InputScript`, `-NoInput` for none), `LEONIDA_STACKS` (`-Stacks 30,60`: every
thread's call chain at those seconds, unwound in-process, which works natively where `tools/re/mdmp_unwind.py`
cannot walk Windows' own DLLs). Other `LEONIDA_*` variables in your shell pass through and are listed.

The command is `gta6rt --run <exe> --game-dir <game> --overlay <out>\overlay --trace <name>.jsonl --trace-d3d12`.
The overlay takes everything the title writes, so the package folder can stay read-only.

## What you get

In the output folder: `<name>.log` (stderr: `[rt]` lines and the D3D12 trace), `<name>.stdout.log`,
`<name>.jsonl` (startup trace), `<name>.summary.txt` (`tools/boot_trace.py`), `<name>.digest.txt` and
`cap_<name>\` (PPM frames plus `index.txt` with the mean RGB of each). The digest:

```
== boot first: ran its full time limit (200 s), stopped
shaders:  [rt] d3d12: 26586 translated shaders (21 fspx, 0 fspx_compute, 0 fspx_gfx, 52 bvh, 10 exe) from ...
states:   t=0s game=0 session=0 | t=15s game=2 session=0 | t=30s game=3 session=3 | t=60s game=3 session=d
presents: PresentX ok 1581, failed 0
input:    13 presses logged
faults:   none
frames:   14 captured, 0 non-black; last mean rgb 0.00 0.00 0.00; brightest 0.00 0.00 0.00
```

`-DigestOnly -Name <name>` writes the summary and digest again for an earlier run. The `faults` row reports whether
the log recorded device loss and counts Vulkan address records by type (for example, `WRITE_INVALID=2`).
Several records can describe one loss. A driver can also report loss without any address records.
Share counts and states on the board. Keep paths, logs, traces, captures and dumps on your machine
([AGENTS.md](../../AGENTS.md), "No game data").

How to read the states: game state goes 1, 2 (init), 3 (running); session state goes 0, 1, 3 (landing
screen waiting for A), `d` after A.

**Changed: the states need two variables now.** The script used to set `LEONIDA_PEEK` to the game-state and
session-state RVAs by default. Those addresses of the retail executable are no longer kept in the repository, so set
`LEONIDA_GAME_STATE_RVA` and `LEONIDA_SESSION_STATE_RVA` (hex, measured locally) in the shell before the boot.
Without them the script warns, sets no `LEONIDA_PEEK`, and the `states:` line and the RESULT show `game=?
session=?`. A `LEONIDA_PEEK` of your own still works for the log, but the digest reads only the two RVAs those
variables name. `docs/linux_proton.md` (the same stages apply natively)
section 2 has the healthy sequence with times and section 3 "which blank do you have".

To pair scripted presses with state changes after a run, use `python -I tools/stage_timeline.py LOG` with the same
two environment variables. For a saved log, `--game-state-rva` and `--session-state-rva` override them. If the log
also records the loading-screen state, pass its hex RVA with `--load-state-rva` to see when the screen ended.

## One game at a time

The script holds the named mutex `Local\LeonidaGameBoot` for the whole boot and also waits while any `gta6rt.exe`
runs (another agent's boot, or one started by hand), up to `-LockWaitSeconds` (default an hour, then exit 3). Two
game processes on one GPU make both runs meaningless and can take the driver down.

## Helper tools (`tools/re/windows/`)

| Tool | What for |
|---|---|
| `sym.py IMAGE RVA...` | `gta6rt.exe+0x...` frames to function and line through dbghelp and the build's PDB (the Windows SDK's or Visual Studio's `dbghelp.dll`; `LEONIDA_DBGHELP` names another) |
| `ripsample.ps1 [-Seconds 20]` | sampling profiler of a running `gta6rt`: per thread, where it spends its time, as module+RVA (`TITLE+0x...` for the title) |
| `minidump.py PID OUT.dmp` | a minidump through dbghelp's `MiniDumpWriteDump`. Do not use `rundll32 comsvcs.dll MiniDump`: Defender flags it. A dump holds game memory: local only |

Each prints its usage with `-h` (Python) or `Get-Help` (PowerShell).

## Recover a stack from a manually mapped image

When the exception stack stops inside the mapped executable, run `mapped_stack.py` on the existing
private dump. Supply the exact PE from the crashing build and an x64 CDB from Debugging Tools for
Windows. If CDB is on your PATH, omit `--cdb`. The optional `--symbols` takes one local directory
containing the matching runtime images and PDBs.

```powershell
py -3 -I scripts\windows-boot\mapped_stack.py D:\runs\fault\crash.dmp D:\package\title.exe --symbols D:\build\RelWithDebInfo --cdb "C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe"
```

The command derives the mapping base, including a relocated base, by locating a unique captured
instruction window in an executable section of the supplied PE. It checks for a file-backed x64
unwind entry at the exception RIP and compares captured bytes from the first header page and that
function's unwind metadata when available.
A small dump can establish a local code match only. Use the original image even if another build
has the same function. Exit-only snapshots, faults outside the supplied image, leaf functions
without unwind entries, and missing, patched or ambiguous instruction windows cannot establish
this stack.

CDB reads the exception context and uses the disk image's unwind data to recover callers. The
script makes a temporary copy of the PE beside the dump under the debugger alias `leonida_mapped`.
It disables network symbols and writes private frame rows to a new `mapped-stack.txt` beside the
dump. Use `--output` for a different new file. A successful walk recovers at least one caller and
can stop when later stack memory or module information is missing. Keep dumps and reports outside
the checkout and share only the outcome and frame count.

Run the synthetic regression tests with `py -3 -I tests\test_mapped_stack.py`. To include the
native CDB walk, put x64 CDB on PATH or set `LEONIDA_TEST_CDB` to its full path.
