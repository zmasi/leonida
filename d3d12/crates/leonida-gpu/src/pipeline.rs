//! Pipeline states. Both descriptions start as on PC: the root signature,
//! then the shader stages (five for graphics, one for compute); streams
//! carry the rest of the graphics state as subobjects of the standard
//! types.
//!
//! Host pipelines are made on a pipeline's first draw, not at creation:
//! The title creates some 60,000 pipelines in its first 20
//! seconds and draws with a few dozen (decision 0007, step 3). Shader
//! containers are shared by content hash and translated once, on first
//! use (decision 0003).
//!
//! shortcut: only vertex plus pixel pipelines from streams and compute
//! pipelines get host pipelines; mesh, tessellation and geometry stages,
//! and the plain graphics description, keep their containers only. The 32-byte
//! packet after the header stays zero: titles only copy it inline, which
//! our null command-list limits keep from running.
#![allow(clippy::not_unsafe_ptr_arg_deref)]

use std::collections::HashMap;
use std::mem::offset_of;
use std::sync::{Arc, Mutex, OnceLock};

use crate::object::{self, E_INVALIDARG, HRESULT, Iid, Object, PIPELINE, Table, This, guid};
use crate::resource::PAGEABLE_IIDS;

const PIPELINE_IIDS: [Iid; 4] = [
    guid(
        0x765a30f3,
        0xf624,
        0x4c6f,
        [0xa8, 0x28, 0xac, 0xe9, 0x48, 0x62, 0x24, 0x45],
    ),
    PAGEABLE_IIDS[0],
    PAGEABLE_IIDS[1],
    PAGEABLE_IIDS[2],
];

#[repr(C)]
pub(crate) struct Bytecode {
    code: *const u8,
    size: usize,
}

#[repr(C)]
pub(crate) struct GraphicsHead {
    root: This,
    /// Vertex, pixel, domain, hull, geometry.
    stages: [Bytecode; 5],
}

#[repr(C)]
pub(crate) struct ComputeHead {
    root: This,
    stage: Bytecode,
}

/// A shader container a title passed, shared by every pipeline with the
/// same bytes.
pub(crate) struct Shader {
    pub(crate) stage: &'static str,
    /// The first 8 bytes of its BLAKE3, as logged.
    pub(crate) hash: String,
    container: Vec<u8>,
    /// Its part in the stand-in for the indirect light.
    #[cfg(windows)]
    pub(crate) ambient: Option<crate::ambient::Role>,
    #[cfg(windows)]
    translated: OnceLock<Option<Translated>>,
}

impl std::fmt::Debug for Shader {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{} {}", self.stage, self.hash)
    }
}

/// Every container seen, by full content hash.
static SHADERS: Mutex<Option<HashMap<[u8; 32], Arc<Shader>>>> = Mutex::new(None);

/// For the memory lines: containers kept and their bytes, translations
/// kept and the bytes of their host code. None of them is ever let go.
static KEPT: [std::sync::atomic::AtomicU64; 4] = [const { std::sync::atomic::AtomicU64::new(0) }; 4];

/// What the shader table and the pipeline library keep in memory:
/// containers and their bytes, translations and their bytes, the bytes of
/// the library's file (kept whole while the library lives) and what its
/// last save wrote.
#[cfg(windows)]
#[cfg_attr(not(feature = "diagnostics"), allow(dead_code))]
pub(crate) fn kept() -> [u64; 7] {
    use std::sync::atomic::Ordering::Relaxed;
    [
        KEPT[0].load(Relaxed),
        KEPT[1].load(Relaxed),
        KEPT[2].load(Relaxed),
        KEPT[3].load(Relaxed),
        library::BLOB.load(Relaxed),
        library::SAVED.load(Relaxed),
        library::TRIMMED_BYTES.load(Relaxed),
    ]
}

/// Leonida: the program's own name in its container (`CS_WaterClip...`,
/// `PS_...`), for logs: a refused or skipped hash says nothing by itself.
pub(crate) fn program_name(container: &[u8]) -> Option<String> {
    const STAGES: [&[u8]; 8] = [b"CS_", b"PS_", b"VS_", b"MS_", b"AS_", b"GS_", b"HS_", b"DS_"];
    let ident = |b: u8| b.is_ascii_alphanumeric() || b == b'_';
    for at in 0..container.len().saturating_sub(6) {
        if (at > 0 && ident(container[at - 1])) || !STAGES.iter().any(|s| container[at..].starts_with(s)) {
            continue;
        }
        let end = container[at..].iter().position(|&b| !ident(b)).map_or(container.len(), |n| at + n);
        if end - at >= 6 {
            return Some(String::from_utf8_lossy(&container[at..end]).into_owned());
        }
    }
    None
}

/// The shared container for `bytes`.
fn shader(stage: &'static str, bytes: &[u8]) -> Arc<Shader> {
    let full = blake3::hash(bytes);
    let mut shaders = SHADERS.lock().unwrap_or_else(|e| e.into_inner());
    shaders
        .get_or_insert_with(HashMap::new)
        .entry(*full.as_bytes())
        .or_insert_with(|| {
            KEPT[0].fetch_add(1, std::sync::atomic::Ordering::Relaxed);
            KEPT[1].fetch_add(bytes.len() as u64, std::sync::atomic::Ordering::Relaxed);
            Arc::new(Shader {
                stage,
                hash: full.to_hex()[..16].to_owned(),
                container: bytes.to_vec(),
                #[cfg(windows)]
                ambient: (stage == "compute").then(|| crate::ambient::role(bytes)).flatten(),
                #[cfg(windows)]
                translated: OnceLock::new(),
            })
        })
        .clone()
}

/// A translated shader and the console resources its code reads.
#[cfg(windows)]
pub(crate) struct Translated {
    pub(crate) host: leonida_shader::emit::HostShader,
    /// A compute program's threads in a group.
    pub(crate) threads: Option<[u32; 3]>,
    pub(crate) used: Vec<leonida_shader::UsedResource>,
    pub(crate) tables: crate::draw::TablePlan,
    /// Whether the shader's code can store to buffer memory ([`stores`]);
    /// true when its code is not known.
    pub(crate) stores: bool,
    /// Whether it can store through raw addresses ([`stores_by_address`]).
    pub(crate) stores_by_address: bool,
    /// Whether it can store to buffer memory otherwise than through a buffer view
    /// ([`stores_past_views`]); true when its code is not known.
    pub(crate) stores_past_views: bool,
    /// Whether its code can write through unordered access at all: a store
    /// to buffer memory ([`stores`]) or to a texture ([`image_stores`]); true
    /// when its code is not known. What a later wait for every shader write
    /// has to wait for (`crate::draw::State::wait_for_writes`).
    pub(crate) writes: bool,
    /// A compute program translated to run several console groups to a host group
    /// (`LEONIDA_GROUP_PACK`): where its console grid goes (`crate::draw::pack_of`).
    pub(crate) pack: Option<crate::draw::PackWords>,
    /// Whether a bound packed program was logged (`crate::draw::pack_of`).
    pub(crate) pack_logged: std::sync::atomic::AtomicBool,
    /// Whether it stores through a view it picks from the heap by an index,
    /// once asked (`crate::picked`).
    pub(crate) picks: std::sync::atomic::AtomicU8,
    /// Whether all it stores to buffers goes through views bound to it, once asked
    /// ([`stores_bound`]).
    pub(crate) bound: std::sync::atomic::AtomicU8,
}

