# Mods

**Status: the mod format, loader and hook are in place; only the lookup's address in the executable is missing.** `gta6rt` reads every
`mods/<folder>/mod.json`, checks it and logs what it would change. A swap takes effect once the game's
archetype lookup is hooked, and that needs the executable (RT-5): its address differs per version.

A mod never contains game data. It names models the game already has, by the names (or `joaat` hashes)
the game uses, and says what to swap. Everything stays on your own PC, like the rest of the build.

## Where mods go

`gta6rt` looks in a `mods` folder next to `gta6rt.exe` (the port's folder, `Documents\Leonida` by
default), or in the folder `--mods DIR` names. Each mod is a folder with a `mod.json`; a folder without
one is ignored. Folders load in name order, and a later one wins when two swap the same model.

```text
Documents\Leonida\
  gta6rt.exe
  mods\
    mandy_prison_intro\
      mod.json
```

At start the runtime prints what it loaded, for example:

```text
[rt] mods: Mandy in the prison intro
[rt] mods:   archetype player_story000_00 (0xE472B687) -> cs_mandy (0xD7C0CDFF)
[rt] mods: 1 archetype swap(s) loaded; not applied yet: the archetype lookup hook for this executable is not known
```

## Managing them (`tools/leonida_mods.py`)

`tools/leonida_mods.py` is the small mod manager. It writes only in the port's `mods` folder: it can create,
install, list, check, enable, disable, pack and remove mods. A package is a normal ZIP file (the `.leonidamod`
extension is only a name) with `mod.json` at its top level or inside one top-level folder. The installer refuses
paths that climb out of the package, links and manifests the runtime would reject; it never goes online and never
copies game data. `list` and `check` read each `mod.json` by the runtime's rules, the `camera` section and a
byte-order mark included, and show the camera a mod sets next to its swaps.

On Windows, double-click **`Mods.bat`** for the Tkinter window (on Linux and macOS, run **`./Mods.sh`**; it needs
Tkinter, `python3-tk` on Debian and Ubuntu): choose the port folder, install a ZIP or folder,
toggle a mod, create a swap, pack or remove one, and launch `gta6rt` normally or as a dry run. The GUI uses the same
standard-library code as the CLI below.

Point `--port` at the built port (the folder with `gta6rt.exe` and `game_path.txt`), or `--dir` at a mods folder:

```powershell
python -I tools/leonida_mods.py gui
python -I tools/leonida_mods.py --port "D:\Games\Leonida" list
python -I tools/leonida_mods.py --port "D:\Games\Leonida" install .\mandy.zip
python -I tools/leonida_mods.py --port "D:\Games\Leonida" new lucia_to_mandy `
    --name "Mandy" --game-version 1.0.2.1 --swap player_story000_00=cs_mandy
