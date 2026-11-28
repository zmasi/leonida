//! The places of static samplers in the NVIDIA driver's table, made the same
//! in every run, so that host pipelines with static samplers come from the
//! pipeline library again ([`super::library`]).
//!
//! What the driver does (RTX 4060 Ti, driver 616.64, 2026-11-27; the tests
//! below show each point on the card, `docs/findings.md` has the figures):
//!
//! - A device has one table of static samplers. `CreateRootSignature` gives
//!   each static sampler it has not seen the lowest free place, in the order
//!   of the root signature's array; a sampler alike in all but register,
//!   space and visibility shares the place. The place is free again when
//!   the last root signature with that sampler is released. Samplers made
//!   in descriptor heaps are not in the table.
//! - A compiled pipeline holds those places, not the samplers. The driver's
//!   own cache on disk knows that (a pipeline whose samplers have other
//!   places is compiled again); a pipeline from an `ID3D12PipelineLibrary`
//!   is taken as it was stored, and in a process whose table is laid out
//!   otherwise it samples through whatever is at the stored places: another
//!   filter, address mode or level. Graphics and compute pipelines alike,
//!   by `LoadGraphicsPipeline`, `LoadComputePipeline` and `LoadPipeline`.
//! - Which sampler a title pipeline needs first differs from run to run
//!   (eight workers make them), so the places did, and so nearly every
//!   pipeline with static samplers was compiled again at every start: some
//!   1,650 of them, five minutes of all workers.
//!
//! What is done about it:
//!
//! 1. The samplers seen so far are listed in a file beside the library
//!    ([`FILE`]), in the order they were first seen; the file only grows.
//!    When the host device is made, before anything else makes a static
//!    sampler on it, a root signature is made for each in that order and
//!    kept for the whole run ([`Places::start`]). A sampler's place then
//!    follows from the list up to it and from nothing else.
//! 2. A pipeline all of whose static samplers were in the list at the start
//!    is kept in the library under a name that also holds the list up to its
//!    last sampler ([`Places::key`]): a list that was lost or begun anew
//!    names it otherwise. A pipeline with a sampler first seen in this run
//!    is made by the driver, as before, and the sampler joins the list for
//!    the next run. (It is given the place it will have then, where that
//!    can be done, so that the driver's own cache has the pipeline for that
//!    run: [`SPARE`]. Nothing rests on it.)
//! 3. Before the first such pipeline is looked up, the places are checked
//!    on the card ([`Places::check`]): for every listed sampler, and one of
//!    ours made before and one made after them, a small pipeline kept in
//!    the library by an earlier run is loaded and must sample exactly as
//!    the same pipeline made now does. If one does not (something made
//!    static samplers on the device before us), pipelines with static
//!    samplers are made by the driver for this run, as before.
//!
//! Only on NVIDIA: the other drivers' pipelines keep their names and the
//! library. `LEONIDA_LIBRARY_STATIC_SAMPLERS=0`: none of this; pipelines
//! with static samplers are made by the driver every run, as before.
//! `=1`: they are kept in the library under their plain names, with no
//! care for the places (for comparison: that is the fault).

use std::collections::{HashMap, HashSet};
use std::path::{Path, PathBuf};
use std::sync::{Mutex, OnceLock};

use windows::Win32::Graphics::Direct3D::ID3DBlob;
use windows::Win32::Graphics::Direct3D12::*;
use windows::Win32::Graphics::Dxgi::Common::*;
use windows::Win32::System::Threading::{CreateEventW, WaitForSingleObject};
use windows::core::{Interface, s};

use super::library::Library;

/// The list of static samplers, in the folder of the pipeline library.
const FILE: &str = "static-samplers.txt";
/// The file's first line, before the list's own name.
const HEAD: &str = "leonida static samplers 1 ";
/// Samplers listed, at most. The driver's table held 2,032 on an RTX 4070
/// SUPER (trustno1-coder/VIctory-NVIDIA 7fa0e95: the next removes the
/// device); half of it is left to everything else.
const MOST: usize = 1000;

/// A static sampler as the driver tells them apart: its description
/// without register, space and visibility.
#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
pub(super) struct Sampler([u32; 10]);

impl Sampler {
    pub(super) fn of(d: &D3D12_STATIC_SAMPLER_DESC) -> Sampler {
        // The numbers bit for bit: what the driver may tell apart is told
        // apart here.
        Sampler([
            d.Filter.0 as u32,
            d.AddressU.0 as u32,
            d.AddressV.0 as u32,
            d.AddressW.0 as u32,
            d.MipLODBias.to_bits(),
            d.MaxAnisotropy,
            d.ComparisonFunc.0 as u32,
            d.BorderColor.0 as u32,
            d.MinLOD.to_bits(),
            d.MaxLOD.to_bits(),
        ])
    }

    /// At register 0 of space 0, for every stage.
    fn desc(&self) -> D3D12_STATIC_SAMPLER_DESC {
        let w = &self.0;
        D3D12_STATIC_SAMPLER_DESC {
            Filter: D3D12_FILTER(w[0] as i32),
            AddressU: D3D12_TEXTURE_ADDRESS_MODE(w[1] as i32),
            AddressV: D3D12_TEXTURE_ADDRESS_MODE(w[2] as i32),
            AddressW: D3D12_TEXTURE_ADDRESS_MODE(w[3] as i32),
            MipLODBias: f32::from_bits(w[4]),
            MaxAnisotropy: w[5],
            ComparisonFunc: D3D12_COMPARISON_FUNC(w[6] as i32),
            BorderColor: D3D12_STATIC_BORDER_COLOR(w[7] as i32),
            MinLOD: f32::from_bits(w[8]),
            MaxLOD: f32::from_bits(w[9]),
            ShaderRegister: 0,
            RegisterSpace: 0,
            ShaderVisibility: D3D12_SHADER_VISIBILITY_ALL,
        }
    }