#[cfg(windows)]
impl Shader {
    /// The translation, made on first use; a shader that does not
    /// translate is logged once.
    pub(crate) fn translated(&self) -> Option<&Translated> {
        self.translated
            .get_or_init(|| {
                let done = translate(&self.container);
                // `LEONIDA_SHADER_SAVE=<dir>`: each shader as the title gave it (`.cdxil`) and as
                // translated (`.dxil`), by the hash crash reports name it with
                if let Some(dir) = std::env::var_os("LEONIDA_SHADER_SAVE") {
                    let dir = std::path::PathBuf::from(dir);
                    let _ = std::fs::create_dir_all(&dir);
                    let stem = format!("{}.{}", self.hash, self.stage);
                    let _ = std::fs::write(dir.join(format!("{stem}.cdxil")), &self.container);
                    if let Ok(t) = &done {
                        let _ = std::fs::write(dir.join(format!("{stem}.dxil")), &t.host.dxil);
                    }
                }
                if let Ok(t) = &done {
                    KEPT[2].fetch_add(1, std::sync::atomic::Ordering::Relaxed);
                    KEPT[3].fetch_add(t.host.dxil.len() as u64, std::sync::atomic::Ordering::Relaxed);
                }
                match &done {
                    Ok(t) => tracing::info!(
                        stage = self.stage,
                        hash = %self.hash,
                        user_data = t.host.user_data.len(),
                        samplers = t.host.samplers.len(),
                        "shader translated"
                    ),
                    Err(e) => tracing::warn!(
                        stage = self.stage,
                        hash = %self.hash,
                        error = %e,
                        "shader does not translate; its pipelines' draws are dropped"
                    ),
                }
                done.ok()
            })
            .as_ref()
    }
}

#[cfg(windows)]
impl Shader {
    /// The console programs of the container; `None` if they are not known.
    pub(crate) fn programs(&self) -> Option<Vec<leonida_shader::NativeProgram>> {
        use leonida_shader::{Container, FourCC};
        let c = Container::parse(&self.container).ok()?;
        let part = c.part(FourCC::DXIL).or(c.part(FourCC::ACCC))?;
        leonida_shader::native_programs(part.data).ok()
    }
}

#[cfg(windows)]
fn translate(container: &[u8]) -> Result<Translated, String> {
    use leonida_shader::{Container, FourCC, HwStage};
    // The translation depends on it (a view index past a capped heap).
    crate::descriptor::host::probe_view_limit();
    let c = Container::parse(container).map_err(|e| e.to_string())?;
    let programs = match c.part(FourCC::DXIL).or(c.part(FourCC::ACCC)) {
        Some(part) => leonida_shader::native_programs(part.data).map_err(|e| e.to_string())?,
        None => Vec::new(),
    };
    let host = crate::shader_cache::host_shader(container)?;
    let stores = programs.is_empty() || programs.iter().any(stores);
    let threads = programs.iter().find_map(|p| match &p.io {
        leonida_shader::native::StageIo::Cs(cs) if cs.threads.iter().all(|&n| n > 0) => Some(cs.threads),
        _ => None,
    });
    let stores_by_address = programs.is_empty() || programs.iter().any(stores_by_address);
    let stores_past_views = programs.is_empty() || programs.iter().any(stores_past_views);
    let writes = stores || programs.iter().any(image_stores);
    let used = programs
        .into_iter()
        .find(|p| {
            matches!(
                p.stage,
                HwStage::Vs | HwStage::Ps | HwStage::Ms | HwStage::Cs
            )
        })
        .map(|p| p.used)
        .unwrap_or_default();
    let tables = crate::draw::TablePlan::new(&used, &host.root_indexed_views);
    let pack = crate::draw::pack_in(&host.user_data);
    // A program that carries any of the console grid's words but not all of them (or no packing),
    // would have every thread leave at once: it does not translate.
    if pack.is_none() && crate::draw::has_pack_words(&host.user_data) {
        return Err("a packed program without the console grid's three words (LEONIDA_GROUP_PACK)".to_owned());
    }
    Ok(Translated {
        host,
        threads,
        used,
        tables,
        stores,
        stores_by_address,
        stores_past_views,
        writes,
        pack,
        pack_logged: Default::default(),
        picks: Default::default(),
        bound: Default::default(),
    })
}

/// The compute pipelines of the console's ray-tracing structure builder
/// (`XboxBvhBuilder.dll`; its programs are the ones with a spilled
/// user-data table) run **all or none**. Its stages feed each other
/// through memory: a stage that runs on what a dropped stage never wrote
/// computes its store offsets from garbage. The structures lie in plain
/// title memory, between the title's own objects, and a root view has no
/// size, so those stores land on the neighbours (run 98, with 14 of 35
/// stages translated: the title died within a second of the first builder
/// submission, on pointers that had become bounding-box bit patterns).
///
/// So a builder dispatch runs only while every builder pipeline the title
/// has created has a host pipeline; the first builder dispatch makes them
/// all on the spot instead of over the next frames (a stage "still being
/// made" would be a dropped stage). While one cannot be made the builder
/// is off; it is looked at again when the title creates another. A stage
/// that could not be bound stops the stages after it for the rest of that
/// frame and the next ([`builder_rests`]); it turned the builder off for the
/// rest of the run at first.
///
/// shortcut: and only with `LEONIDA_BVH_BUILDER=1`, until a run has shown
/// the whole builder and the host side of title memory by address
/// (`memory.rs`) working on a card; nothing reads the structures before
/// the programs that walk them translate.
#[cfg(windows)]
struct Builder {
    pipelines: Vec<Arc<make::Pending>>,
    /// Every one of `pipelines` has a host pipeline.
    whole: bool,
    /// One could not be made, of so many (0: none).
    off: usize,
    /// No stage runs before this frame.
    rests: u64,
}

#[cfg(windows)]
static BUILDER: Mutex<Builder> = Mutex::new(Builder {
    pipelines: Vec::new(),
    whole: false,
    off: 0,
    rests: 0,
});

/// Whether `shaders` are a pipeline of the structure builder.
#[cfg(windows)]
fn of_builder(shaders: &[Arc<Shader>]) -> bool {
    use leonida_shader::{Container, FourCC};
    let [shader] = shaders else { return false };
    if shader.stage != "compute" {
        return false;
    }
    let Ok(c) = Container::parse(&shader.container) else {
        return false;
    };
    c.part(FourCC::DXIL)
        .and_then(|part| leonida_shader::native_programs(part.data).ok())
        .is_some_and(|programs| programs.iter().any(leonida_shader::spill::is_spilled))
}

