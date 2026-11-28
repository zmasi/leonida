//! `leonida.exe` exports `D3D12SDKVersion` and `D3D12SDKPath` (src/agility.rs):
//! Windows' Direct3D 12 reads them from the process's executable to load
//! Microsoft's newer runtime from `D3D12\` beside it (Windows 10's own stops
//! at shader model 6.5). An executable exports nothing by itself, so the
//! linker is told to.
//!
//! Direct3D 12 reads `D3D12SDKVersion` when `d3d12.dll` is loaded. Imported
//! the usual way, it is loaded before `main` runs, so the version `main` sets
//! from the `D3D12` folder (or Windows' own, without one) would come too late.
//! Loaded on its first use instead, it reads that version.
//!
//! From trustno1-coder/VIctory-NVIDIA (GPL-3.0-or-later), commit 0f0ada1 there.
fn main() {
    if std::env::var("CARGO_CFG_TARGET_ENV").as_deref() == Ok("msvc") {
        println!("cargo:rustc-link-arg-bins=/EXPORT:D3D12SDKVersion,DATA");
        println!("cargo:rustc-link-arg-bins=/EXPORT:D3D12SDKPath,DATA");
        println!("cargo:rustc-link-arg-bins=/DELAYLOAD:d3d12.dll");
        println!("cargo:rustc-link-arg-bins=delayimp.lib");
    }
}