python -I tools/leonida_mods.py --port "D:\Games\Leonida" disable mandy_prison_intro
python -I tools/leonida_mods.py --dir "D:\Games\Leonida\mods" check
```

`pack` writes an installed folder back to a ZIP for sharing; `uninstall` removes it. A package carries any file name
that Windows and Linux both keep as written, whatever its length or the spaces inside it; `pack` stops at one that
is not (a device name such as `con.txt`, a trailing dot, a `:`), since `install` would refuse it. `run` is a convenience
launcher for the same options `gta6rt` already takes:

```powershell
python -I tools/leonida_mods.py --port "D:\Games\Leonida" run --archetype-lookup 1A2B3C0
```

Without the measured `--archetype-lookup` RVA the runtime still loads and reports the manifests but the swaps are
not applied, as above. The game executable itself is still needed; the manager does not supply it.

The repository's `mods/` folder has four manifest-only examples. `mandy_prison_intro` is enabled; the other three
are installed disabled so a copied folder cannot surprise anyone or silently fight it:

| Folder | Swap | State |
|---|---|---|
| `mandy_prison_intro` | `player_story000_00` -> `cs_mandy` | enabled |
| `lucia_cutscene_model` | `player_story000_00` -> `cs_lucia` | disabled |
| `monalisa_player_model` | `player_story000_00` -> `cs_monalisa` | disabled |
| `mandy_to_monalisa` | `cs_mandy` -> `cs_monalisa` | disabled |

## mod.json

```json
{
  "name": "Mandy in the prison intro",
  "enabled": true,
  "game_version": "1.0.2.1",
  "archetype_swaps": [
    { "from": "player_story000_00", "to": "cs_mandy", "when": "always" }
  ]
}
```

| Key | Meaning |
|---|---|
| `name` | Shown in the log and the launcher. Defaults to the folder name. |
| `enabled` | `false` keeps the mod installed but off. |
| `game_version` | The package version the mod was made for. |
| `archetype_swaps` | Each entry: when the game asks for the model `from`, it gets `to`. |

`from` and `to` are model names, hashed with `joaat` (lower case, as the game does), or 32-bit hashes
written `0x…`. A swap is one step: a model swapped to another that is itself swapped is not followed
again, so two mods cannot loop. `when` is `always`; a narrower condition (one mission, one cutscene) is
refused until a hook provides the game's mission state, so a mod never silently does more than it says.

`mod.json` is UTF-8. A byte-order mark in front is accepted (Notepad's "UTF-8 with BOM", Windows
PowerShell 5.1's `-Encoding utf8`). A UTF-16 file, which PowerShell 5.1's `>` writes, is skipped with
`mod.json is UTF-16; save it as UTF-8`.

The format is in `runtime/src/mods.h`; `runtime/test/mods_test.cpp` (ctest `mods`) covers it.

## The Mandy mod (`mods/mandy_prison_intro`)

Lucia's player model is `player_story000_00` (model id `0xE472B687`); Mandy is `cs_mandy`
(`0xD7C0CDFF`). Both ids are measured in the packs tables ([character_skeleton.md](character_skeleton.md),
"The Mandy search") and are the `joaat` of those names. Mandy has no drawable of her own: `cs_mandy`
uses the same shared female body as Lucia, and her look comes from her own nine-entry variation table.
Swapping the archetype therefore hands the game Mandy's model info, variation table included, rather
than pasting a mesh over Lucia; the game assembles her as it would in her own scenes.

What is not known yet, and decides how far the swap reaches:

- **The intro.** The prison intro's outfit is a component variation on `player_story000_00`. Which
  mission or cutscene id the intro is, and whether its outfit components exist on `cs_mandy`, is not
  measured; with `when: always` Mandy replaces Lucia everywhere the player model is asked for.
- **Mandy's components.** None of the 18 hashes in her variation table resolves to a name, so her
  head, hair and outfit parts are only reachable through her own model info, not by name.

## Day one: finding the hook

The swap needs one function in the executable: the archetype lookup by name hash (RAGE's
`fwArchetypeManager::GetArchetypeFromHashKey`). `scripts/first_look.ps1` already ranks candidates with
`tools/find_hash_lookup.py` (report `hash_lookup.json`): it finds where the code loads `0xE472B687` and
`0xD7C0CDFF`, takes the functions called right after, and orders them by how many direct callers the
whole image has. Run it alone with `python -I tools/find_hash_lookup.py <exe>`. What it automates:

1. **Constant search.** The player model id `0xE472B687` should appear as an immediate in the code that
   sets up the story player. Its callers pass it to the lookup (or to a function that does).
2. **Shape.** The lookup takes a 32-bit hash (and an optional out-index), hashes into a bucket table
   and walks a chain comparing a 32-bit key; it is called from hundreds of sites.
3. **Cross-check.** Called with `0xD7C0CDFF` it must return a valid archetype too (`cs_mandy` is a live
   model id: a `story000.rpf` cutscene table uses it).

Then pass it to the runtime:

```powershell
gta6rt.exe --dry-run game_scarlett_master.exe --archetype-lookup 1A2B3C0
```

`--archetype-lookup RVA[,ARG]` (hex RVA; `ARG` 0 when the hash is the first argument, 1 when it is the
second, as in a member function) hooks the function right after the image is mapped, before any of its
code runs (`runtime/src/code_hooks.h`). It does not patch the function: every direct `call` and `jmp`
to it in the image's code is pointed at a thunk that passes the hash through
`leonida::mods::active().remap()` and continues into the original with every other argument as it was.
The log line reports how many call sites were redirected, so a `--dry-run` with a candidate RVA shows at
once whether it is the lookup (hundreds of callers) or not (a handful, or none). ctest `code_hooks` runs
the thunk on small x64 functions in memory.

That RVA is the only version-specific piece; it can go in `game/xbox-1.0.2.1.json` once confirmed, and
the manifests stay as they are.

## First-person view (`mods/first_person`)

```json
{ "camera": { "view_mode": "first_person", "first_person_value": 4, "contexts": [0] } }
```

`camera.view_mode` is `first_person` or `default` (leave the game's choice). The game picks a view mode per
camera context (on foot, in a vehicle, ...) with a getter that takes the context and returns an enum; the
mod answers `first_person_value` for the listed `contexts` (every context when the list is left out) and
the game's own answer otherwise. When several enabled mods have a `camera` section, the last folder wins.

**Evidence that the engine has a first-person view.** The camera metadata classes GTA V uses
(`camFirstPersonShooterCameraMetadata`, `camFollowPedCameraMetadata`, ...) do not appear by `joaat` in the
carved `common`, `data` or `script` streams, but the game's own tuning data refers to a first-person view
in several systems: a ladder/mount tunable `FirstPersonDirectionDot`, a `UseFirstPersonCameraMatrix` flag on
every cloth-collision capsule (horse saddle and satchel capsules among them, so the code is RDR2's branch of
the engine, which shipped a first-person mode), a hit-reaction `RecoveryDirection` of `FIRST_PERSON_VIEW`,
and a `firstpersonshooter_hud` UI texture. So the mod switches on a mode the engine has rather than building
a camera. Whether the game exposes it to the player is not known.

**ASSUMPTION:** three things to confirm in the executable: that the view mode is chosen through one getter of the form
`mode = get(context)`, that context 0 is on foot, and that first person is enum value 4 (GTA V's
`camViewModeSettings` order: third person near, medium, far, cinematic, first person). All three are
settings in `mod.json`, so a different answer changes the manifest, not the runtime.

**Day one:** find the getter, then run `gta6rt --dry-run <exe> --view-mode-getter RVA` and check the log
line's count of redirected calls. Leads: the code that reads the view-mode input (GTA V's
`INPUT_NEXT_CAMERA`, an action hash), the switch over the five view-mode values that selects a camera
metadata per mode, and the callers of the `UseFirstPersonCameraMatrix` check in the cloth code (they ask
whether the active view is first person). The hook (`hook_image_results` in `runtime/src/code_hooks.h`) runs
the original and overrides its result; it is for getters with at most four register arguments and an
integer result. ctest `code_hooks` and `mods` cover the thunk and the override.
