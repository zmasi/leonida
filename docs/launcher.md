# The Leonida Launcher

A desktop window for the port, in the usual shape of a game launcher: a narrow navigation column on the left, a hero
area with a big PLAY button, a status line and pages. It is Leonida's own design: no other company's name, logo, font
or artwork, and no game assets in the repository. The hero shows the newest screenshot you have taken (read at run
time from the runtime's `screenshots` folder next to `leonida.exe`, else `%LOCALAPPDATA%\Leonida\captures`), or a
generated gradient. It is not an official product.

## Starting it

- Windows: double-click `Launcher.bat` at the repository root (it finds Python 3.12 or newer, like `Mods.bat`).
- Linux: `python3 tools/leonida_launcher.py` (needs Tkinter: the `python3-tk` package).

Standard library only (`tools/leonida_launcher.py`).

## Pages

- **Play**: PLAY runs `powershell -NoProfile -ExecutionPolicy Bypass -File scripts\play-d3d12.ps1 -Game <folder>
  -Data <folder>` plus the switches chosen in Settings (on Linux `scripts/play-linux.sh <folder>/game_scarlett_master.exe`,
  with `--series-x` if chosen). The script's output goes into the log box. The state line reads a few of the script's
  own messages: Starting, Building the runtime, Preparing shaders, Running, Exited with code N (a guess from the text;
  the log is the truth). Only one thing runs at a time: while it runs PLAY is replaced by Stop, which, after a
  confirmation, ends the process and everything it started (`taskkill /T /F`; on Linux the process group).
- **Settings**: the game folder (a picker, and a check that `game_scarlett_master.exe` is in it), the data folder
  (empty = the script's default), the mods folder, and the options of `play-d3d12.ps1`: DLSS (off is `-NoDlss`),
  `-RayTracing`, `-SmoothMotion`, shader preparation (off is `-NoPrecompile`), `-SeriesX`, `-Quality`, `-Resolution`
  (default = none), `-Capture`, `-Log`. On Linux only the game folder and Series X are used.
- **Mods**: the `mods\*\mod.json` of the mods folder (this repository's `mods` unless changed), with the enabled flag
  written back as the mod manager does it (`tools/leonida_mods.py`); a button opens that manager.
- **Saves**: the D3D12 runtime's saves under the data folder, `<data>\data\7952987A\saves\<user id>\<configuration>\<Name>~<FNV-1a 64
  hex>\v<N>\{0, 1, manifest}` (`d3d12/crates/leonida-hle/src/modules/xgamesave.rs`; `tools/leonida_saves.py`). Containers and
  their blobs are listed with size, modified time and the newest readable version. Actions: back up one container or
  all (a ZIP in `<data>\save-backups`), restore from a backup (confirmation, and the current saves are backed up
  first), duplicate a container under a new name, delete (the folder is moved to `save-backups\deleted`, not erased),
  export and import a single blob, and a read-only hex and strings viewer. All of them are refused while the game
  is running.
- **Tools**: fetch NVIDIA's DLSS files (`d3d12\Get-NVIDIA-DLSS.cmd`, after a confirmation: about 70 MB from NVIDIA's
  GitHub), rebuild the runtime (`d3d12\scripts\build-nvidia.ps1`), open the logs folder
  (`%LOCALAPPDATA%\Leonida\logs`) and the screenshots folder. The two scripts are Windows only.
- **About**: what the port is, that it runs your own copy, the credits (see `d3d12/README.md`) and the licence.

## Settings file

`%APPDATA%\Leonida\launcher.json` (Linux: `~/.config/leonida/launcher.json`), written when you press Save or close the
window. Unknown or mistyped keys are ignored.

## Tests

`python -I tests/test_leonida_saves.py` does the same for the Saves page on fake save trees. `python -I tests/test_leonida_launcher.py` covers the parts without a window: reading and writing the settings, the
command line each option produces (checked against the switch names in `scripts/play-d3d12.ps1`), the mod list and
the newest-picture search.

## Saves: what the bytes are, and what is not edited

- **Layout.** A container's folder is its name with every byte but letters, digits, `-` and `_` written `%XX` (cut at
  100 characters), `~`, and the 64-bit FNV-1a of the exact name in 16 hex digits. Inside, the highest `v<N>` whose
  `manifest` parses is the container; the manifest is u32-LE-length-prefixed strings: container name, display name,
  then the blob names, and blob `i` is the file `i`. The runtime writes a new version as `v<N>.new` and one rename
  commits it, and drops older versions the next time it opens the container. Duplicate therefore writes a folder
  with the new name's hash and a manifest naming it; any change to a blob is written the same way, as a new `v<N+1>`
  beside the old one, after a backup.
- **Blobs.** Every container measured has two: `Save Game Data` and `Save Game Header` (296 bytes). `Save Game Data`
  is an `ASAV` file ([saves.md](saves.md)). The profile and settings containers are plain ASAV (mode 1: the body is
  the record or hash-keyed JSON text, entropy 0.4 to 4.6 bits per byte). The story-save containers are mode 3 and their
  body has an entropy of 8.00: compressed or encrypted, and no format note in `docs/` names an algorithm for it.
- **Editing.** The launcher shows a packed save as "this save is packed (compressed or encrypted); editing is not
  supported" and does not try to decode, decompress or work around it. For the plain ones the ASAV header carries a
  checksum at 0x160 whose algorithm is open (docs/saves.md), and the header blob repeats the size, so the launcher
  does not edit values: that would write saves the title may reject, on the strength of a guess. What is offered is
  replacing a whole blob with a file you changed yourself (export, change, import), with the backup and the new
  version described above; a changed size no longer matches the header blob, which the page warns about.
- Nothing from a real save is in the repository: `tests/test_leonida_saves.py` builds its own fake trees.