/// `LEONIDA_BVH_QUARANTINE=1`: what the structure builder's stages store
/// stays in the host's copies and is not copied back into title memory. A
/// check for a first run of the whole builder: a stage that stores where
/// it should not can then not reach the title's own objects (run 98), and
/// the programs that walk the structures read the host's copies anyway.
/// shortcut: not a way to run: the title also reads results back (sizes,
/// counters) and gets none, and a copy's pages are overwritten from title
/// memory when the title writes them.
#[cfg(windows)]
pub(crate) fn builder_quarantined(this: This) -> bool {
    static ON: OnceLock<bool> = OnceLock::new();
    is_builder(this) && *ON.get_or_init(|| std::env::var_os("LEONIDA_BVH_QUARANTINE").is_some_and(|v| v == "1"))
}

/// Whether the title's pipeline `this` is one of the structure builder's.
#[cfg(windows)]
pub(crate) fn is_builder(this: This) -> bool {
    !this.is_null() && pipeline(this).builder
}

/// A stage of the builder was dropped after all: none runs for the rest of
/// this frame and the next (what it fed is not there).
#[cfg(windows)]
pub(crate) fn builder_rests(why: &'static str) {
    let mut b = BUILDER.lock().unwrap_or_else(|e| e.into_inner());
    b.rests = crate::frame::current() + 2;
    static SAID: std::sync::atomic::AtomicU32 = std::sync::atomic::AtomicU32::new(0);
    if SAID.fetch_add(1, std::sync::atomic::Ordering::Relaxed) < 20 {
        tracing::warn!(why, "the ray-tracing structure builder rests for this frame and the next: its stages run all or none");
    }
}

/// At a dispatch of a builder pipeline: whether the builder runs (see
/// [`Builder`]).
#[cfg(windows)]
pub(crate) fn builder_runs() -> bool {
    static ALLOWED: OnceLock<bool> = OnceLock::new();
    if !*ALLOWED.get_or_init(|| std::env::var_os("LEONIDA_BVH_BUILDER").is_some_and(|v| v == "1")) {
        return false;
    }
    let mut b = BUILDER.lock().unwrap_or_else(|e| e.into_inner());
    if b.off == b.pipelines.len() || crate::frame::current() < b.rests {
        return false;
    }
    if b.whole {
        return true;
    }
    let missing: Vec<String> = b.pipelines.iter().filter(|p| !make::now(p)).map(|p| make::hashes(p)).collect();
    if !missing.is_empty() {
        b.off = b.pipelines.len();
        tracing::warn!(
            missing = missing.len(),
            of = b.pipelines.len(),
            which = ?missing,
            "the ray-tracing structure builder stays off: not all of its programs have a host pipeline, and its stages run all or none"
        );
        return false;
    }
    b.off = 0;
    tracing::info!(
        pipelines = b.pipelines.len(),
        "the ray-tracing structure builder runs: all of its programs have host pipelines"
    );
    b.whole = true;
    true
}

/// Whether a console program can store to buffer memory: it has a buffer,
/// typed-buffer, scalar-memory, flat or global store or atomic, or code
/// that is not all known (an instruction of those kinds outside our table,
/// an indirect jump, code that does not decode). Stores to textures
/// (image instructions) and to the program's own scratch memory do not
/// count: neither reaches a buffer's host copy. After upstream #413,
/// which leaves scalar-memory stores out and counts image stores.
#[cfg(windows)]
fn stores(program: &leonida_shader::NativeProgram) -> bool {
    use leonida_shader::gfx10::{Encoding, decode};
    let Ok(code) = decode(&program.code) else {
        return true;
    };
    code.iter().any(|i| {
        let name = i.name();
        match i.encoding {
            Encoding::Mubuf
            | Encoding::Mtbuf
            | Encoding::Smem
            | Encoding::Flat
            | Encoding::Global => name.is_none_or(|n| {
                (n.contains("store") || n.contains("atomic")) && !n.contains("scratch")
            }),
            Encoding::Sop1 => name.is_none_or(|n| matches!(n, "s_setpc_b64" | "s_swappc_b64")),
            _ => false,
        }
    })
}

/// Whether a console program has an image store or atomic (it writes a
/// texture through unordered access), or code that does not decode.
/// After felicienradio/cold-project-vi 1141d35 (vitutu, from
/// ZigZagBoi1/ScarlettPort; GPL-3.0-or-later).
#[cfg(windows)]
fn image_stores(program: &leonida_shader::NativeProgram) -> bool {
    use leonida_shader::gfx10::{Encoding, decode};
    let Ok(code) = decode(&program.code) else {
        return true;
    };
    code.iter()
        .any(|i| matches!(i.encoding, Encoding::Mimg) && i.name().is_none_or(|n| n.contains("store") || n.contains("atomic")))
}

/// Whether a console program can store to memory through raw addresses
/// (`crate::memory`): a flat or global store or atomic, or code that does
/// not decode. The programs that only walk ray-tracing structures do not,
/// whatever they store through views.
#[cfg(windows)]
fn stores_by_address(program: &leonida_shader::NativeProgram) -> bool {
    use leonida_shader::gfx10::{Encoding, decode};
    let Ok(code) = decode(&program.code) else {
        return true;
    };
    code.iter().any(|i| {
        matches!(i.encoding, Encoding::Flat | Encoding::Global)
            && i.name()
                .is_none_or(|n| (n.contains("store") || n.contains("atomic")) && !n.contains("scratch"))
    })
}

/// Whether a console program can store to buffer memory otherwise than through a buffer view:
/// of what [`stores`] counts, anything but a buffer or typed-buffer store or atomic the table
/// knows (a scalar-memory, flat or global store, an instruction outside the table, an indirect
/// jump, code that does not decode). What a program stores through buffer views lands in the
/// views bound to it, whose ranges a list is told (`crate::shadow::Shadow::gpu_writes`); the
/// rest can land anywhere (`LEONIDA_INDIRECT_BATCH=4`, `crate::draw`).
#[cfg(windows)]
fn stores_past_views(program: &leonida_shader::NativeProgram) -> bool {
    use leonida_shader::gfx10::{Encoding, decode};
    let Ok(code) = decode(&program.code) else {
        return true;
    };
    code.iter().any(|i| {
        let name = i.name();
        match i.encoding {
            Encoding::Mubuf | Encoding::Mtbuf => name.is_none(),
            Encoding::Smem | Encoding::Flat | Encoding::Global => {
                name.is_none_or(|n| (n.contains("store") || n.contains("atomic")) && !n.contains("scratch"))
            }
            Encoding::Sop1 => name.is_none_or(|n| matches!(n, "s_setpc_b64" | "s_swappc_b64")),
            _ => false,
        }
    })
}

