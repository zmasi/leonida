//! Translated shaders kept on disk (decision 0003): under the configured
//! cache folder, keyed by the container's content and by the translator
//! (the BLAKE3 of its sources) and its settings, so a changed translator
//! never serves an old translation. Entries go through
//! `leonida-pipeline`'s content-addressed cache, which writes them
//! atomically; an entry that does not read back is translated again.
//!
//! The entry is our own encoding of [`HostShader`]: a field added to it
//! must be added to `encode` and `decode` too (entries of older builds
//! are ignored anyway).

use std::path::PathBuf;
use std::sync::OnceLock;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};

use leonida_pipeline::{Cache, Converter};
use leonida_shader::emit::{FormsFor, HostShader, StaticSampler, UserData};
use leonida_shader::resource::RootIndexedView;
use leonida_shader::{HwStage, RootType};

static CACHE: OnceLock<Cache> = OnceLock::new();
static HITS: AtomicU64 = AtomicU64::new(0);
static MISSES: AtomicU64 = AtomicU64::new(0);

/// Keeps translated shaders in `dir` from now on.
pub fn use_shader_cache(dir: PathBuf) {
    match Cache::open(dir) {
        Ok(cache) => {
            let _ = CACHE.set(cache);
        }
        Err(e) => tracing::warn!(error = %e, "no shader cache"),
    }
}

/// Shaders found in the cache and translated since the start.
pub(crate) fn counts() -> (u64, u64) {
    (HITS.load(Ordering::Relaxed), MISSES.load(Ordering::Relaxed))
}

/// What identifies a translation beside its container: the translator
/// (the BLAKE3 of its sources, `build.rs`), the settings it runs with, and
/// what it was told of the host. Not our executable: a rebuild of anything
/// else used to translate every shader again.
fn build() -> [u8; 32] {
    // The view heap's null slot differs by host GPU: a translation cached
    // without the clamp must not be served (`host_settings` below).
    crate::descriptor::host::probe_view_limit();
    static FIXED: OnceLock<Vec<u8>> = OnceLock::new();
    let fixed = FIXED.get_or_init(|| {
        let limit = leonida_shader::emit::loop_limit();
        if limit != leonida_shader::emit::DEFAULT_LOOP_LIMIT {
            tracing::warn!(
                limit,
                "LEONIDA_SHADER_LOOP_LIMIT: translated loops stop sooner"
            );
        }
        let mut bytes = env!("LEONIDA_TRANSLATOR").as_bytes().to_vec();
        bytes.extend_from_slice(&limit.to_le_bytes());
        // Which group barriers wait is part of the translation too.
        bytes.push(leonida_shader::emit::group_sync());
        // So are the switches that pick the form of a wave64 program:
        // without them here a run with `LEONIDA_WAVE_WIDE=0` was
        // served the translations an earlier run of the same
        // executable had cached with the switch unset.
        for name in [
            "LEONIDA_BVH_MISS",
            // LOCAL: colours and image stores with NaN made 0.
            "LEONIDA_NAN_SCRUB",
            // LOCAL (fxceless #1216): constant buffer slots marked.
            "LEONIDA_CONSTANT_MARKS",
            "LEONIDA_WAVE_WIDE",
            // Leonida: the loose-LDS last resort (emit/lower.rs with_lds_loose).
            "LEONIDA_WAVE_LDS_LOOSE",
            "LEONIDA_WAVE_DEEP_SKIPS",
            "LEONIDA_WAVE_OWN_BITS",
            "LEONIDA_WAVE_LOCAL",
            "LEONIDA_F16_NEAREST",
            "LEONIDA_HELPER_OWN",
            "LEONIDA_FIRSTLANE_OWN",
            "LEONIDA_FIRST_HALF",
            "LEONIDA_STRICT_JOINS",
            "LEONIDA_MESH_OUTPUT_STRICT",
            "LEONIDA_BVH_FENCES",
            "LEONIDA_BVH_BOUNDS",
            "LEONIDA_THREAD_TICKET",
            "LEONIDA_LDS_FLAGS",
            "LEONIDA_EXCHANGE_SITES",
            "LEONIDA_UNIFORM_LOADS",
            // Leonida: group packing changes the program's group size and words (emit/lower.rs group_pack);
            // without it here a run with it set was served the unpacked translations of an earlier run.
            "LEONIDA_GROUP_PACK",
        ] {
            bytes.extend_from_slice(name.as_bytes());
            bytes.push(b'=');
            if let Some(value) = std::env::var_os(name) {
                bytes.extend_from_slice(value.as_encoded_bytes());
            }
            bytes.push(0);
        }
        // And which clean-up passes ran over the translated code.
        bytes.extend_from_slice(&leonida_shader::emit::shader_opt().to_le_bytes());
        // Leonida: and every `LEONIDA_*` switch the translator itself reads (its build script lists them), so a
        // switch added there without a line above cannot be served a translation made with another value.
        bytes.extend_from_slice(&leonida_shader::emit::translation_fingerprint());
        bytes
    });
    let mut hash = blake3::Hasher::new();
    hash.update(fixed);
    for value in leonida_shader::emit::host_settings() {
        hash.update(&value.to_le_bytes());
    }
    *hash.finalize().as_bytes()
}

