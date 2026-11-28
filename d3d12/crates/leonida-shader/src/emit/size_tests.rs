//! End-to-end tests of what a program tells about a view by how it uses it:
//! descriptors read for sizes (the dwords of a view no instruction uses) and
//! counters, lowered to DXIL, validated by `dxil.dll` when present, and run
//! on the host GPU and WARP.

use super::host::{Stride, View};
use super::waterfall_tests::run;
use super::{HostShader, lower, validator};
use crate::native::{
    ComputeIo, HwStage, NativeProgram, ResourceType, RootParam, RootType, StageIo, UsedResource,
};
use crate::{ShaderError, io, ir, resource, structure};

/// Lowers a wave32 compute program of one thread with the table of views at
/// s0 and the view heap at s1; `srv`: the dwords and the stride of the used
/// resource at the table's first entry (the third is a raw UAV).
fn build(words: &[u32], srv: (u8, u32)) -> Result<HostShader, ShaderError> {
    let param = |root, user_data| RootParam {
        root,
        table_offset_dwords: 0,
        sgprs: 1,
        base_register: 0,
        user_data,
        slot: 0,
    };
    let range = |kind, table_offset_dwords, dwords, stride| UsedResource {
        kind,
        root: RootType::Table,
        table_offset_dwords,
        lower_bound: 0,
        stride,
        dwords,
        user_data: 0,
        slot: 0,
    };
    let native = NativeProgram {
        stage: HwStage::Cs,
        wave64: false,
        vgprs: 32,
        layout: vec![param(RootType::Table, 0), param(RootType::ResourceHeap, 1)],
        used: vec![
            range(ResourceType::Srv, 0, srv.0, srv.1),
            range(ResourceType::Uav, 16, 4, 0),
        ],
        io: StageIo::Cs(ComputeIo {
            rsrc2: 2 << 1,
            threads: [1, 1, 1],
        }),
        code: words.iter().flat_map(|w| w.to_le_bytes()).collect(),
    };
    let p = ir::build(&native.code, native.wave64, native.vgprs)?;
    let st = structure::structure(&p)?;
    let res = resource::track(&p, &native.layout, native.stage, Some(&st))?;
    let io = io::raise(&p, &native)?;
    let lowered = lower::lower(&lower::Parts {
        native: &native,
        p: &p,
        st: &st,
        io: &io,
        res: &res,
        inputs: Vec::new(),
        outputs: Vec::new(),
        prims: Vec::new(),
        mesh_stride: 0,
    })?;
    let shader = HostShader {
        stage: HwStage::Cs,
        dxil: lowered.dxil,
        user_data: lowered.user_data,
        samplers: lowered.samplers,
        static_samplers: Vec::new(),
        root_cbvs: lowered.root_cbvs,
        root_srvs: lowered.root_srvs,
        root_uavs: lowered.root_uavs,
        root_indexed_views: Vec::new(),
        half_views: Vec::new(),
    };
    if let Ok(mut v) = validator::Validator::load(validator::Validator::default_path().as_deref()) {
        v.validate(&mut shader.dxil.clone())
            .expect("DXIL validation failed");
    } else {
        eprintln!("no dxil.dll; validation skipped");
    }
    Ok(shader)
}

/// The table at s[2:3], its raw UAV's descriptor in s[20:23].
const TABLE: &[u32] = &[
    0xBE82_0300, // s_mov_b32 s2, s0
    0xBE83_0380, // s_mov_b32 s3, 0
    0xF408_0501, // s_load_dwordx4 s[20:23], s[2:3], 64
    0xFA00_0040,
];

/// Stores s4 and s5 at bytes 0 and 4 of the UAV.
const STORE: &[u32] = &[
    0x7E08_0204, // v_mov_b32 v4, s4
    0x7E0A_0205, // v_mov_b32 v5, s5
    0xE070_0000, // buffer_store_dword v4, off, s[20:23], 0
    0x8005_0400,
    0xE070_0004, // buffer_store_dword v5, off, s[20:23], 0 offset:4
    0x8005_0500,
    0xBF81_0000, // s_endpgm
];