/// Whether everything a console program can store to buffers goes through views a list is told
/// of at every bind of it (`crate::shadow::Shadow::gpu_writes`; `LEONIDA_INDIRECT_BATCH=4`, where
/// a batch of indirect records stays open past such a program). Each buffer store or atomic has
/// to go through
///
/// - a descriptor loaded from a fixed place in a table (no offset the program computes), at the
///   first descriptor of an entry of its used-resource list that is an unordered access view:
///   the entries `crate::draw::State::tables` goes through (`readable`) at every bind, by the
///   first entry of each descriptor (`TableGroup::entry`), declaring the views of unordered
///   access; a range of several views names only its first, so a store past it does not count;
/// - or a descriptor built around a root view of unordered access, in a program whose user data
///   is not spilled: the host binds that root view and declares it to the end of its host copy
///   (`crate::draw::State::root_uav`); a spilled program reaches it through title memory by
///   address instead.
///
/// A store through a view picked from the view heap, from a table at an offset the program
/// computes, per lane out of memory (a waterfall loop), or built around another address, a
/// scalar, flat or global store ([`stores_past_views`]), code that does not decode or that
/// resource tracking does not follow: no. A program that stores to no buffer: yes.
#[cfg(windows)]
fn stores_in_bound_views(program: &leonida_shader::NativeProgram) -> bool {
    use leonida_shader::gfx10::Fields;
    use leonida_shader::native::{ResourceType, RootType};
    use leonida_shader::resource::{Role, Source};
    if !stores(program) {
        return true;
    }
    if stores_past_views(program) || leonida_shader::spill::is_spilled(program) {
        return false;
    }
    let Ok(p) = leonida_shader::spill::build(program) else {
        return false;
    };
    // (As `crate::picked`: the structure only where plain tracking needs it.)
    let plain = leonida_shader::resource::track(&p, &program.layout, program.stage, None);
    let res = plain.or_else(|_| {
        let st = leonida_shader::structure::structure(&p);
        leonida_shader::resource::track(&p, &program.layout, program.stage, st.as_ref().ok())
    });
    let Ok(res) = res else {
        return false;
    };
    let declared = |leaf: &Source| match leaf {
        Source::Loaded { load, first } => {
            let Some((_, a)) = res.scalar.iter().find(|(site, _)| site == load) else {
                return false;
            };
            let Some(rp) = program.layout.get(a.param) else {
                return false;
            };
            if rp.root != RootType::Table || a.dynamic.is_some() {
                return false;
            }
            // The descriptor (32 bytes each) and the first entry of the table that names it.
            let descriptor = (a.offset / 4 + u32::from(*first)) / 8;
            program
                .used
                .iter()
                .find(|u| u.root == RootType::Table && u.user_data == rp.user_data && u.table_offset_dwords / 8 == descriptor)
                .is_some_and(|u| u.kind == ResourceType::Uav)
        }
        Source::Built { address, .. } => program.layout.get(address.param).is_some_and(|rp| {
            rp.root == RootType::Descriptor
                && program.used.iter().find(|u| u.root == RootType::Descriptor && u.user_data == rp.user_data).is_some_and(|u| u.kind == ResourceType::Uav)
        }),
        _ => false,
    };
    // Every buffer store or atomic of the code, each with the descriptor tracking found for it
    // (one it did not find for, in a block it did not go through say, does not count).
    p.blocks.iter().enumerate().all(|(b, block)| {
        block.insts.iter().enumerate().all(|(i, inst)| {
            let name = inst.name();
            let stores = matches!(inst.fields, Fields::Buffer { .. }) && (name.contains("store") || name.contains("atomic"));
            let mut found = res.descriptors.iter().filter(|d| d.site == (b, i) && d.role == Role::Resource).peekable();
            !stores || found.peek().is_some() && found.all(|d| d.source.leaves().into_iter().all(declared))
        })
    })
}

/// [`stores_in_bound_views`] of every program of a shader that can store to buffers; and the one
/// that does must be the program whose used-resource list the host binds by ([`translate`]: the
/// first that is translated). No for code that is not known.
#[cfg(windows)]
fn bound_programs(all: &[leonida_shader::NativeProgram]) -> bool {
    use leonida_shader::HwStage;
    let first = all.iter().position(|p| matches!(p.stage, HwStage::Vs | HwStage::Ps | HwStage::Ms | HwStage::Cs));
    !all.is_empty() && all.iter().enumerate().all(|(i, p)| !stores(p) || (Some(i) == first && stores_in_bound_views(p)))
}

/// What [`stores_bound`] knows of a shader ([`Translated::bound`]).
#[cfg(windows)]
const BOUND_UNKNOWN: u8 = 0;
#[cfg(windows)]
const BOUND_ASKED: u8 = 1;
#[cfg(windows)]
const BOUND_YES: u8 = 2;
#[cfg(windows)]
const BOUND_NO: u8 = 3;

/// Shaders looked at by [`stores_bound`] since the last frame-rate line: those whose stores all go
/// through views bound to them, and the others.
#[cfg(windows)]
pub(crate) static BOUND_SAID: [std::sync::atomic::AtomicU64; 2] = [const { std::sync::atomic::AtomicU64::new(0) }; 2];

/// Whether all that `shader` (translated as `t`) can store to buffers goes through views a list
/// is told of at every bind ([`bound_programs`]). The first time it is asked its programs are
/// looked at on a thread of its own (resource tracking takes a good part of what a translation
/// takes, and the recording threads do not wait for it); until that answers, no.
#[cfg(windows)]
pub(crate) fn stores_bound(shader: &Arc<Shader>, t: &Translated) -> bool {
    use std::sync::atomic::Ordering::Relaxed;
    match t.bound.load(Relaxed) {
        BOUND_YES => true,
        BOUND_UNKNOWN => {
            if t.bound.compare_exchange(BOUND_UNKNOWN, BOUND_ASKED, Relaxed, Relaxed).is_ok() {
                ask_bound(shader.clone());
            }
            false
        }
        _ => false,
    }
}

#[cfg(windows)]
fn ask_bound(shader: Arc<Shader>) {
    use std::sync::atomic::Ordering::Relaxed;
    use std::sync::mpsc::{Sender, channel};
    static ASKS: OnceLock<Mutex<Sender<Arc<Shader>>>> = OnceLock::new();
    let asks = ASKS.get_or_init(|| {
        let (send, asks) = channel::<Arc<Shader>>();
        let _ = std::thread::Builder::new().name("leonida-bound".into()).spawn(move || {
            use windows_sys::Win32::System::Threading::{GetCurrentThread, SetThreadPriority, THREAD_PRIORITY_BELOW_NORMAL};
            // SAFETY: this thread's own.
            unsafe { SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL) };
            for shader in asks {
                let yes = shader.programs().is_some_and(|all| bound_programs(&all));
                BOUND_SAID[usize::from(!yes)].fetch_add(1, Relaxed);
                if let Some(t) = shader.translated() {
                    t.bound.store(if yes { BOUND_YES } else { BOUND_NO }, Relaxed);
                }
            }
        });
        Mutex::new(send)
    });
    // (If the thread could not be made, the shader stays asked: no.)
    let _ = asks.lock().unwrap_or_else(|e| e.into_inner()).send(shader);
}

