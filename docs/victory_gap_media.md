# VIctory against Leonida: media and input

Every export or method VIctory serves in its media and input modules
(`victory/crates/victory-hle/src/modules/`: `apu.rs`, `audio.rs`, `audio/capture.rs`, `dsp.rs`, `media.rs`,
`speech.rs`, `gameinput.rs`, `gameinput/dualsense.rs`, `gameinput/movement.rs`), set against what Leonida did with it,
and what this branch (`port/gap-media`) changed. VIctory is GPL-3.0-or-later, by Marvisak and contributors (DexNom
fork); every port names the VIctory file it comes from. Nothing here was checked against the game: no game files were
read or run, and every test is synthetic.

Status: **same** (Leonida already matched VIctory), **ported** (on this branch), **left** (with the reason).

## Audio decoder: `xapu.dll` (`apu.rs` / `runtime/shims/xapu.cpp`)

| Export | VIctory | Leonida before | Status |
|---|---|---|---|
| `XApuConnect` | Opus decoding only; other kinds `E_NOTIMPL` | same layouts, limits and refusals | same |
| `XApuDisconnect` | frees memory and event | same | same |
| `XApuEnqueueCommand` | activate, process, notify, reset, deactivate; decodes at once | same queueing and results | same |
| process command | Opus decoded to float PCM (`opus_pure`) | packet parsed (`src/opus_packet.h`): frame counts, lengths and errors are the packet's, samples are silence | **left**: Leonida has no Opus decoder. VIctory's comes from the `opus-pure` crate, which is not in this repository; a port is a full SILK/CELT decoder. Speech and Opus-coded audio stay silent, nothing waits on it |
| `XApuDequeueResult`, `XApuGetResultCount` | result queue | same | same |

## Audio DSP: `chad.dll` (`dsp.rs` / `runtime/shims/chad_dsp.cpp`)

| Export | VIctory | Leonida before | Status |
|---|---|---|---|
| `XDspConnectWithMaximumStreamLimit`, `XDspDisconnect`, `XDspActivate`, `XDspDeactivate`, `XDspSubmitCommand` | streams and status words; outputs are silence | same, a port of it | same (VIctory computes no transform either) |

## Audio hardware: `AcpHal.dll` (`audio.rs` / `runtime/shims/acphal.cpp`, `src/xma_*`)