    /// Its line in the file: ten words of eight digits.
    fn line(&self) -> String {
        self.0.map(|w| format!("{w:08x}")).join(" ")
    }

    /// A whole line and nothing else: a line cut short (a run that ended
    /// while writing it) is no sampler.
    fn parse(line: &str) -> Option<Sampler> {
        let mut words = [0u32; 10];
        let mut parts = line.split(' ');
        for word in &mut words {
            let part = parts.next()?;
            if part.len() != 8 || !part.bytes().all(|b| b.is_ascii_hexdigit()) {
                return None;
            }
            *word = u32::from_str_radix(part, 16).ok()?;
        }
        parts.next().is_none().then_some(Sampler(words))
    }

    /// Whether it compares (`D3D12_FILTER_REDUCTION_TYPE_COMPARISON`).
    fn compares(&self) -> bool {
        self.0[0] >> 7 & 3 == 1
    }

    fn bias(&self) -> f32 {
        f32::from_bits(self.0[4])
    }
}

/// A sampler of ours no title has: made before the list ([`FRONT`]) and
/// after it ([`END`]), and looked at by the check like the others. With one
/// place taken before us the first is not where it was; with one more or
/// one fewer place taken up to the end of the list the second is not.
const fn marker(bias: f32) -> Sampler {
    Sampler([
        D3D12_FILTER_MIN_MAG_MIP_LINEAR.0 as u32,
        D3D12_TEXTURE_ADDRESS_MODE_MIRROR.0 as u32,
        D3D12_TEXTURE_ADDRESS_MODE_BORDER.0 as u32,
        D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE.0 as u32,
        bias.to_bits(),
        1,
        D3D12_COMPARISON_FUNC_NEVER.0 as u32,
        D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE.0 as u32,
        0.25f32.to_bits(),
        13.5f32.to_bits(),
    ])
}
const FRONT: Sampler = marker(5.335_937_5);
const END: Sampler = marker(6.664_062_5);

/// Places kept free after the list for the samplers a run meets first:
/// samplers of ours ([`spare`]) made after the list and let go one by one,
/// each just before such a sampler is made, which then gets the place (the
/// lowest free one) it will have in the next run, when it is in the list.
/// Whatever else makes static samplers meanwhile comes after them. The
/// pipelines of such a sampler are the driver's in this run either way;
/// with the place right, the driver's cache has them in the next, when
/// they are made once more to be stored.
const SPARE: usize = 64;

fn spare(n: usize) -> Sampler {
    marker(-15.0 + n as f32 / 16.0)
}

/// The list as a file holds it.
struct List {
    /// The list's own name, given when its file is begun: a list begun anew
    /// names every pipeline anew.
    id: String,
    samplers: Vec<Sampler>,
    at: HashMap<Sampler, usize>,
    /// What stands for the list before its first sampler, then up to and
    /// with each one.
    upto: Vec<blake3::Hash>,
}

impl List {
    fn new(id: String) -> List {
        let mut h = blake3::Hasher::new();
        h.update(HEAD.as_bytes());
        h.update(id.as_bytes());
        for w in FRONT.0 {
            h.update(&w.to_le_bytes());
        }
        List {
            id,
            samplers: Vec::new(),
            at: HashMap::new(),
            upto: vec![h.finalize()],
        }
    }

    /// The list in `text`; `None` if it does not begin as a list of ours.
    /// A line that is no sampler, or one already there, is passed over.
    fn parse(text: &str) -> Option<List> {
        let mut lines = text.lines();
        let id = lines.next()?.strip_prefix(HEAD)?;
        if id.is_empty() || !id.bytes().all(|b| b.is_ascii_hexdigit()) {
            return None;
        }
        let mut list = List::new(id.to_owned());
        for sampler in lines.filter_map(Sampler::parse) {
            if list.samplers.len() < MOST {
                list.push(sampler);
            }
        }
        Some(list)
    }

    fn push(&mut self, sampler: Sampler) {
        if self.at.contains_key(&sampler) {
            return;
        }
        let mut h = blake3::Hasher::new();
        h.update(self.upto[self.samplers.len()].as_bytes());
        for w in sampler.0 {
            h.update(&w.to_le_bytes());
        }
        self.upto.push(h.finalize());
        self.at.insert(sampler, self.samplers.len());
        self.samplers.push(sampler);
    }

    /// What names the places of `samplers`: the list up to the last of them
    /// (with none, before its first). `Err`: those that are not in the list.
    fn key(&self, samplers: impl Iterator<Item = Sampler>) -> Result<String, Vec<Sampler>> {
        let (mut upto, mut missing) = (0, Vec::new());
        for sampler in samplers {
            match self.at.get(&sampler) {
                Some(&at) => upto = upto.max(at + 1),
                None => missing.push(sampler),
            }
        }
        if missing.is_empty() {
            Ok(self.upto[upto].to_hex()[..32].to_owned())
        } else {
            Err(missing)
        }
    }
}

/// The static samplers made at the start of a device, and what the run
/// learns of others.
pub(super) struct Places {
    /// As it was read, and made, at the start.
    list: List,
    file: PathBuf,
    device: ID3D12Device,
    /// One root signature for each sampler ([`FRONT`], the list, [`END`]):
    /// they hold the places for as long as the device lives.
    _roots: Vec<ID3D12RootSignature>,
    met: Mutex<Met>,
    /// Whether the places are those the library's pipelines were made with
    /// ([`Places::check`]), once looked at.
    same: OnceLock<bool>,
}

/// What a run met that the list did not have at its start.
struct Met {
    /// Samplers this run wrote to the file.
    seen: HashSet<Sampler>,
    /// The places kept free that are left ([`SPARE`]), the next one last.
    spare: Vec<ID3D12RootSignature>,
    /// A root signature for each of `seen`, kept like the list's.
    roots: Vec<ID3D12RootSignature>,
}