/// `GetDimensions` of a texture as the console compiler expands it, from
/// the descriptor in s[8:15]: width in s4, height in s5, base level in s6.
const SIZES: &[u32] = &[
    0x9004_9E09, // s_lshr_b32 s4, s9, 30
    0x8F05_820A, // s_lshl_b32 s5, s10, 2
    0x8804_0504, // s_or_b32 s4, s4, s5
    0x8704_FF04, // s_and_b32 s4, s4, 0xffff
    0x0000_FFFF,
    0x9385_FF0A, // s_bfe_u32 s5, s10, 0x10000e
    0x0010_000E,
    0x9386_FF0B, // s_bfe_u32 s6, s11, 0x4000c
    0x0004_000C,
    0x8104_8104, // s_add_i32 s4, s4, 1
    0x8105_8105, // s_add_i32 s5, s5, 1
    0x9004_0604, // s_lshr_b32 s4, s4, s6
    0x9005_0605, // s_lshr_b32 s5, s5, s6
];

/// s_load_dwordx8 s[8:15], s[6:7], s12: entry 1 of the view heap.
const FROM_HEAP: &[u32] = &[
    0xBE86_0301, // s_mov_b32 s6, s1
    0xBE87_0380, // s_mov_b32 s7, 0
    0xBE8C_03A0, // s_mov_b32 s12, 32
    0xF40C_0203,
    0x1800_0000,
];

/// `s_load_dwordx4 s[8:11], s[2:3], 0`, then the record count in s4 and 0
/// in s5.
const COUNT: &[u32] = &[
    0xF408_0201,
    0xFA00_0000,
    0xBE84_030A, // s_mov_b32 s4, s10
    0xBE85_0380, // s_mov_b32 s5, 0
];

/// A puddle pass of the title picks three textures from the view heap,
/// reads the first one's size and samples the other two.
#[test]
fn the_size_of_a_heap_texture_nothing_reads() {
    let words = [TABLE, FROM_HEAP, SIZES, STORE].concat();
    let shader = build(&words, (8, 0)).expect("translates");
    let (one, texels) = ([[0.0; 4]; 1], [[0.0; 4]; 15]);
    let views = [
        View::Texture(1, 1, &one),
        View::Texture(5, 3, &texels),
        View::RawUav(2),
    ];
    for (label, got) in run(&shader, &views) {
        assert_eq!(got[..2], [5, 3], "{label}");
    }
}

/// A slot that shows no texture (a null view) is one texel each way to the program, as the
/// console's empty descriptor reads: the host's count of none, less one, filled the fields
/// (65,536 each way). `LEONIDA_NULL_VIEW_SIZE=0`: as before.
#[test]
fn the_size_of_a_null_texture_is_one_texel() {
    let words = [TABLE, FROM_HEAP, SIZES, STORE].concat();
    let one = [[0.0; 4]; 1];
    let views = [View::Texture(1, 1, &one), View::NullTexture, View::RawUav(2)];
    let shader = build(&words, (8, 0)).expect("translates");
    for (label, got) in run(&shader, &views) {
        assert_eq!(got[..2], [1, 1], "{label}");
    }
    let was = lower::TEST_NULL_VIEW_SIZE.replace(Some(false));
    let before = build(&words, (8, 0)).expect("translates");
    lower::TEST_NULL_VIEW_SIZE.set(was);
    for (label, got) in run(&before, &views) {
        assert_eq!(got[..2], [65536, 65536], "{label}: without the floor");
    }
    // (a texture that is there keeps its size either way)
    let texels = [[0.0; 4]; 15];
    let views = [View::Texture(1, 1, &one), View::Texture(5, 3, &texels), View::RawUav(2)];
    for (label, got) in run(&shader, &views) {
        assert_eq!(got[..2], [5, 3], "{label}");
    }
}