| Export or method | VIctory | Leonida before | Status |
|---|---|---|---|
| `ApuCreateHeap` | sizes not enforced; `cached == 0` is `E_INVALIDARG` | both parts reserved and enforced; `cached == 0` accepted | same in effect (the title's call has a non-zero cached size and a 0x226e00 non-cached part, `docs/audio_hal_exe_calls.md`) |
| `ApuAlloc` | alignment below 4 or not a power of two is `E_INVALIDARG` | alignment 0 taken as 1 | same in effect (the title passes 0x800); left more permissive on purpose |
| `ApuFree`, `ApuMapVirtualAddress`, `ApuIsVirtualAddressValid`, `ApuMapApuAddress` | heap block mapping | region mapping with the same results inside blocks | same |
| `AcpHalAllocateShapeContexts`, `AcpHalReleaseShapeContexts` | 0x60-byte request, XMA voices attached | same | same |
| `AcpHalCreate` and its six methods | connect, disconnect, submit, pop message, message count, release | same | same |
| XMA commands 5-8, 18 and the decoding thread | decodes enabled contexts into their rings | ported earlier (`src/xma_context.*`, `src/xma_decode.*`) | same |
| other SHAPE units (rate conversion, EQ, filters, flowgraph mixing) | acknowledged, not carried out | same | same (neither emulates them) |
| `audio/capture.rs` (`VICTORY_AUDIO_CAPTURE`) | diagnostics build only: records the title's output to a WAVE file through process loopback | none | **left**: a debugging aid outside the title's API, not on any boot path |

## Media Foundation: `mfplat.dll` (`media.rs` / new `runtime/shims/media_foundation.cpp`)

| Export or method | VIctory | Leonida before | Status |
|---|---|---|---|
| `MFCreateSourceResolver` | the host's resolver behind a proxy that opens local files only | bound straight to the host's, which fetches `http`, `rtsp` and other URLs over the network | **ported** |
| proxy `CreateObjectFromURL`, `BeginCreateObjectFromURL` | refuse a URL that is not a local file with `MF_E_UNSUPPORTED_SCHEME`, logged once per scheme and host; virtual-drive paths go to their host folders | (host) | **ported**; console paths (`GAME:`, `G:`, `T:`, also inside `file:` URLs) go through the new `rt::fs::read_path`, so the overlay and deleted-file rules apply |
| proxy `CreateObjectFromByteStream`, its async pair, `End*`, `CancelObjectCreation` | forwarded | (host) | **ported** |
| proxy `QueryInterface` | only `IUnknown` and `IMFSourceResolver` | (host) | **ported** |
| the rest of `mfplat.dll` | the host's | the host's | same; `mfplat.dll` stays a host module for `LoadLibrary` (`owns_dll`) |

## Speech: `XAccessibilityImpl` (`speech.rs` / `runtime/shims/xgameruntime_offline.cpp`)

| Slot | VIctory | Leonida before | Status |
|---|---|---|---|
| 8 `XSpeechSynthesizerEnumerateInstalledVoices` | no voice, `S_OK` | no voice, `S_OK` (a null callback is `E_INVALIDARG`) | same |
| 9 `XSpeechSynthesizerCreate` | a synthesizer handle | `E_FAIL`, null handle | **ported** |
| 10, 14 close a synthesizer or stream | `S_OK` | `E_INVALIDARG` | **ported** |
| 11, 12 set default or custom voice | `S_OK` | `E_INVALIDARG` | **ported** |
| 13, 21 stream from text, from SSML | a stream handle | `E_INVALIDARG`, null stream | **ported** |
| 15 `GetStreamDataSize` | 44 | `E_INVALIDARG` | **ported** |
| 16 `GetStreamData` | an empty 16 kHz mono 16-bit WAVE file; too small a buffer `E_NOT_SUFFICIENT_BUFFER` | `E_INVALIDARG` | **ported** |
| 3-7, 17-20 captions, high contrast, speech to text | missing in VIctory | answered (contrast Off, captions accepted, the rest `E_FAIL`) | same or better |

## Input: `gameinput.dll` (`gameinput.rs`, `gameinput/dualsense.rs` / `runtime/shims/gameinput.cpp`, new `runtime/shims/gameinput_xinput.cpp`)

Leonida's GameInput shim (`gameinput.cpp`) runs on SDL3 and is much broader than VIctory's (any SDL pad, force
feedback, raw HID, motion, UI navigation, a dispatcher). But it is built only when SDL3 is found, and the main build
(`runtime/build-vk`, `SDL3_DIR-NOTFOUND`) has no SDL3, so `gta6rt` had no `GameInputCreate` at all and the title's
import bound to the host's `GameInput.dll`: Xbox pads only, no keyboard pad. And the SDL3 shim was registered by name
only, while VIctory records that titles import `gameinput.dll` by ordinal only, so even an SDL3 build skipped it.

| Export or method | VIctory | Leonida before | Status |
|---|---|---|---|
| `GameInputCreate` as ordinal `#1` | the only registration | name only: an ordinal import went to the host | **ported** (both shims register `#1`) |
| `GameInputCreate` without SDL3 | XInput pads, keyboard, stand-in pad, wired DualSense | host `GameInput.dll` (`LEONIDA_GAMEINPUT=host` keeps that) | **ported** as `gameinput_xinput.cpp` |
| `IGameInput` 3-7: timestamp, current, next, previous, temporal reading | 32 readings per device, one per change | SDL3 shim only | **ported** |
| `IGameInput` 8 `RegisterReadingCallback` | token, never called | SDL3 shim calls them | **ported** as VIctory (the title polls) |
| `IGameInput` 9 `RegisterDeviceCallback` | blocking enumeration on the caller's thread, async on the next poll | SDL3 shim only | **ported** |
| `IGameInput` 10-13: guide, layout callbacks, `StopCallback`, `UnregisterCallback` | tokens; unregister waits for a running callback, fails inside it | SDL3 shim only | **ported** |
| `IGameInput` 14, 15 `CreateDispatcher`, `CreateAggregateDevice` | `E_NOTIMPL` | SDL3 shim implements both | **ported** as VIctory (no title of VIctory's asks); the SDL3 shim keeps its own |
| `IGameInput` 16-21: find by id, by object/handle/string, OEM support, focus policy | id lookup, the rest not found / accepted | SDL3 shim only | **ported** |
| keyboard device and its readings (`GetKeyCount`, `GetKeyState`) | scan codes and virtual keys while focused | none in either path the main build used (the SDL3 shim reports no key readings) | **ported** in the XInput shim; **left** in the SDL3 shim (not built here; its keyboard pad covers menus there) |
| stand-in pad from the keyboard | on whenever no pad is connected | SDL3 shim: `LEONIDA_KEYBOARD_PAD`, off by default | **ported**: on by default in the XInput shim, `LEONIDA_KEYBOARD_PAD=off` drops it; the SDL3 shim's default left as it is |
| wired DualSense over HID | between XInput and the stand-in | SDL3 shim only | **ported** (SetupAPI and `hid.dll`; USB report 0x01 only) |
| `IGameInputDevice` info, status, `SetRumbleState` | id at +0x20, two motors on XInput pads | SDL3 shim only | **ported**; trigger motors dropped as in VIctory |
| `IGameInputDevice` battery, force feedback, haptics, sync, power off, raw I/O | `E_NOTIMPL` | SDL3 shim implements most | **ported** as VIctory, with a zeroed battery state and null out pointers |
| `IGameInputReading` raw report, axes, buttons, switches, mouse, touch, motion, sticks, wheel, UI navigation | nothing | SDL3 shim answers several | **ported** as VIctory (nothing) |
| `gameinput/movement.rs`, `VICTORY_KEYS`, `VICTORY_ENTER_ON_FILE` | diagnostics-build scripted input | none | **left**: test automation, not the title's API |

## SDL3

The SDL3 shim needs SDL3 3.2 or newer as a static CMake package. Nothing was downloaded here. To build it:

- install SDL3 from its official release page (`https://github.com/libsdl-org/SDL/releases`, the `SDL3-devel-*-VC`
  package for MSVC, or build `SDL3-3.4.20` from source with `-DSDL_STATIC=ON`) and pass
  `-DSDL3_DIR=<SDL3>/cmake` when configuring `runtime/build-vk`; the static target `SDL3::SDL3-static` is required, or
- configure with `-DLEONIDA_SDL3_FETCH=ON`, which downloads the pinned `SDL3-3.4.20.tar.gz` from SDL's GitHub releases
  and checks its SHA-256 (`docs/gameinput.md`), or pass `-DFETCHCONTENT_SOURCE_DIR_SDL3=<extracted SDL3-3.4.20>` for a
  copy already on disk.

Without it the XInput shim above serves the title, so input works either way.

## Tests

All synthetic, run one by one with `ctest -R`:

- `gameinput_xinput` (`runtime/test/gameinput_xinput_test.cpp`): VIctory's GameInput cases over a fake host, the
  DualSense report decoder, the stand-in key map, and the `#1` registration.
- `media_foundation` (`runtime/test/media_foundation_test.cpp`): VIctory's URL cases, console paths through
  `rt::fs::read_path`, and the proxy over a fake host resolver.
- `xgameruntime_offline` (`runtime/test/xgameruntime_offline_test.cpp`): the silent speech stream, byte for byte.