// SAFETY: root signatures are free-threaded D3D12 objects, only held here.
unsafe impl Send for Places {}
// SAFETY: as above; the rest is plain data behind a lock.
unsafe impl Sync for Places {}

/// A root signature of the parameters every pipeline of ours here has (four
/// constants, a table of four textures, a buffer to write) and `samplers`;
/// with `bare`, of the samplers alone.
fn root(
    device: &ID3D12Device,
    samplers: &[D3D12_STATIC_SAMPLER_DESC],
    bare: bool,
) -> windows::core::Result<ID3D12RootSignature> {
    let range = D3D12_DESCRIPTOR_RANGE1 {
        RangeType: D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
        NumDescriptors: TEXTURES,
        BaseShaderRegister: 0,
        RegisterSpace: 0,
        Flags: D3D12_DESCRIPTOR_RANGE_FLAG_NONE,
        OffsetInDescriptorsFromTableStart: 0,
    };
    let params = probe_parameters(&range);
    let desc = D3D12_VERSIONED_ROOT_SIGNATURE_DESC {
        Version: D3D_ROOT_SIGNATURE_VERSION_1_1,
        Anonymous: D3D12_VERSIONED_ROOT_SIGNATURE_DESC_0 {
            Desc_1_1: D3D12_ROOT_SIGNATURE_DESC1 {
                NumParameters: if bare { 0 } else { params.len() as u32 },
                pParameters: params.as_ptr(),
                NumStaticSamplers: samplers.len() as u32,
                pStaticSamplers: samplers.as_ptr(),
                Flags: D3D12_ROOT_SIGNATURE_FLAG_NONE,
            },
        },
    };
    // SAFETY: the description points at live arrays; the blob is read in
    // full before it is released.
    unsafe {
        let mut blob: Option<ID3DBlob> = None;
        D3D12SerializeVersionedRootSignature(&desc, &mut blob, None)?;
        let blob = blob.ok_or_else(windows::core::Error::empty)?;
        let bytes =
            std::slice::from_raw_parts(blob.GetBufferPointer().cast::<u8>(), blob.GetBufferSize());
        device.CreateRootSignature(0, bytes)
    }
}

/// Textures a [`Probe`] binds.
const TEXTURES: u32 = 4;

/// The root parameters [`Probe::run`] sets: constants at `b0`, the table
/// `range` (textures from `t0`), a buffer to write at `u0`.
fn probe_parameters(range: &D3D12_DESCRIPTOR_RANGE1) -> [D3D12_ROOT_PARAMETER1; 3] {
    [
        D3D12_ROOT_PARAMETER1 {
            ParameterType: D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS,
            Anonymous: D3D12_ROOT_PARAMETER1_0 {
                Constants: D3D12_ROOT_CONSTANTS {
                    ShaderRegister: 0,
                    RegisterSpace: 0,
                    Num32BitValues: 4,
                },
            },
            ShaderVisibility: D3D12_SHADER_VISIBILITY_ALL,
        },
        D3D12_ROOT_PARAMETER1 {
            ParameterType: D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
            Anonymous: D3D12_ROOT_PARAMETER1_0 {
                DescriptorTable: D3D12_ROOT_DESCRIPTOR_TABLE1 {
                    NumDescriptorRanges: 1,
                    pDescriptorRanges: range,
                },
            },
            ShaderVisibility: D3D12_SHADER_VISIBILITY_ALL,
        },
        D3D12_ROOT_PARAMETER1 {
            ParameterType: D3D12_ROOT_PARAMETER_TYPE_UAV,
            Anonymous: D3D12_ROOT_PARAMETER1_0 {
                Descriptor: D3D12_ROOT_DESCRIPTOR1 {
                    ShaderRegister: 0,
                    RegisterSpace: 0,
                    Flags: D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE,
                },
            },
            ShaderVisibility: D3D12_SHADER_VISIBILITY_ALL,
        },
    ]
}

impl Places {
    /// The list kept in `dir`, each of its samplers made on `device` in the
    /// list's order. Called when the device is made, before anything else
    /// makes a static sampler on it. `None`: no list can be kept there, or
    /// the device refuses a sampler of ours; nothing is held then.
    pub(super) fn start(device: &ID3D12Device, dir: &Path) -> Option<Places> {
        let file = dir.join(FILE);
        let list = match std::fs::read_to_string(&file)
            .ok()
            .as_deref()
            .and_then(List::parse)
        {
            Some(list) => list,
            None => {
                // Begun anew, under a name no list had: the time and the
                // process.
                let now = std::time::SystemTime::now()
                    .duration_since(std::time::UNIX_EPOCH)
                    .map_or(0, |d| d.as_nanos());
                let mut h = blake3::Hasher::new();
                h.update(&now.to_le_bytes());
                h.update(&std::process::id().to_le_bytes());
                let list = List::new(h.finalize().to_hex()[..16].to_owned());
                let written = std::fs::create_dir_all(dir)
                    .and_then(|()| std::fs::write(&file, format!("{HEAD}{}\n", list.id)));
                if let Err(e) = written {
                    tracing::warn!(error = %e, "no list of static samplers can be kept: pipelines with static samplers are made by the driver every run");
                    return None;
                }
                list
            }
        };
        let ours = |sampler: Sampler| {
            root(device, &[sampler.desc()], true)
                .inspect_err(|e| tracing::warn!(error = %e, "a static sampler of ours was refused: pipelines with static samplers are made by the driver this run"))
                .ok()
        };
        let mut roots = Vec::with_capacity(list.samplers.len() + 2);
        roots.push(ours(FRONT)?);
        for sampler in &list.samplers {
            // One this device refuses (listed with another card in the PC)
            // is refused in every run here, and no pipeline has it: the
            // others' places are the same from run to run without it.
            match root(device, &[sampler.desc()], true) {
                Ok(root) => roots.push(root),
                Err(e) => {
                    tracing::warn!(error = %e, sampler = sampler.line(), "a listed static sampler was refused")
                }
            }
        }
        let mut spares = Vec::with_capacity(SPARE);
        for n in 0..SPARE {
            spares.push(ours(spare(n))?);
        }
        spares.reverse();
        roots.push(ours(END)?);
        tracing::info!(
            samplers = list.samplers.len(),
            "static samplers made in the order of their list"
        );
        Some(Places {
            list,
            file,
            device: device.clone(),
            _roots: roots,
            met: Mutex::new(Met {
                seen: HashSet::new(),
                spare: spares,
                roots: Vec::new(),
            }),
            same: OnceLock::new(),
        })
    }