/// Without the height nothing tells a 2D texture from the others: taken for
/// one, as most are. A volume's depth or an array's layers are no 2D
/// texture's.
#[test]
fn sizes_of_a_volume_or_an_array_stay_untranslated() {
    let width = [
        0x9004_9E09, // s_lshr_b32 s4, s9, 30
        0x8F05_820A, // s_lshl_b32 s5, s10, 2
        0x8804_0504, // s_or_b32 s4, s4, s5
    ];
    let words = [TABLE, FROM_HEAP, &width, STORE].concat();
    assert!(build(&words, (8, 0)).is_ok());
    let depth = [
        0x8805_0C05, // s_or_b32 s5, s5, s12
    ];
    let words = [TABLE, FROM_HEAP, SIZES, &depth, STORE].concat();
    assert!(build(&words, (8, 0)).is_err());
}

/// A water simulation step runs only while a structured buffer is bound:
/// it tests the record count of a view it never reads.
#[test]
fn the_record_count_of_a_structured_buffer_nothing_reads() {
    let words = [TABLE, COUNT, STORE].concat();
    let shader = build(&words, (4, 16)).expect("translates");
    let (data, texel) = ([0u32; 12], [[0.0; 4]; 1]);
    // The runtime makes structured views raw.
    let views = [
        View::Buffer {
            data: &data,
            stride: Stride::Raw,
        },
        View::Texture(1, 1, &texel),
        View::RawUav(2),
    ];
    for (label, got) in run(&shader, &views) {
        assert_eq!(got[..2], [3, 0], "{label}");
    }
    // A stride of 4 or less may be a typed view's component count.
    assert!(build(&words, (4, 4)).is_err());
    assert!(build(&words, (4, 0)).is_err());
}

/// As [`the_record_count_of_a_structured_buffer_nothing_reads`], of one the
/// program also reads by index.
#[test]
fn the_record_count_of_a_structured_buffer_read_by_index() {
    let load = [
        0x7E06_0280, // v_mov_b32 v3, 0
        0xE030_2000, // buffer_load_dword v5, v3, s[8:11], 0 idxen
        0x8002_0503,
        0x7E0A_0505, // v_readfirstlane_b32 s5, v5
    ];
    let words = [TABLE, COUNT, &load, STORE].concat();
    let shader = build(&words, (4, 16)).expect("translates");
    let (mut data, texel) = ([0u32; 12], [[0.0; 4]; 1]);
    data[0] = 77;
    let views = [
        View::Buffer {
            data: &data,
            stride: Stride::Raw,
        },
        View::Texture(1, 1, &texel),
        View::RawUav(2),
    ];
    for (label, got) in run(&shader, &views) {
        assert_eq!(got[..2], [3, 77], "{label}");
    }
}