/// What tells one set of translations from another ([`build`]).
pub(crate) fn translation() -> [u8; 32] {
    build()
}

/// Puts the translation of `container` into the cache before any title asks
/// for it (`leonida shaders`): whether it was there already, or why it does
/// not translate. The host is opened first, as a run does: what the card
/// can do decides the form of some programs.
pub fn precompile(container: &[u8]) -> Result<bool, String> {
    let _ = crate::host::host();
    let misses = MISSES.load(Ordering::Relaxed);
    host_shader(container)?;
    // (Counts other threads' misses too: the caller only adds them up.)
    Ok(MISSES.load(Ordering::Relaxed) == misses)
}

/// The translation of a console shader container, from the cache when
/// there is one.
pub(crate) fn host_shader(container: &[u8]) -> Result<HostShader, String> {
    let forms = forms_for(container);
    let translate = |c: &[u8]| {
        leonida_shader::emit::with_forms_for(forms, || leonida_shader::emit::translate(c)).map_err(|e| e.to_string())
    };
    let shader = match CACHE.get() {
        Some(cache) => cached(cache, &key_for(build(), forms), container, translate),
        None => translate(container),
    }?;
    #[cfg(feature = "diagnostics")]
    let shader = paired_override(container, shader);
    Ok(shader)
}

/// The programs a `LEONIDA_PAIRED_FOR` or `LEONIDA_WAVE_WIDE_FOR` list names: hashes as the log
/// and the frame listings name shaders (the first 16 hex digits of the container's BLAKE3), or
/// their beginnings from 8 digits on, separated by commas. What is not that is left out and
/// returned second, to be said in the log. Pure, for testing.
fn parse_names(value: Option<&str>) -> (Vec<String>, Vec<String>) {
    value
        .unwrap_or_default()
        .split(',')
        .map(|h| h.trim().to_ascii_lowercase())
        .filter(|h| !h.is_empty())
        .partition(|h| (8..=64).contains(&h.len()) && h.bytes().all(|b| b.is_ascii_hexdigit()))
}