#[repr(C)]
struct Pipeline {
    object: Object,
    packet: [u32; 8],
    /// Each used stage's container.
    shaders: Vec<Arc<Shader>>,
    /// Whether a list has bound it yet (its shaders are logged then).
    bound: std::sync::atomic::AtomicBool,
    /// What its host pipeline is made from, and the host pipeline once made
    /// (by a worker: the title may release the pipeline meanwhile).
    #[cfg(windows)]
    host: Arc<make::Pending>,
    /// One of the ray-tracing structure builder's ([`Builder`]).
    #[cfg(windows)]
    builder: bool,
    /// Whether the skip list leaves this pipeline out ([`is_skipped`]), kept
    /// from the first time it is asked ([`kept_answer`]).
    #[cfg(all(windows, feature = "diagnostics"))]
    skip: std::sync::atomic::AtomicU8,
    /// Which of the sets tried in turns name one of its shaders ([`skip_tried`]): a bit each,
    /// and bit 7 once asked.
    #[cfg(all(windows, feature = "diagnostics"))]
    skip_try: std::sync::atomic::AtomicU8,
}
const _: () = assert!(offset_of!(Pipeline, packet) == 0x10);

/// Stage names, in description order.
const GRAPHICS_STAGES: [&str; 5] = ["vertex", "pixel", "domain", "hull", "geometry"];

fn pipeline<'a>(this: This) -> &'a Pipeline {
    // SAFETY: the title passes one of our pipelines.
    unsafe { &*this.cast::<Pipeline>() }
}

/// List method: logs each pipeline's shaders the first time a list binds
/// it, so a run lists the shaders its frames need first.
pub(crate) fn bound(this: This) {
    if this.is_null() {
        return;
    }
    let p = pipeline(this);
    if p.bound.swap(true, std::sync::atomic::Ordering::Relaxed) {
        return;
    }
    let frame = crate::frame::current();
    for shader in &p.shaders {
        tracing::info!(pipeline = ?this, frame, stage = shader.stage, size = shader.container.len(), hash = %shader.hash, "first bind");
    }
}

/// The host pipeline of a title pipeline; `None` if one of its shaders does
/// not translate, the host refuses it, or it is still being made (its first
/// use starts that, see [`make`]).
#[cfg(windows)]
pub(crate) fn host_pipeline<'a>(this: This) -> Option<&'a host::HostPipeline> {
    if this.is_null() {
        return None;
    }
    make::ready(&pipeline(this).host)
}

/// LOCAL (fxceless a35c557393): as [`host_pipeline`], for a draw into views
/// of the formats `bound`: if one has another format than the pipeline
/// declares for its slot, the host pipeline made for the bound formats.
#[cfg(windows)]
pub(crate) fn host_pipeline_for<'a>(this: This, bound: &host::TargetFormats) -> Option<&'a host::HostPipeline> {
    if this.is_null() {
        return None;
    }
    let p = &pipeline(this).host;
    if p.formats != *bound && host::formats_differ(&p.formats, bound) {
        return make::for_formats(p, bound);
    }
    make::ready(p)
}

/// Why the title pipeline `this` has no host pipeline now, for the drop
/// counters (`crate::drops`): on a skip list, not made yet, or refused.
#[cfg(windows)]
pub(crate) fn why_gone(this: This) -> crate::drops::Why {
    use crate::drops::Why;
    if this.is_null() {
        return Why::Other;
    }
    match make::gone(&pipeline(this).host) {
        make::Gone::LeftOut => Why::SkipList,
        make::Gone::Making => Why::NotMade,
        make::Gone::Refused => Why::Refused,
    }
}

/// Host pipelines queued and not made yet.
#[cfg(windows)]
pub(crate) fn making() -> u64 {
    make::waiting()
}

/// Draws and dispatches dropped since the last [`take_drops`], by reason
/// (diagnostics).
#[cfg(all(windows, feature = "diagnostics"))]
static DROPS: Mutex<Option<HashMap<String, u64>>> = Mutex::new(None);

/// Shader hashes as printed in the pipeline diagnostics.
#[cfg(all(windows, feature = "diagnostics"))]
fn parse_skip_hashes(value: &str) -> Vec<String> {
    value
        .split(',')
        .map(str::trim)
        .filter(|hash| !hash.is_empty())
        .filter_map(|hash| {
            if hash.len() == 16 && hash.bytes().all(|byte| byte.is_ascii_hexdigit()) {
                Some(hash.to_ascii_lowercase())
            } else {
                tracing::warn!(
                    hash,
                    "LEONIDA_SKIP_PIPELINES expects 16-digit shader hashes"
                );
                None
            }
        })
        .collect()
}

#[cfg(all(windows, feature = "diagnostics"))]
fn skipped_shader(p: &Pipeline) -> Option<&str> {
    // Leonida: a set, not a list. The launcher skips some 1,034 ray-tracing shaders, and every draw and dispatch
    // asks here (diagnostics builds, which are the ones played): a scan of the list for each of the pipeline's
    // shaders was ~3,000 string compares a draw, 23% of the list threads' running time in a Series X frame of
    // ~6,000 draws (profile prof2, memmove/memcmp under prepare_for, dispatch and execute_indirect).
    static HASHES: OnceLock<std::collections::HashSet<String>> = OnceLock::new();
    let hashes = HASHES.get_or_init(|| {
        let value = std::env::var("LEONIDA_SKIP_PIPELINES").unwrap_or_default();
        parse_skip_hashes(&value).into_iter().collect()
    });
    if hashes.is_empty() {
        return None;
    }
    p.shaders
        .iter()
        .find(|shader| hashes.contains(&shader.hash))
        .map(|shader| shader.hash.as_str())
}

/// A yes or no asked once and kept in `cell` (0: not asked yet, 1: no, 2:
/// yes): `ask` runs the first time only. (Two threads that ask at once both
/// run it and store the same answer.)
#[cfg(any(all(windows, feature = "diagnostics"), test))]
fn kept_answer(cell: &std::sync::atomic::AtomicU8, ask: impl FnOnce() -> bool) -> bool {
    use std::sync::atomic::Ordering::Relaxed;
    match cell.load(Relaxed) {
        1 => false,
        2 => true,
        _ => {
            let answer = ask();
            cell.store(1 + u8::from(answer), Relaxed);
            answer
        }
    }
}

/// Timing experiment: the pipelines with a shader that `LEONIDA_SKIP_TRY_A`, `_B` or `_C` names
/// (hashes, as for `LEONIDA_SKIP_PIPELINES`) are left out while that switch is on. Each is a
/// switch of `LEONIDA_AB` (`LEONIDA_AB=15,LEONIDA_SKIP_TRY_A,LEONIDA_SKIP_TRY_B`) and does nothing
/// when it is not named there: a phase's frame time against the next one's says what those
/// programs cost the card in the frame. Their lines of a timing run do not say it: a command's
/// time there runs from the end of the command before it, so the first program behind the
/// geometry is charged with the card finishing the geometry. For timing only: the picture is
/// wrong while one is on, and what the programs left out make is stale for those after them.
#[cfg(all(windows, feature = "diagnostics"))]
fn skip_tried(p: &Pipeline) -> bool {
    use std::sync::atomic::Ordering::Relaxed;
    const NAMES: [&str; 3] = ["LEONIDA_SKIP_TRY_A", "LEONIDA_SKIP_TRY_B", "LEONIDA_SKIP_TRY_C"];
    // The sets that are switches of this run, each with its number among the switches.
    static SETS: OnceLock<Vec<(u32, std::collections::HashSet<String>)>> = OnceLock::new();
    let sets = SETS.get_or_init(|| {
        NAMES
            .iter()
            .filter_map(|name| {
                let bit = crate::ab::bit(name)?;
                let hashes: std::collections::HashSet<String> =
                    parse_skip_hashes(&std::env::var(name).unwrap_or_default()).into_iter().collect();
                (!hashes.is_empty()).then_some((bit, hashes))
            })
            .collect()
    });
    if sets.is_empty() {
        return false;
    }
    let mut mask = p.skip_try.load(Relaxed);
    if mask == 0 {
        mask = 0x80;
        for (n, (_, hashes)) in sets.iter().enumerate() {
            if p.shaders.iter().any(|shader| hashes.contains(&shader.hash)) {
                mask |= 1 << n;
            }
        }
        p.skip_try.store(mask, Relaxed);
    }
    mask & 0x7f != 0 && sets.iter().enumerate().any(|(n, (bit, _))| mask >> n & 1 != 0 && crate::ab::on(*bit))
}

