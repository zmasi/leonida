# VIctory gap table: system side

What VIctory (in-tree at `victory/`, Rust, GPL-3.0-or-later) serves in the system modules and the loader, against what
Leonida did, and what was ported on branch `port/gap-system`. Each ported piece names its VIctory source in a comment.

Slice: `victory/crates/victory-hle/src/modules/` `kernel`, `xmem`, `files`, `lifecycle`, `xsystem`, `xpackage`,
`xgame`, `xgameruntime` and the runtime classes it dispatches to (`xuser`, `xgamesave`, `xstore`, `xgameui`, `xasync`,
`xnetworking`, `storage`), `net`, `mod.rs`; and `victory/crates/victory-loader/src/` (thunks, TLS, thread notices,
crash handling and CPU instruction emulation). `display.rs`, `speech.rs` and the graphics, audio and input modules are
other slices.

How the comparison was made: VIctory's exports are its modules' `hle_export!` lists; its runtime classes are the
`INTERFACES` (provider, interface id) pairs and the vtable slots that are not `missing::<N>`. Leonida's are the
`SHIM_TABLE` entries and the `xgameruntime_*.cpp` vtables. "The title" is the retail executable both run; where VIctory
says a slot or id is what the title asks for, that is VIctory's measurement, not one made here (no game was run for
this work).

Status: **ported** (on this branch), **had** (Leonida already served it as VIctory does), **left** (with the reason).

## Runtime classes (QueryApiImpl)

| Class / interface | VIctory serves | Leonida before | Status |
|---|---|---|---|
| `XPackageImpl` as `IXPackageImpl4` (`0b9d556e-...`) | the id the title asks with; slots 0x18-0x138 | not an accepted id: every XPackage call of the title got `E_NOINTERFACE` | **ported**: id accepted (and slot 41 `XPackageGetPackageKind`, no public signature, `E_NOTIMPL`) |
| `XPackageImpl` slots 5, 6, 9 (installation monitor create/close, progress registration) | a fixed monitor for the finished installation; a token | `E_NOTIMPL` | **ported** |
| `XPackageImpl` slot 7 (installation progress) | the package folder's bytes, launchable, completed | 0 of 0 bytes, completed | **ported** (game folder's bytes, counted once) |
| `XPackageImpl` slots 12, 14, 18, 19 (find chunk availability, change order, estimate download, uninstall chunks) | ready / accepted / nothing to download, own package only, else `0x89245208` | `E_NOTIMPL` | **ported** |
| `XPackageImpl` slots 15-17 (install chunks, async + result) | async completes with the monitor (sync form missing) | `E_NOTIMPL` | **ported**, the sync form too |
| `XPackageImpl` slots 37/38 (mount with UI) | completes with "not installed" | `E_NOTIMPL` | **ported**, through `XPackageMount` so `LEONIDA_CONTENT_PACKAGES` mounts work here too |
| `XPackageImpl` slots 39/40 (enumerate packages, package installed) | enumerate: none (slot 40 missing) | `E_NOTIMPL` | **ported** (40 hands out a token, never fires) |
| `XPackageImpl` slots 13, 20, 21, 27-33, 35, 36 | missing in VIctory too | `E_NOTIMPL` | left: neither serves them, no measured caller |
| `XUserImpl` slot 15 (0x78) | the older `XUserGetGamertag` | `E_NOTIMPL` ("unknown in the docs") | **ported** |
| `XUserImpl` slots 3-14, 19-25, 33/34, gamertag and device interfaces | served | served (more: 35-49) | had |
| `XUserImpl` token slots 23-25 | complete with `E_GAMEUSER_FAILED_TO_GET_TOKEN` | fail before starting with the same error | had (both offline failures) |
| `XStoreImpl` slots 71/72 (licence for durables) | completes with a licence that is not valid | `E_FAIL` before starting | **ported** |
| `XStoreImpl` slot 65 (package identifier for a store id) | the package id for the title's own store id | `ERROR_NOT_FOUND` for every id | left: Leonida has no store id source in the store shim; low use (one lookup) |
| `XStoreImpl` consumables, collections/purchase ids, purchase/redeem/product page UIs, update downloads (33-42, 49, 53, 58-64, 75) | async starts and completes with the store network error `0x89245301` | fails before starting (`E_FAIL`), no async block used | left: both are offline failures; Leonida's file documents its choice (nothing waits on a block). Revisit if a call site is seen to wait for the completion |
| `XStoreImpl` contexts, game licence, entitled/associated products, package licences, updates, registrations | served | served | had |
| `XGameUiImpl` slots 7/8 (profile card) | completes, S_OK | `E_FAIL` | **ported** |
| `XGameUiImpl` slots 17-19 (text entry) | a modal host dialog; `E_ABORT` when dismissed | `E_FAIL` | **ported** as dismissed (`E_ABORT`, which VIctory measured the title treats as cancelled); the host dialog is left (a UI to type into; no measured need during boot) |
| `XGameSaveImpl` (`IXGameSaveImpl3` id, slots 3-29) | served | served (3-32) | had |
| `XThreadingImpl` (XAsync/XTaskQueue) | served | served (third_party/xtaskqueue) | had |
| `XSystemImpl`, `XSystemAnalyticsImpl` | served | served | had |
| `XGameImpl`, `XGameRuntimeFeatureImpl`, `XGameEventImpl` | served | served | had |
| `XGameInviteImpl` (`0651aae2`) register/unregister | served | not served (class not provided) | left: the retail executable's 161 thunks reach 17 classes and this is not one (docs/runtime_imports.md, RT-5) |
| `XNetworkingImpl` (`IXNetworkingImpl2`) | slots 3-5, 8-10, 14-17 | slots 3-20 | had |
| `XPersistentLocalStorageImpl` | slots 3-7 | slots 3-8 | had |
| `XUserDeviceImpl` | 3-5 | 3-10 | had |
| `XAppCaptureImpl` as `IXAppCaptureImpl4` (`22e672d7`), `XDisplayImpl`, `XLauncherImpl`, `XAccessibilityImpl` | in `display.rs` / `speech.rs` | partly served | left: other slice (display / speech) |