/// The forms asked for `container` by name (`leonida_shader::emit::with_forms_for`), read once:
/// - `LEONIDA_PAIRED_FOR=<hash>,<hash>`: the paired form before every other for these programs,
///   the forms the chain counts as exact included (as `LEONIDA_PAIRED_LANES=first`, for them
///   alone);
/// - `LEONIDA_WAVE_WIDE_FOR=<hash>,<hash>`: the wave-wide form before the approximate ones for
///   these (as `LEONIDA_WAVE_WIDE=1`, for them alone, and not the paired form in its place).
///
/// Both unset (the default): nothing changes. Only console wave64 compute programs that a host
/// of 32-lane waves does not take as they are have such forms; naming another changes nothing
/// but its place in the cache. The named programs are translated again and kept apart
/// ([`key_for`]); every other program keeps its translation, so trying a form on one program
/// does not cost a translation of them all, as `LEONIDA_PAIRED_LANES` and `LEONIDA_WAVE_WIDE`
/// do.
///
/// After edsuuu/VI-BR-PORT 7318dee, 1fa0ed1 and cf3394b (edsuuu; `VICTORY_WAVE_WIDE_FOR`,
/// `VICTORY_WAVE_PAIRED_FOR`), GPL-3.0. There the lists are part of every program's key.
fn forms_for(container: &[u8]) -> FormsFor {
    static NAMED: OnceLock<[Vec<String>; 2]> = OnceLock::new();
    let [paired, wide] = NAMED.get_or_init(|| {
        ["LEONIDA_PAIRED_FOR", "LEONIDA_WAVE_WIDE_FOR"].map(|switch| {
            let (named, refused) = parse_names(std::env::var(switch).ok().as_deref());
            if !refused.is_empty() {
                tracing::warn!(switch, ?refused, "not shader hashes (8 to 64 hex digits each): left out");
            }
            if !named.is_empty() {
                tracing::info!(switch, programs = ?named, "these programs are translated in a form asked for by name");
            }
            named
        })
    });
    if paired.is_empty() && wide.is_empty() {
        return FormsFor::default();
    }
    let hash = blake3::hash(container).to_hex();
    let is = |names: &[String]| names.iter().any(|h| hash.starts_with(h.as_str()));
    FormsFor { paired_first: is(paired), wide: is(wide) }
}

/// The cache's key for a translation with `forms` asked for its program: `build` itself when
/// none is (every entry made before stays good), else one of its own for each choice.
fn key_for(build: [u8; 32], forms: FormsFor) -> [u8; 32] {
    if forms == FormsFor::default() {
        return build;
    }
    let mut hash = blake3::Hasher::new();
    hash.update(&build);
    hash.update(b"forms asked for this program");
    hash.update(&[u8::from(forms.paired_first), u8::from(forms.wide)]);
    *hash.finalize().as_bytes()
}

/// Try cache entries made by the paired-wave translator for the 32-lane,
/// one-million-descriptor host. The normal translation remains our source
/// of binding metadata and remains in the translation cache.
#[cfg(feature = "diagnostics")]
fn paired_override(container: &[u8], mut shader: HostShader) -> HostShader {
    static DIR: OnceLock<Option<PathBuf>> = OnceLock::new();
    let Some(dir) = DIR.get_or_init(|| std::env::var_os("LEONIDA_PAIRED_SHADER_DIR").map(PathBuf::from)) else {
        return shader;
    };
    if shader.stage != HwStage::Cs || leonida_shader::emit::host_settings() != [32, 999_999, 0, 0] {
        return shader;
    }
    let digest = blake3::hash(container).to_hex();
    let hash = &digest[..16];
    let path = dir.join(format!("{hash}.vsh"));
    let mut bytes = match std::fs::read(&path) {
        Ok(bytes) => bytes,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return shader,
        Err(e) => {
            tracing::warn!(hash, error = %e, "could not read paired shader");
            return shader;
        }
    };
    // Paired VSH1 entries predate the half-view list. These programs have
    // none. The equality check includes the list on our side.
    bytes.extend_from_slice(&0u32.to_le_bytes());
    if let Some(mut paired) = decode(&bytes) {
        let dxil = std::mem::replace(&mut paired.dxil, shader.dxil.clone());
        if paired == shader && dxil.starts_with(b"DXBC") {
            shader.dxil = dxil;
            tracing::info!(hash, "using paired Wave32 bytecode with matching shader bindings");
            return shader;
        }
    }
    tracing::warn!(hash, "paired shader entry did not match the translated shader bindings");
    shader
}

/// The cache entry's converter: the build and the container in, the
/// encoded translation out. `ran` tells a miss from a hit.
struct Translate<F> {
    translate: F,
    ran: AtomicBool,
}