/// Whether the skip list (`LEONIDA_SKIP_PIPELINES`) leaves the pipeline out.
/// Every draw, dispatch and indirect execution asks, on the threads that
/// record lists; each ask looked every shader of the pipeline up in the
/// list (a string hashed for each). The list is read once and a pipeline's
/// shaders never change, so the pipeline keeps its answer: one byte read.
/// `LEONIDA_SKIP_KEPT=0`: looked up each time, as before.
///
/// From aki-gta6/aki-runtime f8cdc2d (Aki-Developer), GPL-3.0 (there the
/// list can change while the title runs, and the answer is kept by the
/// list's count of changes).
#[cfg(all(windows, feature = "diagnostics"))]
pub(crate) fn is_skipped(this: This) -> bool {
    if this.is_null() {
        return false;
    }
    let p = pipeline(this);
    if skip_tried(p) {
        return true;
    }
    static KEPT: OnceLock<bool> = OnceLock::new();
    if !*KEPT.get_or_init(|| std::env::var_os("LEONIDA_SKIP_KEPT").is_none_or(|v| v != "0")) {
        return skipped_shader(p).is_some();
    }
    kept_answer(&p.skip, || skipped_shader(p).is_some())
}

/// Counts a draw or dispatch of `this` intentionally skipped or dropped
/// for want of a host pipeline (diagnostics).
#[cfg(all(windows, feature = "diagnostics"))]
pub(crate) fn note_drop(this: This) {
    let why = if this.is_null() {
        Some("no pipeline set".to_owned())
    } else {
        let p = pipeline(this);
        skipped_shader(p)
            .map(|hash| format!("skipped shader {hash}"))
            .or_else(|| make::missing(&p.host))
    };
    if let Some(why) = why {
        note_why(why);
    }
}

/// Counts a draw or dispatch dropped for `why` (diagnostics).
#[cfg(all(windows, feature = "diagnostics"))]
pub(crate) fn note_why(why: String) {
    crate::capture::op(0, || format!("DROPPED {why}"));
    let mut drops = DROPS.lock().unwrap_or_else(|e| e.into_inner());
    *drops.get_or_insert_with(HashMap::new).entry(why).or_default() += 1;
}

/// The drops counted since the last call, most first (diagnostics).
#[cfg(all(windows, feature = "diagnostics"))]
pub(crate) fn take_drops() -> Vec<(String, u64)> {
    let drops = DROPS.lock().unwrap_or_else(|e| e.into_inner()).take();
    let mut drops: Vec<(String, u64)> = drops.into_iter().flatten().collect();
    // Frame reports only show the first 40 reasons. Keep ties ordered so
    // a HashMap iteration order change cannot change which reasons appear.
    drops.sort_unstable_by(|a, b| b.1.cmp(&a.1).then_with(|| a.0.cmp(&b.0)));
    drops
}

/// The stage's container, if it has one.
fn copy(stage: &'static str, code: &Bytecode) -> Option<Arc<Shader>> {
    if code.code.is_null() || code.size == 0 {
        return None;
    }
    // SAFETY: the title passes `size` bytes of shader code.
    Some(shader(stage, unsafe {
        std::slice::from_raw_parts(code.code, code.size)
    }))
}

fn hand_out(
    shaders: Vec<Arc<Shader>>,
    #[cfg(windows)] graphics: Option<Box<host::GraphicsState>>,
    iid: *const Iid,
    out: *mut This,
) -> HRESULT {
    tracing::trace!(?shaders, "pipeline state");
    #[cfg(windows)]
    let host = Arc::new(make::Pending::new(shaders.clone(), graphics));
    #[cfg(windows)]
    known::note(host.key(), &shaders, host.graphics.as_deref());
    #[cfg(windows)]
    make::early(&host);
    #[cfg(windows)]
    let builder = of_builder(&shaders);
    #[cfg(windows)]
    if builder {
        // Another stage: the group is looked at again at its next dispatch.
        let mut b = BUILDER.lock().unwrap_or_else(|e| e.into_inner());
        b.pipelines.push(host.clone());
        b.whole = false;
    }
    let pipeline = Box::new(Pipeline {
        object: Object::new(&FUNCTIONS),
        packet: [0; 8],
        #[cfg(windows)]
        host,
        #[cfg(windows)]
        builder,
        shaders,
        bound: std::sync::atomic::AtomicBool::new(false),
        #[cfg(all(windows, feature = "diagnostics"))]
        skip: std::sync::atomic::AtomicU8::new(0),
        #[cfg(all(windows, feature = "diagnostics"))]
        skip_try: std::sync::atomic::AtomicU8::new(0),
    });
    object::hand_out(PIPELINE, pipeline, iid, &PIPELINE_IIDS, out)
}

/// Device method.
pub(crate) extern "system" fn create_graphics(
    _: This,
    desc: *const GraphicsHead,
    iid: *const Iid,
    out: *mut This,
) -> HRESULT {
    // SAFETY: the title passes a description (null is tolerated).
    let Some(desc) = (unsafe { desc.as_ref() }) else {
        return E_INVALIDARG;
    };
    let stages = GRAPHICS_STAGES.iter().zip(&desc.stages);
    let shaders = stages.filter_map(|(s, c)| copy(s, c)).collect();
    hand_out(
        shaders,
        #[cfg(windows)]
        None,
        iid,
        out,
    )
}

/// Device method.
pub(crate) extern "system" fn create_compute(
    _: This,
    desc: *const ComputeHead,
    iid: *const Iid,
    out: *mut This,
) -> HRESULT {
    // SAFETY: the title passes a description (null is tolerated).
    let Some(desc) = (unsafe { desc.as_ref() }) else {
        return E_INVALIDARG;
    };
    hand_out(
        copy("compute", &desc.stage).into_iter().collect(),
        #[cfg(windows)]
        None,
        iid,
        out,
    )
}

/// Device method 0x1f0: the plain compute description plus console
/// extensions `(desc, extension count, extensions, iid, out)`.
/// shortcut: the extensions (console shader settings) are ignored, as is
/// the rest of the description.
pub(crate) extern "system" fn create_compute_x(
    this: This,
    desc: *const ComputeHead,
    _extensions: u32,
    _extended: *const u8,
    iid: *const Iid,
    out: *mut This,
) -> HRESULT {
    create_compute(this, desc, iid, out)
}