    /// What goes into the library name of a pipeline with the static
    /// `samplers` (not none). `None`: one of them was not in the list at the
    /// start, so its place in this run may not be the one it will have; it
    /// is written to the file, and the pipeline is the driver's this run.
    /// Asked before the pipeline's root signature is made ([`SPARE`]).
    pub(super) fn key(&self, samplers: &[D3D12_STATIC_SAMPLER_DESC]) -> Option<String> {
        match self.list.key(samplers.iter().map(Sampler::of)) {
            Ok(key) => Some(key),
            Err(missing) => {
                self.learn(&missing);
                None
            }
        }
    }

    /// Writes those of `samplers` this run has not written yet to the file,
    /// and makes each at the next place kept free ([`SPARE`]).
    fn learn(&self, samplers: &[Sampler]) {
        use std::io::Write;
        let mut met = self.met.lock().unwrap_or_else(|e| e.into_inner());
        let mut lines = String::new();
        for sampler in samplers {
            if self.list.samplers.len() + met.seen.len() < MOST && met.seen.insert(*sampler) {
                lines.push_str(&sampler.line());
                lines.push('\n');
                // Its place is free from here to the next line.
                drop(met.spare.pop());
                if let Ok(root) = root(&self.device, &[sampler.desc()], true) {
                    met.roots.push(root);
                }
            }
        }
        if lines.is_empty() {
            return;
        }
        // Whole lines in one write, at the file's end whoever else writes.
        let written = std::fs::OpenOptions::new()
            .append(true)
            .open(&self.file)
            .and_then(|mut file| file.write_all(lines.as_bytes()));
        if let Err(e) = written {
            static ONCE: std::sync::Once = std::sync::Once::new();
            ONCE.call_once(|| tracing::warn!(error = %e, "static samplers first seen in this run could not be listed"));
        }
    }

    /// Whether the places are those the library's pipelines were made with:
    /// looked at once ([`Places::check`]), with the first pipeline that asks.
    pub(super) fn same(&self, device: &ID3D12Device, library: &Library) -> bool {
        *self.same.get_or_init(|| {
            let began = std::time::Instant::now();
            let ms = || began.elapsed().as_secs_f64() * 1e3;
            match self.check(device, library) {
                Ok(checked) if checked.differ.is_empty() => {
                    tracing::info!(
                        same = checked.same,
                        new = checked.new,
                        ms = ms(),
                        "static sampler places checked: pipelines with static samplers use the library"
                    );
                    true
                }
                Ok(checked) => {
                    tracing::warn!(
                        differ = checked.differ.len(),
                        first = checked.differ[0],
                        same = checked.same,
                        ms = ms(),
                        "static samplers are not at the places the library's pipelines were made with (something made static samplers on the device before the list): pipelines with static samplers are made by the driver this run. If every run says so, delete {FILE} in the pipeline cache folder: the next two runs make them anew"
                    );
                    false
                }
                Err(e) => {
                    tracing::warn!(error = %e, "static sampler places could not be checked: pipelines with static samplers are made by the driver this run");
                    false
                }
            }
        })
    }