/// An upscaler counts its groups in a texture with atomics and sets the
/// counter back with a store of one channel, 16 bits wide.
#[test]
fn a_counter_texture_is_set_by_a_store_of_one_channel() {
    let program = |store: [u32; 2], value: u32| {
        let words = [
            0xF40C_0201u32, // s_load_dwordx8 s[8:15], s[2:3], 0
            0xFA00_0000,
            0x7E00_0280, // v_mov_b32 v0, 0
            0x7E02_0280, // v_mov_b32 v1, 0
            0x7E04_0283, // v_mov_b32 v2, 3
            0xF044_3108, // image_atomic_add v2, v[0:1], s[8:15] dmask:0x1 2D glc
            0x0002_0200,
            value, // v_mov_b32 v3, ..
            store[0],
            store[1],
            0x7E08_0281, // v_mov_b32 v4, 1
            0xF044_3108, // image_atomic_add v4, v[0:1], s[8:15] dmask:0x1 2D glc
            0x0002_0400,
            0xBE84_0380, // s_mov_b32 s4, 0
            0xBE85_0380, // s_mov_b32 s5, 0
        ];
        build(&[TABLE, &words, STORE].concat(), (8, 0))
    };
    // image_store v3, v[0:1], s[8:15] dmask:0x1 2D, with and without d16
    for store in [[0xF020_1108, 0x0002_0300], [0xF020_1108, 0x8002_0300]] {
        let shader = program(store, 0x7E06_0289).expect("translates"); // v3 = 9
        let texel = [[0.0; 4]; 1];
        let views = [
            View::UintTextureUav(1, 1),
            View::Texture(1, 1, &texel),
            View::RawUav(2),
        ];
        for (label, got) in run(&shader, &views) {
            assert_eq!(got[0], 10, "{label}");
        }
    }
    // Sixteen bits of a number only the card knows may be signed.
    assert!(program([0xF020_1108, 0x8002_0300], 0x7E06_0302).is_err()); // v3 = v2
    // A store of two channels says nothing about the view.
    assert!(program([0xF020_1308, 0x0002_0300], 0x7E06_0289).is_err());
}

/// A load past the wave's cache (GLC) of a buffer other groups write: its
/// view is globally coherent on the host, as is every access to it.
#[test]
fn a_buffer_loaded_past_the_cache_is_coherent() {
    let program = |load: u32| {
        let words = [
            0x7E04_028Bu32, // v_mov_b32 v2, 11
            0xE070_0008,    // buffer_store_dword v2, off, s[20:23], 0 offset:8
            0x8005_0200,
            load, // buffer_load_dword v3, off, s[20:23], 0 offset:8
            0x8005_0300,
            0x7E08_0503, // v_readfirstlane_b32 s4, v3
            0xBE85_0380, // s_mov_b32 s5, 0
        ];
        build(&[TABLE, &words, STORE].concat(), (8, 0)).expect("translates")
    };
    let (plain, coherent) = (program(0xE030_0008), program(0xE030_C008));
    // the annotations of the store's and the load's handles
    assert_ne!(plain.dxil, coherent.dxil);
    let texel = [[0.0; 4]; 1];
    let views = [
        View::Texture(1, 1, &texel),
        View::Texture(1, 1, &texel),
        View::RawUav(3),
    ];
    for shader in [&plain, &coherent] {
        for (label, got) in run(shader, &views) {
            assert_eq!(got[..3], [11, 0, 11], "{label}");
        }
    }
}

/// A buffer picked from the view heap and read as raw bytes is read at the
/// host's raw twin of its slot, when the host keeps some; an index past the
/// title's slots reads the last twin, not a slot of something else.
#[test]
fn a_picked_buffer_is_read_at_its_raw_twin() {
    let program = |at: u32| {
        let words = [
            0xBE86_0301, // s_mov_b32 s6, s1
            0xBE87_0380, // s_mov_b32 s7, 0
            0xBE8C_03FF, // s_mov_b32 s12, at
            at,
            0xF408_0203, // s_load_dwordx4 s[8:11], s[6:7], s12
            0x1800_0000,
            0xE030_0004, // buffer_load_dword v5, off, s[8:11], 0 offset:4
            0x8002_0500,
            0x7E0A_0505, // v_readfirstlane_b32 s5, v5
            0xBE84_0380, // s_mov_b32 s4, 0
        ];
        [TABLE, &words, STORE].concat()
    };
    let (title, twin, last, texel) = ([1u32, 2, 3, 4], [5u32, 6, 7, 8], [9u32, 10, 11, 12], [[0.0; 4]; 1]);
    let raw = |data| View::Buffer { data, stride: Stride::Raw };
    // (The store goes through the table's third entry: its twin, too.)
    let views = |twins: bool| {
        let uav = |here: bool| if here { View::RawUav(2) } else { View::Texture(1, 1, &texel) };
        [
            View::Texture(1, 1, &texel),
            raw(&title),
            uav(!twins),
            View::Texture(1, 1, &texel),
            View::Texture(1, 1, &texel),
            raw(&twin),
            uav(twins),
            raw(&last),
        ]
    };
    for (first, at, expected) in [(0, 32, 2), (4, 32, 6), (4, 32 * 1000, 10)] {
        super::shader::RAW_VIEWS_HERE.set(Some(first));
        let shader = build(&program(at), (8, 0)).expect("translates");
        super::shader::RAW_VIEWS_HERE.set(None);
        for (label, got) in run(&shader, &views(first != 0)) {
            assert_eq!(got[..2], [0, expected], "{label}: twins from {first}, byte {at}");
        }
    }
}