/// A pipeline described as a stream: its size and the subobjects, each a
/// type then its data, padded to 8 bytes.
#[repr(C)]
pub(crate) struct StreamDesc {
    size: usize,
    stream: *const u8,
}

/// A subobject type's size (type included), and the stage it is if it is a
/// shader. The console numbers amplification and mesh shaders 23 and 24
/// (PC: 24 and 25); its own types from 25 on are not sized yet.
fn subobject(kind: u32) -> Option<(usize, Option<&'static str>)> {
    let stage = |name| Some((24, Some(name)));
    match kind {
        1 => stage("vertex"),
        2 => stage("pixel"),
        3 => stage("domain"),
        4 => stage("hull"),
        5 => stage("geometry"),
        6 => stage("compute"),
        23 => stage("amplification"),
        24 => stage("mesh"),
        0 => Some((16, None)),
        12 | 19 => Some((24, None)),
        7 | 15 => Some((40, None)),
        8 => Some((336, None)),
        10 => Some((48, None)),
        11 => Some((56, None)),
        17 => Some((16, None)),
        21 => Some((64, None)),
        22 => Some((32, None)),
        9 | 13 | 14 | 16 | 18 | 20 => Some((8, None)),
        _ => None,
    }
}

/// Device method: a pipeline from a stream; the shaders are kept, and the
/// graphics state for the host pipeline.
pub(crate) extern "system" fn create_stream(
    _: This,
    desc: *const StreamDesc,
    iid: *const Iid,
    out: *mut This,
) -> HRESULT {
    // SAFETY: the title passes a description (null is tolerated).
    let Some(desc) = (unsafe { desc.as_ref() }) else {
        return E_INVALIDARG;
    };
    if desc.stream.is_null() {
        return E_INVALIDARG;
    }
    let mut shaders = Vec::new();
    #[cfg(windows)]
    let mut graphics = host::GraphicsState::default();
    let mut at = 0;
    while at + 8 <= desc.size {
        // SAFETY: inside the stream the title passes.
        let kind = unsafe { desc.stream.add(at).cast::<u32>().read_unaligned() };
        let Some((size, stage)) = subobject(kind) else {
            tracing::warn!(
                kind,
                at,
                "pipeline subobject of unknown size; the rest is ignored"
            );
            break;
        };
        if let Some(stage) = stage {
            // SAFETY: a shader subobject has its bytecode at +8.
            let code = unsafe { &*desc.stream.add(at + 8).cast::<Bytecode>() };
            shaders.extend(copy(stage, code));
        }
        #[cfg(windows)]
        // SAFETY: a whole subobject of this kind lies at `at`.
        unsafe {
            graphics.read(kind, desc.stream.add(at))
        };
        at += size;
    }
    hand_out(
        shaders,
        #[cfg(windows)]
        Some(Box::new(graphics)),
        iid,
        out,
    )
}

extern "system" fn query(this: This, iid: *const Iid, out: *mut This) -> HRESULT {
    let found = object::is(iid, &PIPELINE_IIDS).then_some(this);
    object::answer(PIPELINE, iid, found, out)
}
extern "system" fn release(this: This) -> u32 {
    object::release_boxed::<Pipeline>(this)
}

/// IUnknown 3, object 4, device child 1, pipeline 3.
static FUNCTIONS: Table<24> = {
    let mut f = object::d3d12_object(object::missing_table::<PIPELINE, 24>());
    f[0] = query as *const ();
    f[2] = release as *const ();
    f[7] = object::parent_device as *const (); // 0x38
    Table(f)
};

#[cfg(windows)]
pub(crate) mod host;
#[cfg(windows)]
pub mod known;
#[cfg(windows)]
mod library;
#[cfg(windows)]
mod make;
#[cfg(windows)]
pub(crate) mod places;

/// Lets the pipeline library's mapped file go from memory when no pipeline
/// has loaded for a while (`library::trim`); called by the host's sweep.
#[cfg(windows)]
pub(crate) fn trim_library() {
    library::trim();
}

/// The pipeline library's part of the report when the host device is
/// removed (`library::removal_note`).
#[cfg(windows)]
pub(crate) fn library_removal_note() -> String {
    library::removal_note()
}

/// Keeps compiled host pipelines, and the list of title pipelines to start
/// early, in `dir` (a folder of the cache) across runs.
#[cfg(windows)]
pub fn use_pipeline_cache(dir: std::path::PathBuf) {
    library::use_pipeline_cache(dir);
}

/// Saves the pipeline cache now if anything was added (at the end of a run).
#[cfg(windows)]
pub fn save_pipeline_cache() {
    library::save(true);
}

/// Which stores [`stores_in_bound_views`] takes as declared at a bind (`LEONIDA_INDIRECT_BATCH=4`).
#[cfg(all(test, windows))]
mod bound_tests {
    use leonida_shader::native::{ComputeIo, HwStage, NativeProgram, ResourceType, RootParam, RootType, StageIo, UsedResource};

    fn used(kind: ResourceType, root: RootType, at: u32) -> UsedResource {
        UsedResource { kind, root, table_offset_dwords: at, lower_bound: 0, stride: 4, dwords: 4, user_data: 4, slot: 1 }
    }

    /// A compute program: constants in s[0:3], the second parameter (`root`) from s4, `code` and
    /// an end.
    fn program(root: RootType, used: Vec<UsedResource>, code: &[u32]) -> NativeProgram {
        let param = |root, user_data, sgprs, slot| RootParam { root, table_offset_dwords: 0, sgprs, base_register: 0, user_data, slot };
        let words: Vec<u32> = code.iter().copied().chain([0xBF81_0000]).collect(); // s_endpgm
        NativeProgram {
            stage: HwStage::Cs,
            wave64: false,
            vgprs: 22,
            layout: vec![param(RootType::Constants, 0, 4, 0), param(root, 4, if root == RootType::Table { 1 } else { 2 }, 1)],
            used,
            io: StageIo::Cs(ComputeIo { rsrc2: 5 << 1, threads: [32, 1, 1] }),
            code: words.iter().flat_map(|w| w.to_le_bytes()).collect(),
        }
    }

    /// v[13:16] stored at 16 bytes a thread through the descriptor loaded from the table at byte
    /// `at` (with s0 added: `computed`).
    fn table_store(at: u32, computed: bool) -> Vec<u32> {
        let soffset = if computed { 0 } else { 0x7D << 25 };
        vec![
            0xBE85_0380, // s_mov_b32 s5, 0
            0xF408_0302,
            soffset | at, // s_load_dwordx4 s[12:15], s[4:5], at (+ s0)
            0x342A_0084, // v_lshlrev_b32 v21, 4, v0
            0xE078_1000,
            0x8003_0D15, // buffer_store_dwordx4 v[13:16], v21, s[12:15], 0 offen
        ]
    }

