//! Fingerprints the shader translator, so that cached translations
//! (`src/shader_cache.rs`) last until the translator itself changes and not
//! only until the next build of anything.

use std::path::{Path, PathBuf};

fn files(dir: &Path, out: &mut Vec<PathBuf>) {
    for entry in std::fs::read_dir(dir).into_iter().flatten().flatten() {
        let path = entry.path();
        if path.is_dir() {
            files(&path, out);
        } else {
            out.push(path);
        }
    }
}

fn main() {
    let here = PathBuf::from(std::env::var_os("CARGO_MANIFEST_DIR").unwrap_or_default());
    let crates = here.join("..");
    let root = crates.join("..");
    // The translator, what it is built from, and the form entries are kept in.
    let mut all = vec![
        crates.join("leonida-shader/Cargo.toml"),
        here.join("src/shader_cache.rs"),
        root.join("Cargo.lock"),
        root.join("rust-toolchain.toml"),
    ];
    let source = crates.join("leonida-shader/src");
    println!("cargo:rerun-if-changed={}", source.display());
    files(&source, &mut all);
    // Leonida: not the translator's test modules (`*_tests.rs`, built only for `cargo test`). A new test changed
    // the fingerprint, and every one of the title's 25,663 shaders was translated again (two minutes) and its
    // pipelines made again, for a translator that emits the same code.
    all.retain(|path| !path.file_name().is_some_and(|n| n.to_string_lossy().ends_with("_tests.rs")));
    all.sort();
    let mut hash = blake3::Hasher::new();
    for path in &all {
        println!("cargo:rerun-if-changed={}", path.display());
        // Names and line endings as they are in every checkout.
        let name = path.file_name().unwrap_or_default().to_string_lossy().into_owned();
        let bytes: Vec<u8> = std::fs::read(path).unwrap_or_default().into_iter().filter(|&b| b != 13).collect();
        hash.update(name.as_bytes());
        hash.update(&(bytes.len() as u64).to_le_bytes());
        hash.update(&bytes);
    }
    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rustc-env=LEONIDA_TRANSLATOR={}", hash.finalize().to_hex());
    // The `dlss` feature links the DLSS SDK's loader (`nvsdk_ngx_d.lib`, the
    // dynamic-CRT one) from `LEONIDA_DLSS_SDK`.
    println!("cargo:rerun-if-env-changed=LEONIDA_DLSS_SDK");
    if std::env::var_os("CARGO_FEATURE_DLSS").is_some() {
        let sdk = std::env::var_os("LEONIDA_DLSS_SDK")
            .map(PathBuf::from)
            .expect("the dlss feature needs LEONIDA_DLSS_SDK (the NVIDIA/DLSS checkout)");
        println!("cargo:rustc-link-search=native={}", sdk.join("lib/Windows_x86_64/x64").display());
    }
}
