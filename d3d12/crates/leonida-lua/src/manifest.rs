//! `mods\<name>\mod.json` as the Lua runtime reads it: the same files the mod
//! manager (`tools/leonida_mods.py`) lists. A Lua mod has `"script":
//! "main.lua"` next to the manifest; `"enabled"` is true unless it says
//! otherwise; `"name"` is shown in logs.

use std::path::{Component, Path, PathBuf};

use crate::json::{self, Json};

/// The largest script read.
pub const MAX_SCRIPT: u64 = 1 << 20;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Manifest {
    /// The folder's name.
    pub folder: String,
    pub name: String,
    pub enabled: bool,
    /// The script, relative to the folder; `None`: not a Lua mod.
    pub script: Option<String>,
}

/// Reads `folder/mod.json`.
pub fn read(folder: &Path) -> Result<Manifest, String> {
    let folder_name = folder
        .file_name()
        .map_or_else(|| "mod".to_owned(), |n| n.to_string_lossy().into_owned());
    let text = std::fs::read_to_string(folder.join("mod.json"))
        .map_err(|e| format!("cannot read mod.json: {e}"))?;
    parse(&folder_name, text.trim_start_matches('\u{feff}'))
}

/// Reads a manifest's text.
pub fn parse(folder_name: &str, text: &str) -> Result<Manifest, String> {
    let json = json::parse(text).map_err(|e| format!("mod.json is not valid JSON ({e})"))?;
    let Json::Object(members) = json else {
        return Err("mod.json is not a JSON object".into());
    };
    // The last of a repeated key wins, as in most readers.
    let get = |key: &str| members.iter().rev().find(|(k, _)| k == key).map(|(_, v)| v);
    let enabled = match get("enabled") {
        None => true,
        Some(Json::Bool(b)) => *b,
        Some(_) => return Err("\"enabled\" must be true or false".into()),
    };
    let name = match get("name") {
        Some(Json::String(s)) if !s.trim().is_empty() => s.trim().to_owned(),
        _ => folder_name.to_owned(),
    };
    let script = match get("script") {
        None => None,
        Some(Json::String(s)) => {
            check_script_path(s)?;
            Some(s.clone())
        }
        Some(_) => return Err("\"script\" must be a file name such as \"main.lua\"".into()),
    };
    Ok(Manifest {
        folder: folder_name.to_owned(),
        name,
        enabled,
        script,
    })
}

/// A script path stays inside the mod's folder and names a `.lua` file.
fn check_script_path(path: &str) -> Result<(), String> {
    safe_relative(path)?;
    if !path.to_ascii_lowercase().ends_with(".lua") {
        return Err("\"script\" must be a .lua file".into());
    }
    Ok(())
}

/// Whether `path` is a plain relative path of ordinary names (no `..`, no
/// root, no drive, no stream or reserved device syntax): what a mod may name
/// inside its folder.
pub fn safe_relative(path: &str) -> Result<PathBuf, String> {
    if path.is_empty()
        || path.len() > 200
        || path.contains(['\0', ':', '\\', '*', '?', '"', '<', '>', '|'])
    {
        return Err(format!(
            "`{path}` is not a plain path inside the mod's folder"
        ));
    }
    let mut out = PathBuf::new();
    for part in Path::new(path).components() {
        match part {
            Component::Normal(n) => {
                let n = n.to_string_lossy();
                let stem = n.split('.').next().unwrap_or("").to_ascii_uppercase();
                let reserved = ["CON", "PRN", "AUX", "NUL"].contains(&stem.as_str())
                    || (stem.len() == 4
                        && (stem.starts_with("COM") || stem.starts_with("LPT"))
                        && stem.as_bytes()[3].is_ascii_digit());
                if reserved || n.ends_with('.') || n.ends_with(' ') {
                    return Err(format!(
                        "`{path}` is not a plain path inside the mod's folder"
                    ));
                }
                out.push(part);
            }
            _ => {
                return Err(format!(
                    "`{path}` is not a plain path inside the mod's folder"
                ));
            }
        }
    }
    if out.as_os_str().is_empty() {
        return Err(format!(
            "`{path}` is not a plain path inside the mod's folder"
        ));
    }
    Ok(out)
}

/// The mods folder: `LEONIDA_MODS`, else `mods` beside the executable or in a
/// folder above it (so a run from `d3d12\target\...` finds the repository's
/// own), else `mods` in the current folder.
pub fn mods_dir() -> Option<PathBuf> {
    if let Some(dir) = std::env::var_os("LEONIDA_MODS") {
        let dir = PathBuf::from(dir);
        return dir.is_dir().then_some(dir);
    }
    let mut candidates = Vec::new();
    if let Ok(exe) = std::env::current_exe() {
        let mut at = exe.parent().map(Path::to_path_buf);
        for _ in 0..6 {
            let Some(dir) = at else { break };
            candidates.push(dir.join("mods"));
            at = dir.parent().map(Path::to_path_buf);
        }
    }
    candidates.push(PathBuf::from("mods"));
    candidates.into_iter().find(|d| d.is_dir())
}

/// The subfolders of `dir` that have a `mod.json`, by name.
pub fn folders(dir: &Path) -> Vec<PathBuf> {
    let Ok(read) = std::fs::read_dir(dir) else {
        return Vec::new();
    };
    let mut all: Vec<PathBuf> = read
        .filter_map(Result::ok)
        .map(|e| e.path())
        .filter(|p| p.is_dir() && p.join("mod.json").is_file())
        .collect();
    all.sort();
    all
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_lua_manifest_is_read_with_its_defaults() {
        let m = parse("example", r#"{"name": " Example ", "script": "main.lua"}"#).unwrap();
        assert_eq!(
            m,
            Manifest {
                folder: "example".into(),
                name: "Example".into(),
                enabled: true,
                script: Some("main.lua".into())
            }
        );
        let m = parse("x", r#"{"enabled": false, "script": "src/a.LUA"}"#).unwrap();
        assert!(!m.enabled);
        assert_eq!(
            (m.name.as_str(), m.script.as_deref()),
            ("x", Some("src/a.LUA"))
        );
        // An asset mod has no script: not ours.
        let m = parse("assets", r#"{"name": "Mandy", "archetype_swaps": []}"#).unwrap();
        assert_eq!(m.script, None);
    }

    #[test]
    fn bad_manifests_say_why() {
        for bad in [
            "[]",
            "{",
            r#"{"enabled": "yes"}"#,
            r#"{"script": 1}"#,
            r#"{"script": "../main.lua"}"#,
            r#"{"script": "/main.lua"}"#,
            r#"{"script": "C:main.lua"}"#,
            r#"{"script": "main.txt"}"#,
            r#"{"script": "a\\b.lua"}"#,
            r#"{"script": "NUL.lua"}"#,
        ] {
            assert!(parse("m", bad).is_err(), "{bad}");
        }
    }

    #[test]
    fn paths_stay_inside() {
        assert!(safe_relative("data.txt").is_ok());
        assert!(safe_relative("sub/dir/data.txt").is_ok());
        for bad in [
            "",
            "..",
            "../x",
            "a/../../x",
            "/x",
            "x:y",
            "a\\b",
            "a/./..",
            "CON",
            "aux.txt",
            "x.",
            "com1",
        ] {
            assert!(safe_relative(bad).is_err(), "{bad:?}");
        }
    }
}
