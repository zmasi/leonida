# XMA2 decoder C ABI

This crate exposes a C ABI around the separately licensed xma Rust package pinned in Cargo.toml. It decodes one mono or stereo stream from a non-empty buffer of complete 2048-byte XMA2 packets into interleaved signed 16-bit PCM.

Each decoder handle serializes decode calls. The returned PCM block owns its samples and stays valid until leonida_xma2_pcm_destroy, even after its decoder is destroyed. Do not destroy a decoder concurrently with a decode call or destroy a handle more than once.

The tests build packets in memory; they do not open or contain audio fixtures. The synthetic C++ smoke test verifies the exported ABI and ownership boundary.

Run:

```powershell
cargo fmt --manifest-path runtime/xma_decoder/Cargo.toml -- --check
cargo test --manifest-path runtime/xma_decoder/Cargo.toml --locked
cargo build --manifest-path runtime/xma_decoder/Cargo.toml --locked
```

The `xma2-decoder` workflow builds and runs the C++ smoke test with MSVC. It links `ws2_32.lib`, `userenv.lib`,
`ntdll.lib`, and `synchronization.lib` for Rust's Windows standard library. `synchronization.lib` resolves the
`WaitOnAddress` and `WakeByAddress*` imports in the Rust static library.

## Use in the runtime

`runtime/CMakeLists.txt` builds this crate for `x86_64-pc-windows-msvc` at configure time when `cargo` is on `PATH` and links it into `gta6rt` and `acphal_test` with `LEONIDA_HAVE_XMA2_DECODER` defined. The first build fetches the pinned `xma` git dependency. Set `CARGO_NET_OFFLINE=true` for a configure that uses only the local Cargo cache.

`runtime/shims/acphal.cpp` uses `leonida_xma2_decoder_create_hardware` and decodes 2048-byte packets. The shim uses the frame bit positions in the packet to keep PCM through the selected 128-sample loop-end subframe on a repeating pass. If that frame continues in the next linked packet, the shim decodes the continuation before rewinding, including when the linked packet comes from the other input buffer. The shim starts the next pass at the selected loop-start frame with a fresh decoder. Unmatched frame metadata retains packet-level handling. Decoder errors still start a fresh decoder.

Without `cargo`, when that build fails, or with the cl.exe fallback of `scripts/build_runtime.ps1`, XMA voices output silence.
Use `-DLEONIDA_REQUIRE_XMA2_DECODER=ON` to require the decoder in a CMake build, as the Windows runtime CI does.

To link an already-built `leonida_xma2_decoder.lib` without invoking `cargo` during CMake configure (for offline builds
or Linux cross-builds whose distribution `rustc` omits the `x86_64-pc-windows-msvc` standard library), pass
`-DLEONIDA_XMA2_DECODER_LIB=<path>`. A value that does not name an existing file fails configure instead of falling
back silently to muted XMA voices.

Build that library yourself, from this checkout's `runtime/xma_decoder`, so that it matches the C ABI the runtime is
compiled against. Configure checks only that the file exists, not which source, version or target it was built from,
so don't use a library from another checkout, another project or a download:

```sh
rustup target add x86_64-pc-windows-msvc
cargo build --manifest-path runtime/xma_decoder/Cargo.toml --release --locked --target x86_64-pc-windows-msvc
```

The library is then `runtime/xma_decoder/target/x86_64-pc-windows-msvc/release/leonida_xma2_decoder.lib`. Building a
static library for that target needs only the Rust target, not the MSVC linker, so this also works on Linux.

With the option set, CMake no longer builds the decoder: after a change under `runtime/xma_decoder/`, run the command
again before building the runtime. A relative path given with `-D` on the command line is made absolute against the
directory CMake runs in, so passing an absolute path avoids surprises.