    /// The same through a descriptor built around the root view in s[4:5].
    const ROOT_STORE: [u32; 6] = [
        0xBE86_03C1, // s_mov_b32 s6, -1
        0xBE87_03FF,
        0x0002_7FAC, // s_mov_b32 s7, 0x27fac
        0x342A_0084, // v_lshlrev_b32 v21, 4, v0
        0xE078_1000,
        0x8001_0D15, // buffer_store_dwordx4 v[13:16], v21, s[4:7], 0 offen
    ];

    #[test]
    fn stores_through_bound_views_are_told_from_the_others() {
        use ResourceType::{Srv, Uav};
        use RootType::{Descriptor, Table};
        let yes = |p: NativeProgram| super::stores_in_bound_views(&p);
        // Descriptor 1 of the table, the first of an entry of unordered access: declared at every bind.
        assert!(yes(program(Table, vec![used(Srv, Table, 0), used(Uav, Table, 8)], &table_store(0x20, false))));
        // The same entry named as read only: the bind declares nothing for it.
        assert!(!yes(program(Table, vec![used(Srv, Table, 0), used(Srv, Table, 8)], &table_store(0x20, false))));
        // Descriptor 2, inside a range that names only its first (1): not gone through.
        assert!(!yes(program(Table, vec![used(Uav, Table, 8)], &table_store(0x40, false))));
        // At an offset the program computes: any of the table's.
        assert!(!yes(program(Table, vec![used(Uav, Table, 8)], &table_store(0x20, true))));
        // Through a root view of unordered access: declared to the end of its copy.
        assert!(yes(program(Descriptor, vec![used(Uav, Descriptor, 0)], &ROOT_STORE)));
        assert!(!yes(program(Descriptor, vec![used(Srv, Descriptor, 0)], &ROOT_STORE)));
        // A program that stores nothing.
        assert!(yes(program(Table, vec![used(Uav, Table, 8)], &[0x342A_0084])));
        // Code that does not decode.
        assert!(!yes(program(Table, vec![used(Uav, Table, 8)], &[0xFFFF_FFFF])));
        // Of a shader's programs, the one that stores must be the one the host binds by.
        let bound = program(Table, vec![used(Srv, Table, 0), used(Uav, Table, 8)], &table_store(0x20, false));
        assert!(super::bound_programs(std::slice::from_ref(&bound)));
        assert!(!super::bound_programs(&[]));
        let mut ps = bound.clone();
        ps.stage = HwStage::Ps;
        assert!(!super::bound_programs(&[bound.clone(), ps]), "a second storing program the host does not bind by");
    }
}

#[cfg(test)]
mod kept_tests {
    /// A pipeline's skip answer is asked for once, whichever it is.
    #[test]
    fn an_answer_is_asked_for_once_and_kept() {
        use std::sync::atomic::AtomicU8;
        for answer in [false, true] {
            let cell = AtomicU8::new(0);
            let asked = std::cell::Cell::new(0);
            for _ in 0..5 {
                let got = super::kept_answer(&cell, || {
                    asked.set(asked.get() + 1);
                    answer
                });
                assert_eq!(got, answer);
            }
            assert_eq!(asked.get(), 1);
        }
    }
}

#[cfg(test)]
mod tests {
    #[cfg(all(windows, feature = "diagnostics"))]
    #[test]
    fn skip_hashes_accepts_full_hashes_only() {
        assert!(super::parse_skip_hashes(" , ").is_empty());
        assert_eq!(
            super::parse_skip_hashes(" 0123456789ABCDEF, 0123456789abcde, xyz, 0011223344556677,"),
            ["0123456789abcdef", "0011223344556677"]
        );
    }

    use super::*;
    use crate::object::{S_OK, slot};

    fn shaders(p: This) -> Vec<(&'static str, Vec<u8>)> {
        pipeline(p)
            .shaders
            .iter()
            .map(|s| (s.stage, s.container.clone()))
            .collect()
    }

    #[test]
    fn pipelines_keep_their_shaders() {
        let (vs, ps) = ([1u8, 2, 3], [4u8; 5]);
        let none = || Bytecode {
            code: std::ptr::null(),
            size: 0,
        };
        let desc = GraphicsHead {
            root: std::ptr::null(),
            stages: [
                Bytecode {
                    code: vs.as_ptr(),
                    size: vs.len(),
                },
                Bytecode {
                    code: ps.as_ptr(),
                    size: ps.len(),
                },
                none(),
                none(),
                none(),
            ],
        };
        let mut p = std::ptr::null();
        assert_eq!(
            create_graphics(std::ptr::null(), &desc, &PIPELINE_IIDS[0], &mut p),
            S_OK
        );
        assert_eq!(
            shaders(p),
            [("vertex", vs.to_vec()), ("pixel", ps.to_vec())]
        );
        // A second pipeline with the same pixel shader shares its container.
        let mut q = std::ptr::null();
        create_graphics(std::ptr::null(), &desc, &PIPELINE_IIDS[0], &mut q);
        assert!(Arc::ptr_eq(
            &pipeline(p).shaders[1],
            &pipeline(q).shaders[1]
        ));
        assert_eq!(slot::<extern "system" fn(This) -> u32>(p, 0x10)(p), 0);
    }

    /// The stream layout the title builds (title+0x86f090): root
    /// signature, five shader slots (the first a mesh shader), blend, sample
    /// mask, rasterizer, depth-stencil 1, sample description, input layout,
    /// topology, render target and depth formats: 0x2b0 bytes.
    #[test]
    fn streams_yield_their_shaders() {
        let (ms, ps) = ([9u8; 7], [4u8; 5]);
        let mut stream = vec![0u8; 0x2b0];
        let mut put = |at: usize, v: u64| stream[at..at + 8].copy_from_slice(&v.to_le_bytes());
        for (at, kind) in [
            (0x10, 24),
            (0x28, 2),
            (0x40, 3),
            (0x58, 4),
            (0x70, 5),
            (0x88, 8),
            (0x1d8, 9),
            (0x1e0, 10),
            (0x210, 21),
            (0x250, 17),
            (0x260, 12),
            (0x278, 14),
            (0x280, 15),
            (0x2a8, 16),
        ] {
            put(at, kind);
        }
        put(0x18, ms.as_ptr() as u64);
        put(0x20, ms.len() as u64);
        put(0x30, ps.as_ptr() as u64);
        put(0x38, ps.len() as u64);
        // Topology type 3 (triangles), one render target of format 28.
        put(0x278, 14 | 3 << 32);
        put(0x280, 15 | 28 << 32);
        put(0x2a0, 1 << 32);
        let desc = StreamDesc {
            size: stream.len(),
            stream: stream.as_ptr(),
        };
        let mut p = std::ptr::null();
        let hr = create_stream(std::ptr::null(), &desc, &PIPELINE_IIDS[0], &mut p);
        assert_eq!(hr, S_OK);
        assert_eq!(shaders(p), [("mesh", ms.to_vec()), ("pixel", ps.to_vec())]);
        #[cfg(windows)]
        {
            let g = pipeline(p).host.graphics.as_deref().unwrap();
            assert_eq!(g.topology.0, 3);
            assert_eq!(
                (g.targets.NumRenderTargets, g.targets.RTFormats[0].0),
                (1, 28)
            );
        }
    }
}