/// A table's buffer read as raw bytes is read at its raw twin too.
#[test]
fn a_tables_raw_buffer_is_read_at_its_raw_twin() {
    let load = [
        0xF408_0201u32, // s_load_dwordx4 s[8:11], s[2:3], 0
        0xFA00_0000,
        0xE030_0004, // buffer_load_dword v5, off, s[8:11], 0 offset:4
        0x8002_0500,
        0x7E0A_0505, // v_readfirstlane_b32 s5, v5
        0xBE84_0380, // s_mov_b32 s4, 0
    ];
    let words = [TABLE, &load, STORE].concat();
    let (title, twin, texel) = ([1u32, 2, 3, 4], [5u32, 6, 7, 8], [[0.0; 4]; 1]);
    let raw = |data| View::Buffer { data, stride: Stride::Raw };
    super::shader::RAW_VIEWS_HERE.set(Some(4));
    let shader = build(&words, (4, 0)).expect("translates");
    super::shader::RAW_VIEWS_HERE.set(None);
    let other = || View::Texture(1, 1, &texel);
    let views = [raw(&title), other(), other(), other(), raw(&twin), other(), View::RawUav(2)];
    for (label, got) in run(&shader, &views) {
        assert_eq!(got[..2], [0, 6], "{label}");
    }
}