    /// Looks at every place on the card. For each sampler made at the start
    /// a small pipeline that samples through it ([`PLAIN`], [`COMPARING`])
    /// is in `library` under a name that holds the list up to it; stored by
    /// the first run that had the sampler at the start, it holds the place
    /// the sampler had then. Loaded, it must write what the same pipeline
    /// made now (from code that differs by a number, so that it is not the
    /// loaded one again) writes. One that is not in the library yet is made
    /// and, if all the others agree, stored.
    fn check(&self, device: &ID3D12Device, library: &Library) -> windows::core::Result<Checked> {
        let probe = Probe::new(device)?;
        // The code stored and the code made now, for each kind of sampler.
        let mut codes: [Option<[Vec<u8>; 2]>; 2] = [None, None];
        // What is looked at: the sampler, and what names its place.
        let last = self.list.samplers.len();
        let mut end = blake3::Hasher::new();
        end.update(self.list.upto[last].as_bytes());
        end.update(b"end");
        let places = [(FRONT, self.list.upto[0])]
            .into_iter()
            .chain(
                self.list
                    .samplers
                    .iter()
                    .zip(&self.list.upto[1..])
                    .map(|(s, h)| (*s, *h)),
            )
            .chain([(END, end.finalize())]);
        let (mut runs, mut loaded, mut new, mut checked) =
            (Vec::new(), Vec::new(), Vec::new(), Checked::default());
        for (n, (sampler, upto)) in places.enumerate() {
            let kind = usize::from(sampler.compares());
            if codes[kind].is_none() {
                let source = if kind == 0 { PLAIN } else { COMPARING };
                let code = |number: u32| {
                    crate::host::compile(
                        &format!("#define NUMBER {number}.0\n{source}"),
                        s!("main"),
                        s!("cs_5_1"),
                    )
                };
                codes[kind] = Some([code(0)?, code(1)?]);
            }
            let [stored, now] = codes[kind]
                .as_ref()
                .ok_or_else(windows::core::Error::empty)?;
            let root = root(device, &[sampler.desc()], false)?;
            let desc = |code: &[u8]| D3D12_COMPUTE_PIPELINE_STATE_DESC {
                // SAFETY: a borrowed pointer the description never releases.
                pRootSignature: unsafe { std::mem::transmute_copy(&root) },
                CS: D3D12_SHADER_BYTECODE {
                    pShaderBytecode: code.as_ptr().cast(),
                    BytecodeLength: code.len(),
                },
                ..Default::default()
            };
            // The pattern's samples at the levels they name, whatever the
            // sampler adds to a level.
            let constants = |slot: usize| [(slot * SLOT) as u32, (-sampler.bias()).to_bits(), 0, 0];
            let name =
                super::library::name("static sampler place", upto.as_bytes(), &[&stored[..]], "");
            // SAFETY: the descriptions point at live data.
            match library.load(&name, |library, name| unsafe {
                library.LoadComputePipeline(name, &desc(stored))
            }) {
                Some(pso) => {
                    // SAFETY: as above.
                    let made: ID3D12PipelineState =
                        unsafe { device.CreateComputePipelineState(&desc(now)) }?;
                    runs.push(Run {
                        root: root.clone(),
                        pso: made,
                        constants: constants(2 * n),
                    });
                    runs.push(Run {
                        root,
                        pso,
                        constants: constants(2 * n + 1),
                    });
                    loaded.push(n);
                }
                None => {
                    // SAFETY: as above.
                    let pso: ID3D12PipelineState =
                        unsafe { device.CreateComputePipelineState(&desc(stored)) }?;
                    new.push((name, pso));
                }
            }
        }
        let wrote = if runs.is_empty() {
            Vec::new()
        } else {
            probe.run(&runs, 2 * (last + 2))?
        };
        for n in loaded {
            let at = |slot: usize| &wrote[slot * SLOT..slot * SLOT + COMPARED];
            if at(2 * n) == at(2 * n + 1) {
                checked.same += 1;
            } else {
                checked.differ.push(n);
            }
        }
        // Only what was made at the places of the others goes beside them:
        // in a run whose places differ, these would hold the wrong ones for
        // every run after it.
        if checked.differ.is_empty() {
            for (name, pso) in &new {
                library.store(name, pso);
            }
            checked.new = new.len();
        }
        Ok(checked)
    }
}

/// What [`Places::check`] found: places that hold what they held when their
/// pipeline was stored, places no pipeline was stored for yet, and those
/// (0: [`FRONT`], then the list's, then [`END`]) that hold something else.
#[derive(Default)]
struct Checked {
    same: usize,
    new: usize,
    differ: Vec<usize>,
}

/// Vectors a pipeline of the check writes ([`COMPARED`] of them and the
/// number of its code), at most.
const SLOT: usize = 16;
const COMPARED: usize = 12;

/// Samples through one sampler what tells samplers apart: the level it
/// reads at eight levels asked for (`levels` holds its level's number: the
/// bias and both bounds), the level it takes for a stretched and a round
/// footprint (`wide`, likewise: the anisotropy), and from a small noisy
/// volume a point between texels above, at and between levels (the three
/// filters) and points beyond each side (the address modes, the border).
const PLAIN: &str = r#"
cbuffer At : register(b0) { uint base; float lod; uint spare0; uint spare1; };
Texture2D<float> levels : register(t0);
Texture2D<float> wide : register(t1);
Texture3D<float4> pattern : register(t2);
RWStructuredBuffer<float4> wrote : register(u0);
SamplerState s : register(s0);
[numthreads(1, 1, 1)]
void main() {
    float2 m = float2(0.5, 0.5);
    wrote[base + 0] = float4(levels.SampleLevel(s, m, -20.0), levels.SampleLevel(s, m, -9.37),
        levels.SampleLevel(s, m, -3.61), levels.SampleLevel(s, m, 0.0));
    wrote[base + 1] = float4(levels.SampleLevel(s, m, 2.43), levels.SampleLevel(s, m, 6.79),
        levels.SampleLevel(s, m, 11.17), levels.SampleLevel(s, m, 20.0));
    wrote[base + 2] = float4(wide.SampleGrad(s, m, float2(0.25, 0.0), float2(0.0, 1.0 / 256.0)),
        wide.SampleGrad(s, m, float2(1.0 / 32.0, 0.0), float2(0.0, 1.0 / 32.0)), 0.0, 0.0);
    float3 p = float3(0.37, 0.61, 0.45);
    wrote[base + 3] = pattern.SampleLevel(s, p, lod - 1.0);
    wrote[base + 4] = pattern.SampleLevel(s, p, lod + 1.0);
    wrote[base + 5] = pattern.SampleLevel(s, p, lod + 1.5);
    wrote[base + 6] = pattern.SampleLevel(s, float3(-0.3, 0.4, 0.4), lod);
    wrote[base + 7] = pattern.SampleLevel(s, float3(-1.3, 0.4, 0.4), lod);
    wrote[base + 8] = pattern.SampleLevel(s, float3(0.4, -0.3, 0.4), lod);
    wrote[base + 9] = pattern.SampleLevel(s, float3(0.4, -1.3, 0.4), lod);
    wrote[base + 10] = pattern.SampleLevel(s, float3(0.4, 0.4, -0.3), lod);
    wrote[base + 11] = pattern.SampleLevel(s, float3(0.4, 0.4, -1.3), lod);
    wrote[base + 12] = float4(NUMBER, 0.0, 0.0, 0.0);
}
"#;