impl<F: Fn(&[u8]) -> Result<HostShader, String> + Send + Sync> Converter for Translate<F> {
    fn id(&self) -> &'static str {
        "shader.dxil"
    }
    fn version(&self) -> u32 {
        // The build fingerprint in the input does the versioning.
        1
    }
    fn convert(&self, input: &[u8]) -> anyhow::Result<Vec<u8>> {
        self.ran.store(true, Ordering::Relaxed);
        let shader = (self.translate)(&input[32..]).map_err(anyhow::Error::msg)?;
        Ok(encode(&shader))
    }
}

fn cached(
    cache: &Cache,
    build: &[u8; 32],
    container: &[u8],
    translate: impl Fn(&[u8]) -> Result<HostShader, String> + Send + Sync,
) -> Result<HostShader, String> {
    let input = [&build[..], container].concat();
    let converter = Translate {
        translate,
        ran: AtomicBool::new(false),
    };
    for _ in 0..2 {
        let path = cache
            .get_or_convert(&converter, &input)
            .map_err(|e| format!("{e:#}"))?;
        if let Some(shader) = std::fs::read(&path).ok().and_then(|b| decode(&b)) {
            let (counter, what) = if converter.ran.load(Ordering::Relaxed) {
                (&MISSES, "translated and cached")
            } else {
                (&HITS, "from the cache")
            };
            counter.fetch_add(1, Ordering::Relaxed);
            tracing::debug!(path = %path.display(), "shader {what}");
            return Ok(shader);
        }
        tracing::debug!(path = %path.display(), "unreadable shader cache entry; translating again");
        let _ = std::fs::remove_file(&path);
    }
    MISSES.fetch_add(1, Ordering::Relaxed);
    (converter.translate)(container)
}

const MAGIC: &[u8; 4] = b"VSH1";
const STAGES: [HwStage; 8] = [
    HwStage::Ls,
    HwStage::Hs,
    HwStage::Es,
    HwStage::Gs,
    HwStage::Vs,
    HwStage::Ps,
    HwStage::Ms,
    HwStage::Cs,
];

fn root_code(r: RootType) -> u32 {
    match r {
        RootType::Constants => 0,
        RootType::Descriptor => 1,
        RootType::Table => 2,
        RootType::ImmediateSampler => 3,
        RootType::ResourceHeap => 4,
        RootType::SamplerHeap => 5,
        RootType::Other(x) => 6 + x,
    }
}

fn root_type(code: u32) -> RootType {
    match code {
        0 => RootType::Constants,
        1 => RootType::Descriptor,
        2 => RootType::Table,
        3 => RootType::ImmediateSampler,
        4 => RootType::ResourceHeap,
        5 => RootType::SamplerHeap,
        x => RootType::Other(x - 6),
    }
}

/// Little-endian words and length-prefixed lists.
fn encode(s: &HostShader) -> Vec<u8> {
    let mut out = MAGIC.to_vec();
    let word = |out: &mut Vec<u8>, v: u32| out.extend(v.to_le_bytes());
    let stage = STAGES.iter().position(|&x| x == s.stage).unwrap_or(0) as u32;
    word(&mut out, stage);
    word(&mut out, s.dxil.len() as u32);
    out.extend(&s.dxil);
    word(&mut out, s.user_data.len() as u32);
    for u in &s.user_data {
        for v in [u.index, u32::from(u.slot), root_code(u.root), u.dword] {
            word(&mut out, v);
        }
    }
    word(&mut out, s.samplers.len() as u32);
    for sampler in &s.samplers {
        for &v in sampler {
            word(&mut out, v);
        }
    }
    word(&mut out, s.static_samplers.len() as u32);
    for t in &s.static_samplers {
        let [u, v, w] = t.address;
        let words = [
            t.filter,
            u,
            v,
            w,
            t.mip_lod_bias.to_bits(),
            t.max_anisotropy,
            t.comparison,
            t.border,
            t.min_lod.to_bits(),
            t.max_lod.to_bits(),
        ];
        for v in words {
            word(&mut out, v);
        }
    }
    for slots in [&s.root_cbvs, &s.root_srvs, &s.root_uavs] {
        word(&mut out, slots.len() as u32);
        out.extend(slots);
    }
    word(&mut out, s.root_indexed_views.len() as u32);
    for view in &s.root_indexed_views {
        for value in [
            u32::from(view.table_slot),
            u32::from(view.constant_slot),
            view.constant_dword,
            view.byte_offset,
            u32::from(view.shift),
        ] {
            word(&mut out, value);
        }
    }
    word(&mut out, s.half_views.len() as u32);
    for &(slot, dword) in &s.half_views {
        word(&mut out, u32::from(slot));
        word(&mut out, dword);
    }
    out
}

