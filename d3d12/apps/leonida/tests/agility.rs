//! `leonida.exe` asks Windows for Microsoft's newer Direct3D 12 runtime
//! (the D3D12 Agility SDK) beside it: Windows reads `D3D12SDKVersion` and
//! `D3D12SDKPath` from the executable's exports, and reads them when
//! `d3d12.dll` is loaded, so that DLL is delay-loaded (src/agility.rs,
//! build.rs). Without the exports Windows 10 keeps its own runtime, which
//! stops at shader model 6.5 and refuses every translated pipeline.
#![cfg(all(windows, target_env = "msvc"))]

use std::process::Command;

/// `leonida inspect <leonida.exe> <flag>`'s output.
fn inspect_self(flag: &str) -> String {
    let exe = env!("CARGO_BIN_EXE_leonida");
    let output = Command::new(exe)
        .args(["inspect", exe, flag])
        .output()
        .unwrap();
    assert!(
        output.status.success(),
        "inspect failed: {}",
        String::from_utf8_lossy(&output.stderr)
    );
    String::from_utf8_lossy(&output.stdout).into_owned()
}

#[test]
fn the_exe_exports_the_agility_sdk_symbols() {
    let stdout = inspect_self("--exports");
    for name in ["D3D12SDKVersion", "D3D12SDKPath"] {
        assert!(
            stdout
                .lines()
                .any(|line| line.split_whitespace().nth(1) == Some(name)),
            "{name} is not exported:\n{stdout}"
        );
    }
}

#[test]
fn d3d12_dll_is_delay_loaded() {
    let stdout = inspect_self("--imports");
    let (plain, delayed) = stdout
        .split_once("delay-load imports")
        .unwrap_or((&stdout, ""));
    let names = |part: &str| -> Vec<String> {
        part.lines()
            .filter_map(|line| line.split_whitespace().next())
            .map(str::to_ascii_lowercase)
            .collect()
    };
    assert!(
        !names(plain).iter().any(|dll| dll == "d3d12.dll"),
        "d3d12.dll is imported before main runs:\n{stdout}"
    );
    assert!(
        names(delayed).iter().any(|dll| dll == "d3d12.dll"),
        "d3d12.dll is not delay-loaded:\n{stdout}"
    );
}