/// The same for a sampler that compares: a texel that holds 0.5 against
/// 0.5 and a point between texels against three values (the function, the
/// filter), and points beyond each side (the address modes, the border).
const COMPARING: &str = r#"
cbuffer At : register(b0) { uint base; float lod; uint spare0; uint spare1; };
Texture2D<float> flat : register(t3);
RWStructuredBuffer<float4> wrote : register(u0);
SamplerComparisonState s : register(s0);
[numthreads(1, 1, 1)]
void main() {
    float2 p = float2(0.45, 0.55);
    wrote[base + 0] = float4(flat.SampleCmpLevelZero(s, float2(0.375, 0.375), 0.5), flat.SampleCmpLevelZero(s, p, 0.2),
        flat.SampleCmpLevelZero(s, p, 0.5), flat.SampleCmpLevelZero(s, p, 0.8));
    wrote[base + 1] = float4(flat.SampleCmpLevelZero(s, float2(-0.3, 0.4), 0.5),
        flat.SampleCmpLevelZero(s, float2(-1.3, 0.4), 0.5), flat.SampleCmpLevelZero(s, float2(0.4, -0.3), 0.5),
        flat.SampleCmpLevelZero(s, float2(0.4, -1.3), 0.5));
    for (uint i = 2; i < 12; i++) wrote[base + i] = float4(0.0, 0.0, 0.0, 0.0);
    wrote[base + 12] = float4(NUMBER, 0.0, 0.0, 0.0);
}
"#;

/// A pipeline for [`Probe::run`], its root signature ([`root`]'s
/// parameters) and its four constants (the first: where it writes).
pub(super) struct Run {
    pub(super) root: ID3D12RootSignature,
    pub(super) pso: ID3D12PipelineState,
    pub(super) constants: [u32; 4],
}

/// Runs small compute pipelines over textures of known contents on a queue
/// of its own, and reads back what they wrote.
pub(super) struct Probe {
    device: ID3D12Device,
    queue: ID3D12CommandQueue,
    fence: ID3D12Fence,
    /// `levels` (`t0`: 16,384 by 1, each of its 15 levels holds its number),
    /// `wide` (`t1`: 256 by 256, likewise), `pattern` (`t2`: a volume of 8
    /// texels a side, four levels of noise) and `flat` (`t3`: 4 by 4, one
    /// level, values from 0.1 to 0.85, 0.5 at texel 1, 1).
    views: ID3D12DescriptorHeap,
    _textures: Vec<ID3D12Resource>,
}