fn decode(bytes: &[u8]) -> Option<HostShader> {
    struct Reader<'a>(&'a [u8]);
    impl<'a> Reader<'a> {
        fn take(&mut self, n: usize) -> Option<&'a [u8]> {
            let (head, rest) = self.0.split_at_checked(n)?;
            self.0 = rest;
            Some(head)
        }
        fn word(&mut self) -> Option<u32> {
            Some(u32::from_le_bytes(self.take(4)?.try_into().ok()?))
        }
        fn list(&mut self) -> Option<Vec<u8>> {
            let n = self.word()? as usize;
            Some(self.take(n)?.to_vec())
        }
    }
    let mut r = Reader(bytes);
    if r.take(4)? != MAGIC {
        return None;
    }
    let stage = *STAGES.get(r.word()? as usize)?;
    let dxil = r.list()?;
    let n = r.word()? as usize;
    let mut user_data = Vec::with_capacity(n.min(256));
    for _ in 0..n {
        user_data.push(UserData {
            index: r.word()?,
            slot: u8::try_from(r.word()?).ok()?,
            root: root_type(r.word()?),
            dword: r.word()?,
        });
    }
    let n = r.word()? as usize;
    let mut samplers = Vec::with_capacity(n.min(256));
    for _ in 0..n {
        samplers.push([r.word()?, r.word()?, r.word()?, r.word()?]);
    }
    let n = r.word()? as usize;
    let mut static_samplers = Vec::with_capacity(n.min(256));
    for _ in 0..n {
        static_samplers.push(StaticSampler {
            filter: r.word()?,
            address: [r.word()?, r.word()?, r.word()?],
            mip_lod_bias: f32::from_bits(r.word()?),
            max_anisotropy: r.word()?,
            comparison: r.word()?,
            border: r.word()?,
            min_lod: f32::from_bits(r.word()?),
            max_lod: f32::from_bits(r.word()?),
        });
    }
    let root_cbvs = r.list()?;
    let root_srvs = r.list()?;
    let root_uavs = r.list()?;
    let n = r.word()? as usize;
    let mut root_indexed_views = Vec::with_capacity(n.min(256));
    for _ in 0..n {
        root_indexed_views.push(RootIndexedView {
            table_slot: u8::try_from(r.word()?).ok()?,
            constant_slot: u8::try_from(r.word()?).ok()?,
            constant_dword: r.word()?,
            byte_offset: r.word()?,
            shift: u8::try_from(r.word()?).ok().filter(|&s| s < 32)?,
        });
    }
    let n = r.word()? as usize;
    let mut half_views = Vec::with_capacity(n.min(256));
    for _ in 0..n {
        half_views.push((u8::try_from(r.word()?).ok()?, r.word()?));
    }
    r.0.is_empty().then_some(HostShader {
        stage,
        dxil,
        user_data,
        samplers,
        static_samplers,
        root_cbvs,
        root_srvs,
        root_uavs,
        root_indexed_views,
        half_views,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn programs_are_named_by_hash_or_its_beginning() {
        let (named, refused) = parse_names(Some(" 1AA247ED3653B616 ,d3e5b737,, 55d2 , tiles,6f225b641c5b0708"));
        assert_eq!(named, ["1aa247ed3653b616", "d3e5b737", "6f225b641c5b0708"]);
        // (too short to name one program; not a hash)
        assert_eq!(refused, ["55d2", "tiles"]);
        assert_eq!(parse_names(None), (vec![], vec![]));
        assert_eq!(parse_names(Some("")), (vec![], vec![]));
    }

    #[test]
    fn only_a_program_with_a_form_asked_for_gets_a_key_of_its_own() {
        let build = [7u8; 32];
        let (paired, wide) = (
            FormsFor { paired_first: true, wide: false },
            FormsFor { paired_first: false, wide: true },
        );
        assert_eq!(key_for(build, FormsFor::default()), build);
        let keys = [key_for(build, paired), key_for(build, wide), key_for(build, FormsFor { paired_first: true, wide: true })];
        for (i, k) in keys.iter().enumerate() {
            assert_ne!(*k, build);
            assert!(keys[..i].iter().all(|other| other != k));
        }
        // (and still by the translation: another build, another key)
        assert_ne!(key_for([8u8; 32], paired), keys[0]);
    }

    fn shader() -> HostShader {
        HostShader {
            stage: HwStage::Ps,
            dxil: b"DXBC and the rest".to_vec(),
            user_data: vec![UserData {
                index: 8,
                slot: 1,
                root: RootType::Table,
                dword: 0,
            }],
            samplers: vec![[0x400, 0xc0_0000, 0x18f0_0000, 0]],
            static_samplers: vec![StaticSampler::from_words([
                0x400,
                0xc0_0000,
                0x18f0_0000,
                0,
            ])],
            root_cbvs: vec![2],
            root_srvs: vec![0, 3],
            root_uavs: vec![1],
            root_indexed_views: vec![RootIndexedView {
                table_slot: 1,
                constant_slot: 2,
                constant_dword: 3,
                byte_offset: 32,
                shift: 5,
            }],
            half_views: vec![(0, 8), (4, 0)],
        }
    }

    #[test]
    fn translations_come_back_from_the_cache() {
        let dir = tempfile::tempdir().unwrap();
        let cache = Cache::open(dir.path()).unwrap();
        let build = [7u8; 32];
        let container = b"a console container".as_slice();
        let runs = AtomicU64::new(0);
        let translate = |_: &[u8]| {
            runs.fetch_add(1, Ordering::Relaxed);
            Ok(shader())
        };
        assert_eq!(cached(&cache, &build, container, translate), Ok(shader()));
        assert_eq!(cached(&cache, &build, container, translate), Ok(shader()));
        assert_eq!(runs.load(Ordering::Relaxed), 1, "the second came from disk");

        // Another build translates again.
        assert_eq!(cached(&cache, &[8; 32], container, translate), Ok(shader()));
        assert_eq!(runs.load(Ordering::Relaxed), 2);

        // A damaged entry is a miss, translated and stored again.
        for e in walk(dir.path()) {
            std::fs::write(e, b"VSH1 broken").unwrap();
        }
        assert_eq!(cached(&cache, &build, container, translate), Ok(shader()));
        assert_eq!(runs.load(Ordering::Relaxed), 3);
        assert_eq!(cached(&cache, &build, container, translate), Ok(shader()));
        assert_eq!(runs.load(Ordering::Relaxed), 3);

        // Failures are not stored.
        let fail = |_: &[u8]| Err::<HostShader, _>("unsupported".to_owned());
        assert!(cached(&cache, &build, b"other", fail).is_err());
    }

    fn walk(dir: &std::path::Path) -> Vec<PathBuf> {
        let mut files = Vec::new();
        for e in std::fs::read_dir(dir).unwrap().flatten() {
            if e.path().is_dir() {
                files.extend(walk(&e.path()));
            } else {
                files.push(e.path());
            }
        }
        files
    }

    #[test]
    fn entries_round_trip_and_reject_damage() {
        let bytes = encode(&shader());
        assert_eq!(decode(&bytes), Some(shader()));
        assert_eq!(decode(&bytes[..bytes.len() - 1]), None);
        assert_eq!(decode(&[bytes.as_slice(), &[0]].concat()), None);
        assert_eq!(decode(b"VSH0"), None);
    }
}