/// LOCAL: with `LEONIDA_NAN_SCRUB=1` an image store keeps 32 bits that no float instruction
/// of the program made: they may be an integer's (the view's format is not known to the
/// translation, and the store passes the bits as they are). All-ones, which is "no number"
/// read as a float, was made 0: a mask with every bit set came out empty. What a float
/// instruction made is still scrubbed, and so are the bits a GPU's arithmetic gives for "no
/// number" and the infinities, wherever they come from.
#[test]
fn the_nan_scrub_keeps_the_integer_bits_an_image_store_writes() {
    // v2 = `value` (two words), stored to texel (0, 0) of the image in the table's first entry
    let program = |value: &[u32]| {
        let mut words = vec![
            0xF40C_0201u32, // s_load_dwordx8 s[8:15], s[2:3], 0
            0xFA00_0000,
            0x7E00_0280, // v_mov_b32 v0, 0
            0x7E02_0280, // v_mov_b32 v1, 0
            0x7E06_0280, // v_mov_b32 v3, 0
            0x7E08_0280, // v_mov_b32 v4, 0
            0x7E0A_0280, // v_mov_b32 v5, 0
        ];
        words.extend_from_slice(value);
        words.extend_from_slice(&[
            0xF020_1F08, // image_store v[2:5], v[0:1], s[8:15] dmask:0xf 2D
            0x0002_0200,
            0xBE84_0380, // s_mov_b32 s4, 0
            0xBE85_0380, // s_mov_b32 s5, 0
        ]);
        build(&[TABLE, &words, STORE].concat(), (8, 0)).expect("translates")
    };
    let moved = |bits: u32| [0x7E04_02FF, bits]; // v_mov_b32 v2, bits
    let texel = [[0.0; 4]; 1];
    let was = lower::TEST_NAN_SCRUB.replace(Some(1));
    // an R32_UINT image: (the bits moved in, what the texel holds)
    let integers = [
        (0xFFFF_FFFFu32, 0xFFFF_FFFFu32),
        (0xFF80_0001, 0xFF80_0001),
        (0x7F80_0001, 0x7F80_0001),
        (0x1234_5678, 0x1234_5678),
        // the patterns arithmetic makes, taken for what a float computation left: "no
        // number" is 0, an infinity the largest finite half-float of its sign
        (0x7FFF_FFFF, 0),
        (0xFFC0_0000, 0),
        (0x7F80_0000, 0x477F_E000),
        (0xFF80_0000, 0xC77F_E000),
    ];
    let shaders: Vec<_> = integers.iter().map(|&(bits, _)| program(&moved(bits))).collect();
    // a float image: 0 x infinity, made by a float instruction, is no number on any card
    let product = program(&[
        0x7E0C_0280, // v_mov_b32 v6, 0
        0x7E0E_02FF, // v_mov_b32 v7, infinity
        0x7F80_0000,
        0x1004_0F06, // v_mul_f32 v2, v6, v7
    ]);
    // 2 x an infinity of either sign, made by a float instruction: (the infinity, what the
    // texel holds), then the same with `LEONIDA_NAN_SCRUB_INF=0` (0, as before), computed and
    // as bits moved in
    let twice = |infinity: u32| {
        program(&[
            0x7E0C_02FF, // v_mov_b32 v6, 2.0
            0x4000_0000,
            0x7E0E_02FF, // v_mov_b32 v7, infinity
            infinity,
            0x1004_0F06, // v_mul_f32 v2, v6, v7
        ])
    };
    let mut infinities: Vec<_> =
        [(0x7F80_0000u32, 0x477F_E000u32), (0xFF80_0000, 0xC77F_E000)].map(|(i, want)| (twice(i), i, want)).into();
    let held = lower::TEST_NAN_SCRUB_INF.replace(Some(false));
    infinities.push((twice(0x7F80_0000), 0x7F80_0000, 0));
    infinities.push((program(&moved(0xFF80_0000)), 0xFF80_0000, 0));
    lower::TEST_NAN_SCRUB_INF.set(held);
    // `LEONIDA_NAN_SCRUB=2`: every image store as a float, all-ones too
    lower::TEST_NAN_SCRUB.set(Some(2));
    let all = program(&moved(0xFFFF_FFFF));
    lower::TEST_NAN_SCRUB.set(was);
    let unscrubbed = program(&moved(0x7F80_0000));
    for (shader, (bits, want)) in shaders.iter().zip(integers) {
        let views = [View::UintTextureUav(1, 1), View::Texture(1, 1, &texel), View::RawUav(2)];
        for (label, got) in run(shader, &views) {
            assert_eq!(got[0], want, "{label}: {bits:#010x} stored to an R32_UINT image");
        }
    }
    let views = [View::UintTextureUav(1, 1), View::Texture(1, 1, &texel), View::RawUav(2)];
    for (label, got) in run(&all, &views) {
        assert_eq!(got[0], 0, "{label}: all-ones stored to an R32_UINT image, every store scrubbed");
    }
    let views = [View::TextureUav(1, 1), View::Texture(1, 1, &texel), View::RawUav(2)];
    for (label, got) in run(&product, &views) {
        assert_eq!(got[0], 0, "{label}: 0 x infinity stored to a float image");
    }
    for (shader, infinity, want) in &infinities {
        for (label, got) in run(shader, &views) {
            assert_eq!(got[0], *want, "{label}: {infinity:#010x} stored to a float image");
        }
    }
    // (without the switch the store writes what it is given)
    for (label, got) in run(&unscrubbed, &views) {
        assert_eq!(got[0], 0x7F80_0000, "{label}: an infinity, the scrub off");
    }
}