impl Probe {
    pub(super) fn new(device: &ID3D12Device) -> windows::core::Result<Probe> {
        // SAFETY: plain D3D12 object creation with valid descriptions.
        unsafe {
            let queue: ID3D12CommandQueue =
                device.CreateCommandQueue(&D3D12_COMMAND_QUEUE_DESC {
                    Type: D3D12_COMMAND_LIST_TYPE_DIRECT,
                    ..Default::default()
                })?;
            let fence: ID3D12Fence = device.CreateFence(0, D3D12_FENCE_FLAG_NONE)?;
            let views: ID3D12DescriptorHeap =
                device.CreateDescriptorHeap(&D3D12_DESCRIPTOR_HEAP_DESC {
                    Type: D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                    NumDescriptors: TEXTURES,
                    Flags: D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
                    ..Default::default()
                })?;
            let mut probe = Probe {
                device: device.clone(),
                queue,
                fence,
                views,
                _textures: Vec::new(),
            };
            let (allocator, list) = probe.list()?;
            let mut uploads = Vec::new();
            let level = |level: u32, _: u32, _: u32, _: u32| (level as f32).to_le_bytes();
            let noise = |level: u32, x: u32, y: u32, z: u32| {
                let n =
                    (level << 9 | z << 6 | y << 3 | x).wrapping_mul(2_654_435_761) ^ 0x9E37_79B9;
                (n ^ n >> 15).wrapping_mul(2_246_822_519).to_le_bytes()
            };
            let flat = |_: u32, x: u32, y: u32, _: u32| {
                (if (x, y) == (1, 1) {
                    0.5f32
                } else {
                    0.1 + 0.05 * (x + 4 * y) as f32
                })
                .to_le_bytes()
            };
            let textures = [
                probe.texture(
                    &list,
                    &mut uploads,
                    [16384, 1, 1],
                    15,
                    DXGI_FORMAT_R32_FLOAT,
                    &level,
                )?,
                probe.texture(
                    &list,
                    &mut uploads,
                    [256, 256, 1],
                    9,
                    DXGI_FORMAT_R32_FLOAT,
                    &level,
                )?,
                probe.texture(
                    &list,
                    &mut uploads,
                    [8, 8, 8],
                    4,
                    DXGI_FORMAT_R8G8B8A8_UNORM,
                    &noise,
                )?,
                probe.texture(
                    &list,
                    &mut uploads,
                    [4, 4, 1],
                    1,
                    DXGI_FORMAT_R32_FLOAT,
                    &flat,
                )?,
            ];
            let step = device
                .GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)
                as usize;
            let first = probe.views.GetCPUDescriptorHandleForHeapStart().ptr;
            for (n, texture) in textures.iter().enumerate() {
                device.CreateShaderResourceView(
                    texture,
                    None,
                    D3D12_CPU_DESCRIPTOR_HANDLE {
                        ptr: first + n * step,
                    },
                );
            }
            probe._textures.extend(textures);
            probe.execute(&list)?;
            drop((uploads, allocator));
            Ok(probe)
        }
    }

    fn list(&self) -> windows::core::Result<(ID3D12CommandAllocator, ID3D12GraphicsCommandList)> {
        // SAFETY: plain creations; the list is open on its own allocator.
        unsafe {
            let allocator: ID3D12CommandAllocator = self
                .device
                .CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT)?;
            let list = self.device.CreateCommandList(
                0,
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                &allocator,
                None,
            )?;
            Ok((allocator, list))
        }
    }

    /// Closes `list`, runs it and waits until the card is through with it.
    fn execute(&self, list: &ID3D12GraphicsCommandList) -> windows::core::Result<()> {
        // SAFETY: an open list of ours; the event is ours until closed.
        unsafe {
            list.Close()?;
            self.queue.ExecuteCommandLists(&[Some(list.cast()?)]);
            let value = self.fence.GetCompletedValue().wrapping_add(1);
            self.queue.Signal(&self.fence, value)?;
            let event = CreateEventW(None, false, false, None)?;
            let waited = self
                .fence
                .SetEventOnCompletion(value, event)
                .map(|()| WaitForSingleObject(event, 30_000));
            let _ = windows::Win32::Foundation::CloseHandle(event);
            // A removed device's fence is at the end already.
            if waited? != windows::Win32::Foundation::WAIT_OBJECT_0
                || self.fence.GetCompletedValue() != value
            {
                return Err(self
                    .device
                    .GetDeviceRemovedReason()
                    .err()
                    .unwrap_or_else(windows::core::Error::empty));
            }
            Ok(())
        }
    }

    fn buffer(
        &self,
        heap: D3D12_HEAP_TYPE,
        bytes: u64,
        flags: D3D12_RESOURCE_FLAGS,
        state: D3D12_RESOURCE_STATES,
    ) -> windows::core::Result<ID3D12Resource> {
        let mut buffer: Option<ID3D12Resource> = None;
        // SAFETY: a valid buffer description.
        unsafe {
            self.device.CreateCommittedResource(
                &D3D12_HEAP_PROPERTIES {
                    Type: heap,
                    ..Default::default()
                },
                D3D12_HEAP_FLAG_NONE,
                &D3D12_RESOURCE_DESC {
                    Dimension: D3D12_RESOURCE_DIMENSION_BUFFER,
                    Width: bytes,
                    Height: 1,
                    DepthOrArraySize: 1,
                    MipLevels: 1,
                    SampleDesc: DXGI_SAMPLE_DESC {
                        Count: 1,
                        Quality: 0,
                    },
                    Layout: D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
                    Flags: flags,
                    ..Default::default()
                },
                state,
                None,
                &mut buffer,
            )?;
        }
        buffer.ok_or_else(windows::core::Error::empty)
    }

    /// A texture of `size` (a volume if deeper than 1) with `levels` levels
    /// of four-byte texels, filled by `texel` (level, x, y, z) through
    /// `list`; shaders of every stage may read it afterwards.
    fn texture(
        &self,
        list: &ID3D12GraphicsCommandList,
        uploads: &mut Vec<ID3D12Resource>,
        size: [u32; 3],
        levels: u16,
        format: DXGI_FORMAT,
        texel: &dyn Fn(u32, u32, u32, u32) -> [u8; 4],
    ) -> windows::core::Result<ID3D12Resource> {
        let desc = D3D12_RESOURCE_DESC {
            Dimension: if size[2] > 1 {
                D3D12_RESOURCE_DIMENSION_TEXTURE3D
            } else {
                D3D12_RESOURCE_DIMENSION_TEXTURE2D
            },
            Width: u64::from(size[0]),
            Height: size[1],
            DepthOrArraySize: size[2] as u16,
            MipLevels: levels,
            Format: format,
            SampleDesc: DXGI_SAMPLE_DESC {
                Count: 1,
                Quality: 0,
            },
            ..Default::default()
        };
        let mut texture: Option<ID3D12Resource> = None;
        let mut prints = vec![D3D12_PLACED_SUBRESOURCE_FOOTPRINT::default(); usize::from(levels)];
        let mut bytes = 0u64;
        // SAFETY: valid descriptions; the upload buffer is written within
        // the bytes the device asked for (each level's rows at its pitch),
        // and lives until the list has run (`uploads`).
        unsafe {
            self.device.CreateCommittedResource(
                &D3D12_HEAP_PROPERTIES {
                    Type: D3D12_HEAP_TYPE_DEFAULT,
                    ..Default::default()
                },
                D3D12_HEAP_FLAG_NONE,
                &desc,
                D3D12_RESOURCE_STATE_COPY_DEST,
                None,
                &mut texture,
            )?;
            let texture = texture.ok_or_else(windows::core::Error::empty)?;
            self.device.GetCopyableFootprints(
                &desc,
                0,
                u32::from(levels),
                0,
                Some(prints.as_mut_ptr()),
                None,
                None,
                Some(&mut bytes),
            );
            let upload = self.buffer(
                D3D12_HEAP_TYPE_UPLOAD,
                bytes,
                D3D12_RESOURCE_FLAG_NONE,
                D3D12_RESOURCE_STATE_GENERIC_READ,
            )?;
            let mut data = std::ptr::null_mut();
            upload.Map(0, None, Some(&mut data))?;
            let data = std::slice::from_raw_parts_mut(data.cast::<u8>(), bytes as usize);
            for (level, print) in prints.iter().enumerate() {
                let (f, pitch) = (print.Footprint, print.Footprint.RowPitch as usize);
                for z in 0..f.Depth {
                    for y in 0..f.Height {
                        let row = print.Offset as usize + (z * f.Height + y) as usize * pitch;
                        for x in 0..f.Width {
                            let at = row + x as usize * 4;
                            data[at..at + 4].copy_from_slice(&texel(level as u32, x, y, z));
                        }
                    }
                }
                let to = D3D12_TEXTURE_COPY_LOCATION {
                    pResource: std::mem::transmute_copy(&texture),
                    Type: D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
                    Anonymous: D3D12_TEXTURE_COPY_LOCATION_0 {
                        SubresourceIndex: level as u32,
                    },
                };
                let from = D3D12_TEXTURE_COPY_LOCATION {
                    pResource: std::mem::transmute_copy(&upload),
                    Type: D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
                    Anonymous: D3D12_TEXTURE_COPY_LOCATION_0 {
                        PlacedFootprint: *print,
                    },
                };
                list.CopyTextureRegion(&to, 0, 0, 0, &from, None);
            }
            upload.Unmap(0, None);
            list.ResourceBarrier(&[crate::host::transition(
                &texture,
                D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                    | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            )]);
            uploads.push(upload);
            Ok(texture)
        }
    }

    /// Runs each of `runs` once and gives what they wrote: `slots` times
    /// [`SLOT`] vectors, as bits.
    pub(super) fn run(&self, runs: &[Run], slots: usize) -> windows::core::Result<Vec<[u32; 4]>> {
        let bytes = (slots.max(1) * SLOT * 16) as u64;
        let wrote = self.buffer(
            D3D12_HEAP_TYPE_DEFAULT,
            bytes,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        )?;
        let back = self.buffer(
            D3D12_HEAP_TYPE_READBACK,
            bytes,
            D3D12_RESOURCE_FLAG_NONE,
            D3D12_RESOURCE_STATE_COPY_DEST,
        )?;
        let (_allocator, list) = self.list()?;
        // SAFETY: an open list; every object it names lives until it has
        // run; the read-back buffer holds `bytes` bytes.
        unsafe {
            list.SetDescriptorHeaps(&[Some(self.views.clone())]);
            for run in runs {
                list.SetComputeRootSignature(&run.root);
                list.SetComputeRoot32BitConstants(0, 4, run.constants.as_ptr().cast(), 0);
                list.SetComputeRootDescriptorTable(
                    1,
                    self.views.GetGPUDescriptorHandleForHeapStart(),
                );
                list.SetComputeRootUnorderedAccessView(2, wrote.GetGPUVirtualAddress());
                list.SetPipelineState(&run.pso);
                list.Dispatch(1, 1, 1);
            }
            list.ResourceBarrier(&[crate::host::transition(
                &wrote,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_COPY_SOURCE,
            )]);
            list.CopyResource(&back, &wrote);
            self.execute(&list)?;
            let mut data = std::ptr::null_mut();
            back.Map(0, None, Some(&mut data))?;
            let read = std::slice::from_raw_parts(data.cast::<[u32; 4]>(), slots * SLOT).to_vec();
            back.Unmap(0, None);
            Ok(read)
        }
    }
}