## DLL exports

| Export(s) | VIctory | Leonida before | Status |
|---|---|---|---|
| `GetModuleHandleA/W(NULL)` | the title's image | gta6rt.exe's base | **ported** (`library_loader.cpp`; main.cpp sets the main image) |
| `GetModuleFileNameA/W` (NULL or a mapped image) | the title image's own file | gta6rt.exe's path, or `ERROR_MOD_NOT_FOUND` for a mapped image | **ported** (new `BundledModuleOps::module_path`, `Loader::module_path`) |
| `LoadLibrary*`, `GetModuleHandle*` by name, `GetProcAddress`, `FreeLibrary`, `DisableThreadLibraryCalls` | served | served | had |
| `OutputDebugStringA/W` | to the log | the host's (seen only by a debugger) | **ported** (`process.cpp`: `[title] ...`, `LEONIDA_TITLE_DEBUG_OUTPUT=0` to silence) |
| `TerminateProcess` (own process) | logged, then done | the host's, silent | **ported** (logged with code and caller) |
| `exit`, `ExitProcess`, `abort` | logged exits | served by `process.cpp` (end the run, atexit table) | had |
| `terminate`, `_invalid_parameter_noinfo_noreturn` | logged, then abort | the host CRT's | left: diagnostics only; the startup trace reports the fault |
| `SetUnhandledExceptionFilter` | ours in front of the title's, crash report first | the host's | left: diagnostics only; the startup trace's vectored handler already sees exceptions first |
| `CreateFile*`, `DeleteFile*`, `GetFileAttributes*`, `FindFirstFile*`, `MoveFileEx*`, `CreateDirectory*`, `RemoveDirectory*`, `SetFileAttributes*` | console drives | served by `file_system.cpp` (GAME:, G:, T:, overlay) | had |
| `GetDiskFreeSpaceExW`, `GetDiskFreeSpaceW` | through the drive | the host's (fails or answers for a host drive) | **ported**, A forms too |
| `SetCurrentDirectoryA` | through the drive | the host's | **ported**, W too |
| `ReadFile`, `fread` | VIctory's own drive/streaming bookkeeping | the host's | left: Leonida's handles are real host files opened on the mapped path, so the host's read is right |
| CRT path functions (`fopen`, `_wfopen(_s)`, `_wfsopen`, `_open`, `_wopen`, `_wstat64`, `_stat64i32`, `remove`, `_wchmod`, `_wutime64`, msvcp140 `_Fiopen`) | through the drive | the host CRT's, which calls the host's `CreateFileW` directly, so a console path fails | left: whether the executable imports them was not measured here; worth porting as soon as an import listing or a run shows a CRT open of a console path |
| `RegisterAppStateChangeNotification`, `UnregisterAppStateChangeNotification` | kept; run on close with a 1 s deadline | the host's (for packaged apps; never calls them) | **ported** (`app_state.cpp`; closing the window now suspends the title first) |
| `CreateWindowExA/W` | fitted, close ends the title | served (`title_window.cpp`, an earlier port) | had |
| `XMem*` (13 exports) | served | served (`xmem.cpp`) | had (behaviour not compared line by line here) |
| `RegOpenKeyW`, `RegOpenKeyExW`, `RegQueryValueExW`, `RegCloseKey` (device type) | served | served (`registry_oem.cpp`) | had |
| `ws2_32.dll` `connect`, `WSAConnect`, `sendto`, `WSASendTo`, `bind`, `WSAIoctl` (ConnectEx, WSASendMsg), `getaddrinfo`, `GetAddrInfoW`, `gethostbyname`, `gethostbyaddr`, `getnameinfo`, `GetNameInfoW`, `WSAConnectByNameA/W`, ordinals 2, 4, 20, 51, 52 | loopback only | the host's: a `--run` could open real sockets ("nothing online") | **ported** (`winsock_offline.cpp`) |
| `GetAddrInfoExA/W`, `WSAConnectByList`, `WSAJoinLeaf`, `WSASendMsg` (import), `WSAAsyncGetHostByName/Addr`, `WSALookupServiceBeginA/W` | loopback only | the host's | left: the executable imports 23 classic ordinals plus `getaddrinfo`/`freeaddrinfo`, which cannot name these; worth adding if a bundled DLL imports them |
| `winhttp.dll` `WinHttpConnect`, `WinHttpOpen`, `WinHttpSetOption`, proxy queries | loopback only, proxies off | the whole module is offline (`winhttp_offline.cpp`: no request ever connects) | had (Leonida's is stricter; the proxy functions resolve to the unimplemented stub, which answers FALSE) |
| `xgameruntime.dll` `XGameRuntimeInitialize`, `*ApiImpl`, `QueryApiImpl`, `XErrorReport` | served | served | had |

## Loader

| Piece | VIctory | Leonida before | Status |
|---|---|---|---|
| CPU instructions a PC may lack (`console_insn.rs`, `crash.rs`): CLZERO (and a whole clzero loop), CLWB (patched to NOPs), MONITORX/MWAITX, RDPRU, RDPID, SSE4a EXTRQ/INSERTQ | emulated from a vectored handler | none: an Intel host dies at the title's first CLZERO loop | **ported** (`src/console_insn.*`; decoder written from AMD's manual, not translated from VIctory's, which is XCL's GPL-2.0-only code; installed in main.cpp after load, mapped images only). Not needed on an AMD Zen 2+ host, which runs them natively |
| CLWB write-back loop fused (`crash.rs clwb_loop`) | one exception per loop | none | left: the CLWB site is patched to NOPs on its first fault, so the loop runs natively after that |
| Unimplemented-import thunks (`thunks.rs`) | log and return | `registry.cpp` thunks with the log/fail/stop policy | had |
| `/GA` static TLS rewrite (`tls.rs`) | thunk per `gs:[58h]` load | rewritten to a TlsAlloc slot (`pe_loader.cpp`) | had |
| Thread notices to mapped images (`threads.rs`) | own TLS callback | `gta6rt_tls_host.dll` thread events | had |
| Crash reports (`crash.rs`): first-chance fatal kinds, unhandled filter, crash log with the title's last debug output | served | the startup trace (`--trace`) logs exceptions with module+RVA | left in part: the crash log file and "last debug output" are diagnostics; the debug output now reaches stderr |
| Stack dumps on a schedule (`stacks.rs`, `VICTORY_STACKS`) | served | none | left: a diagnostic, not on the boot path |

## Tests (no game data)

`module_identity`, `winsock_offline`, `console_insn`, `app_state` are new; `xgameruntime`, `xgameruntime_user`,
`xgameruntime_user_gamertag`, `xgameruntime_user_gamertag_long`, `xgameruntime_store`, `xgameruntime_offline`,
`xgameruntime_offline_hdr`, `file_system` and `process_cmdline` were extended. Each runs with
`ctest --test-dir runtime/build-vk -C RelWithDebInfo -R "^<name>$"`.
