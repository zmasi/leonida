//! Offline translation of console shader containers (`.cdxil`, as
//! `LEONIDA_SHADER_SAVE` writes them) with the host settings of the
//! NVIDIA run: waves of 32, a view heap of 2^21 slots with raw twins, no
//! RDNA shortcuts.
//!
//! (`TRANSLATE_VALIDATE=1` with `--census` runs each translation through dxil.dll's validator and
//! prints `INVALID` lines for the ones it refuses.)
//!
//! `translate_cdxil <out dir> <file.cdxil>...` writes `<stem>.dxil` (the
//! translation) and `<stem>.s` (the console code in SP3 text, with the
//! program's wave size, threads and LDS in a header) and prints a line of
//! counts per shader. Shaders are the user's own: nothing here is kept in
//! the repository.

use leonida_shader::native::StageIo;
use leonida_shader::{Container, FourCC, emit, gfx10, native_programs};

fn main() {
    let mut args = std::env::args().skip(1);
    // `--census <out dir> files...`: one TSV line per shader, no files written
    let census_only = std::env::args().nth(1).as_deref() == Some("--census");
    if census_only {
        args.next();
        // SAFETY: before any other thread exists
        unsafe { std::env::set_var("LEONIDA_SHADER_CENSUS", "1") };
    }
    let out = std::path::PathBuf::from(args.next().expect("out dir"));
    // `--validate` (with `--census`): each translation also goes through dxil.dll's validator
    let validate = std::env::var_os("TRANSLATE_VALIDATE").is_some();
    #[cfg(windows)]
    let mut validator = validate
        .then(|| emit::validator::Validator::load(emit::validator::Validator::default_path().as_deref()).expect("dxil.dll"));
    std::fs::create_dir_all(&out).unwrap();
    emit::set_host_max_wave(32);
    emit::set_raw_views(1 << 21);
    emit::set_view_heap_limit((1 << 21) - 1);
    emit::set_host_rdna(false);
    for path in args {
        let path = std::path::PathBuf::from(path);
        let stem = path.file_stem().unwrap().to_string_lossy().into_owned();
        let bytes = std::fs::read(&path).unwrap();
        let c = Container::parse(&bytes).unwrap();
        let mut head = String::new();
        let mut native_insts = 0usize;
        if let Some(part) = c.part(FourCC::DXIL).or(c.part(FourCC::ACCC)) {
            for p in native_programs(part.data).unwrap() {
                let mut text = String::new();
                if let StageIo::Cs(cs) = &p.io {
                    head = format!(
                        "wave64={} threads={:?} lds={} vgprs={}",
                        p.wave64,
                        cs.threads,
                        cs.lds_bytes(),
                        p.vgprs
                    );
                }
                text.push_str(&format!("; {:?} {head}\n", p.stage));
                if let Ok(insts) = gfx10::decode(&p.code) {
                    native_insts = insts.len();
                    for i in &insts {
                        text.push_str(&format!("{:04x}: {}\n", i.offset, i.text(p.wave64)));
                    }
                }
                if !census_only {
                    std::fs::write(out.join(format!("{stem}.s")), text).unwrap();
                }
            }
        }
        let t = std::time::Instant::now();
        match emit::translate(&bytes) {
            Ok(h) if census_only => {
                #[cfg(windows)]
                if let Some(v) = validator.as_mut() {
                    let mut copy = h.dxil.clone();
                    if let Err(e) = v.validate(&mut copy) {
                        println!("{stem}	INVALID	{}", e.lines().next().unwrap_or(""));
                        continue;
                    }
                }
                let c = emit::census::take(&h.dxil).unwrap_or_default();
                let counts: Vec<String> = c.counts.iter().map(|n| n.to_string()).collect();
                // (with LEONIDA_GROUP_PACK: the packing of the program, or why not)
                let packed = h
                    .user_data
                    .iter()
                    .find(|u| u.root == leonida_shader::native::RootType::Other(emit::USER_PACK_GROUPS))
                    .map_or_else(|| format!("0 {}", emit::take_pack_why().unwrap_or("").replace(' ', "_")), |u| format!("{} -", u.dword));
                println!("{stem}	{}	{native_insts}	{}	{}	pack	{packed}", head.replace(' ', ","), h.dxil.len(), counts.join("	"));
            }
            Ok(h) => {
                std::fs::write(out.join(format!("{stem}.dxil")), &h.dxil).unwrap();
                println!(
                    "{stem}: {head} native_insts={native_insts} dxil_bytes={} ({} ms)",
                    h.dxil.len(),
                    t.elapsed().as_millis()
                );
            }
            Err(e) if census_only => println!("{stem}	ERR	{e}"),
            Err(e) => println!("{stem}: {head} native_insts={native_insts} ERROR {e}"),
        }
    }
}