/// `LEONIDA_LIBRARY_STATIC_SAMPLERS=0`: pipelines with static samplers are
/// made by the driver every run on NVIDIA, as before (and no list is made).
fn off() -> bool {
    static OFF: OnceLock<bool> = OnceLock::new();
    *OFF.get_or_init(|| {
        std::env::var_os("LEONIDA_LIBRARY_STATIC_SAMPLERS").is_some_and(|v| v == "0")
    })
}

/// Whether `device` is an NVIDIA card's.
fn nvidia(device: &ID3D12Device) -> bool {
    use windows::Win32::Graphics::Dxgi::{
        CreateDXGIFactory2, DXGI_CREATE_FACTORY_FLAGS, IDXGIAdapter1, IDXGIFactory4,
    };
    // SAFETY: plain queries.
    unsafe {
        CreateDXGIFactory2::<IDXGIFactory4>(DXGI_CREATE_FACTORY_FLAGS(0))
            .and_then(|f| f.EnumAdapterByLuid::<IDXGIAdapter1>(device.GetAdapterLuid()))
            .and_then(|a| a.GetDesc1())
            .is_ok_and(|d| d.VendorId == 0x10DE)
    }
}

/// The host device's places, if it has them.
static PLACES: OnceLock<Option<Places>> = OnceLock::new();

/// The host device was made: its listed static samplers are made now,
/// before anything else is made with it ([`Places::start`]). Nothing on
/// another vendor's card, without a cache folder, or with
/// `LEONIDA_LIBRARY_STATIC_SAMPLERS` set to 0 or 1.
pub(crate) fn start(device: &ID3D12Device) {
    PLACES.get_or_init(|| {
        if off()
            || std::env::var_os("LEONIDA_LIBRARY_STATIC_SAMPLERS").is_some_and(|v| v == "1")
            || !nvidia(device)
        {
            return None;
        }
        Places::start(device, &super::library::folder()?)
    });
}

/// How a host pipeline with static samplers uses the pipeline library.
pub(super) enum Use {
    /// Under its plain name: there is nothing to care for.
    Plain,
    /// Not at all: the driver makes it every run.
    Skip,
    /// Under its name and what names the places of its samplers.
    Placed(String),
}

impl Use {
    /// The library name of the pipeline plainly named `name`, if it uses
    /// the library.
    pub(super) fn name(&self, name: &str) -> Option<String> {
        match self {
            Use::Plain => Some(name.to_owned()),
            Use::Skip => None,
            Use::Placed(key) => Some(super::library::name(
                "static samplers placed",
                key.as_bytes(),
                &[name.as_bytes()],
                "",
            )),
        }
    }
}

/// How the host pipeline with the static `samplers` uses the library.
pub(super) fn library_use(host: &crate::host::Host, samplers: &[D3D12_STATIC_SAMPLER_DESC]) -> Use {
    if samplers.is_empty() || super::library::allows_static_samplers(host) {
        return Use::Plain;
    }
    let Some(places) = PLACES.get().and_then(Option::as_ref).filter(|_| !off()) else {
        return Use::Skip;
    };
    // Asked first: a sampler new to the list is written down even in a run
    // whose places differ.
    let Some(key) = places.key(samplers) else {
        return Use::Skip;
    };
    match super::library::host_library() {
        Some(library) if places.same(&host.device, library) => Use::Placed(key),
        _ => Use::Skip,
    }
}

#[cfg(test)]
mod tests;
