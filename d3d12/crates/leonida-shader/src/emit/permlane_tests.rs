//! Synthetic row permutations on hosts supporting waves of at least 32 lanes.

use super::host::{Dispatch, Host, Root, View};
use super::{HostShader, lower, validator};
use crate::native::{
    ComputeIo, HwStage, NativeProgram, ResourceType, RootParam, RootType, StageIo, UsedResource,
};
use crate::{ShaderError, io, ir, resource, structure};

const THREADS: u32 = 128;
const OLD: u32 = 42;
const BASE: u32 = 0x1234_0000;

fn native(cross: bool, fi: bool, bc: bool, alias: bool, wave64: bool) -> NativeProgram {
    let mov = |dst: u32, src: u32| 0x7E00_0200 | dst << 17 | src;
    let smov = |dst: u32, src: u32| 0xBE80_0300 | dst << 16 | src;
    let mut words = vec![
        smov(5, 128),
        0x3D << 26 | 0x02 << 18 | 8 << 6 | 2,
        125 << 25,   // s_load_dwordx4 s[8:11], s[4:5], 0
        0x340E_0082, // v_lshlrev_b32 v7, 2, v0 (output byte offset)
        0x4A0A_00FF,
        BASE, // v_add_nc_u32 v5, BASE, v0
        mov(6, if alias { 256 + 5 } else { 128 + OLD }),
    ];
    words.push(if wave64 { 0xBEFE_0402 } else { smov(126, 2) }); // exec = s[2:3] / s2
    words.extend([
        0xD400_0006
            | (if cross { 0x378 } else { 0x377 }) << 16
            | u32::from(fi) << 11
            | u32::from(bc) << 12,
        (256 + if alias { 6 } else { 5 }) | (1 << 18),
    ]); // v_permlane[x]16_b32 v6, v5/v6, s0, s1
    words.push(if wave64 { 0xBEFE_04C1 } else { smov(126, 193) }); // restore EXEC
    words.extend([
        0x38 << 26 | 0x1C << 18 | 1 << 12,
        0x80 << 24 | 2 << 16 | 6 << 8 | 7, // buffer_store_dword v6, v7, s[8:11], 0 offen
        0xBF81_0000,
    ]);
    NativeProgram {
        stage: HwStage::Cs,
        wave64,
        vgprs: 8,
        layout: vec![
            RootParam {
                root: RootType::Constants,
                table_offset_dwords: 0,
                sgprs: 4,
                base_register: 0,
                user_data: 0,
                slot: 0,
            },
            RootParam {
                root: RootType::Table,
                table_offset_dwords: 0,
                sgprs: 1,
                base_register: 0,
                user_data: 4,
                slot: 1,
            },
        ],
        used: vec![UsedResource {
            kind: ResourceType::Uav,
            root: RootType::Table,
            table_offset_dwords: 0,
            lower_bound: 0,
            stride: 0,
            dwords: 4,
            user_data: 4,
            slot: 1,
        }],
        io: StageIo::Cs(ComputeIo {
            rsrc2: 5 << 1,
            threads: [THREADS, 1, 1],
        }),
        code: words.iter().flat_map(|w| w.to_le_bytes()).collect(),
    }
}

fn lowered(native: &NativeProgram, host_wave: u32) -> Result<HostShader, ShaderError> {
    let p = ir::build(&native.code, native.wave64, native.vgprs).expect("ir");
    let st = structure::structure(&p).expect("structure");
    let res = resource::track(&p, &native.layout, native.stage, Some(&st)).expect("resources");
    let io = io::raise(&p, native).expect("io");
    let was = lower::TEST_HOST_WAVE.replace(Some(host_wave));
    let result = lower::lower(&lower::Parts {
        native,
        p: &p,
        st: &st,
        io: &io,
        res: &res,
        inputs: Vec::new(),
        outputs: Vec::new(),
        prims: Vec::new(),
        mesh_stride: 0,
    });
    lower::TEST_HOST_WAVE.set(was);
    let result = result?;
    let shader = HostShader {
        stage: HwStage::Cs,
        dxil: result.dxil,
        user_data: result.user_data,
        samplers: result.samplers,
        static_samplers: Vec::new(),
        root_cbvs: result.root_cbvs,
        root_srvs: result.root_srvs,
        root_uavs: result.root_uavs,
        root_indexed_views: res.root_indexed_views,
        half_views: Vec::new(),
    };
    match validator::Validator::load(validator::Validator::default_path().as_deref()) {
        Ok(mut v) => {
            let mut copy = shader.dxil.clone();
            v.validate(&mut copy).expect("DXIL validation");
            assert_eq!(copy[4..20], shader.dxil[4..20], "container hash");
        }
        Err(e) => eprintln!("DXIL validator unavailable: {e}"),
    }
    Ok(shader)
}

#[test]
fn permlane_fetch_inactive_outside_the_host_branch_is_rejected() {
    let mut native = native(true, true, false, false, false);
    let mut words: Vec<u32> = native
        .code
        .chunks_exact(4)
        .map(|w| u32::from_le_bytes(w.try_into().unwrap()))
        .collect();
    // Skip the permutation when EXEC is empty. Structurisation turns this
    // into a host lane branch, so FI cannot read lanes that took the skip.
    words.insert(8, 0xBF88_0002); // s_cbranch_execz +2 (to the EXEC restore)
    native.code = words.iter().flat_map(|w| w.to_le_bytes()).collect();
    assert!(matches!(
        lowered(&native, 32),
        Err(ShaderError::Unsupported {
            what: "a permlane fetch from lanes outside the host branch",
            ..
        })
    ));
}

#[test]
fn permlane_rows_and_inactive_sources() {
    let _one = super::valu_tests::ONE_AT_A_TIME
        .lock()
        .unwrap_or_else(|e| e.into_inner());
    let mut ran = 0;
    for (backend, warp) in [("hardware", false), ("WARP", true)] {
        let Ok(host) = Host::new(warp) else {
            eprintln!("no SM 6.6 {backend}; skipped");
            continue;
        };
        let mut options =
            windows::Win32::Graphics::Direct3D12::D3D12_FEATURE_DATA_D3D12_OPTIONS1::default();
        // SAFETY: options is the correctly sized output for this feature query.
        unsafe {
            host.device
                .CheckFeatureSupport(
                    windows::Win32::Graphics::Direct3D12::D3D12_FEATURE_D3D12_OPTIONS1,
                    (&raw mut options).cast(),
                    size_of_val(&options) as u32,
                )
                .expect("wave capabilities");
        }
        eprintln!(
            "{backend} waves {}..{}",
            options.WaveLaneCountMin, options.WaveLaneCountMax
        );
        if options.WaveLaneCountMax < 32 {
            eprintln!("{backend} cannot hold two 16-lane rows; GPU checks skipped");
            continue;
        }
        ran += 1;
        eprintln!("permlane: testing row and EXEC cases on {backend}");
        for wave64 in [false, true] {
            if options.WaveLaneCountMin > if wave64 { 64 } else { 32 } {
                eprintln!("{backend} does not support the requested console wave size; skipped");
                continue;
            }
            for cross in [false, true] {
                for fi in [false, true] {
                    for bc in [false, true] {
                        for alias in [false, true] {
                            let shader = lowered(
                                &native(cross, fi, bc, alias, wave64),
                                options.WaveLaneCountMax,
                            )
                            .expect("lower permlane");
                            let (root, pso) =
                                host.compute_pipeline(&shader).expect("permlane pipeline");
                            for selectors in [
                                [0x7654_3210u32, 0xFEDC_BA98], // identity
                                [0x89AB_CDEF, 0x0123_4567],    // reverse
                                [0, 0],                        // broadcast lane 0
                                [u32::MAX, u32::MAX],          // broadcast lane 15
                                [0xF10E_2D3C, 0x4B5A_6978],    // arbitrary; both selector words
                            ] {
                                for mask in [u32::MAX, 0x5555_5555, 0xFFFF_0000, 1, 0] {
                                    let constants = [selectors[0], selectors[1], mask, mask];
                                    let mut ud = vec![0; shader.user_data_dwords() as usize];
                                    for u in &shader.user_data {
                                        ud[u.index as usize] = if u.root == RootType::Constants {
                                            constants[u.dword as usize]
                                        } else {
                                            0
                                        };
                                    }
                                    let out = host
                                        .dispatch(
                                            &root,
                                            &pso,
                                            &Dispatch {
                                                roots: vec![(0, Root::Constants(ud))],
                                                views: &[View::RawUav(THREADS)],
                                                samplers: &[],
                                                groups: [1, 1, 1],
                                            },
                                        )
                                        .expect("dispatch permlane");
                                    for t in 0..THREADS {
                                        let pos = t % 16;
                                        let packed =
                                            u64::from(selectors[0]) | u64::from(selectors[1]) << 32;
                                        let pick = (packed >> (4 * pos)) as u32 & 15;
                                        let src = ((t & !15) ^ if cross { 16 } else { 0 }) | pick;
                                        let old = if alias { BASE + t } else { OLD };
                                        let want = if mask >> (t % 32) & 1 == 0 {
                                            old
                                        } else if fi || mask >> (src % 32) & 1 != 0 {
                                            BASE + src
                                        } else if bc {
                                            0
                                        } else {
                                            old
                                        };
                                        let got = out[0][t as usize];
                                        assert_eq!(
                                            got, want,
                                            "{backend} wave64={wave64} cross={cross} fi={fi} bc={bc} alias={alias} selectors={selectors:x?} mask={mask:x} lane={t}"
                                        );
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        eprintln!("permlane: all row and EXEC cases passed on {backend}");
    }
    if ran == 0 {
        eprintln!("no SM 6.6 device with 32-lane rows; GPU checks skipped");
    }
}

fn lane_select_program(threads: u32, wave64: bool) -> NativeProgram {
    let smov = |dst: u32, src: u32| 0xBE80_0300 | dst << 16 | src;
    let mov = |dst: u32, src: u32| 0x7E00_0200 | dst << 17 | src;
    let words = [
        smov(5, 128),
        0x3D << 26 | 0x02 << 18 | 8 << 6 | 2,
        125 << 25,   // s_load_dwordx4 s[8:11], s[4:5], 0
        0x340E_0082, // v_lshlrev_b32 v7, 2, v0
        0x4A0A_00FF,
        BASE,               // v_add_nc_u32 v5, BASE, v0
        0xD761_0005, 1,     // v_writelane_b32 v5, s1, s0
        0xD760_0006, 0x505, // v_readlane_b32 s6, v5, s2
        mov(6, 6),          // v_mov_b32 v6, s6
        0xE070_1000,
        0x8002_0507, // buffer_store_dword v5, v7, s[8:11], 0 offen
        0xE070_1000 | (threads * 4),
        0x8002_0607, // buffer_store_dword v6, v7, s[8:11], 0 offen offset:threads*4
        0xBF81_0000,
    ];
    NativeProgram {
        stage: HwStage::Cs,
        wave64,
        vgprs: 8,
        layout: vec![
            RootParam {
                root: RootType::Constants,
                table_offset_dwords: 0,
                sgprs: 4,
                base_register: 0,
                user_data: 0,
                slot: 0,
            },
            RootParam {
                root: RootType::Table,
                table_offset_dwords: 0,
                sgprs: 1,
                base_register: 0,
                user_data: 4,
                slot: 1,
            },
        ],
        used: vec![UsedResource {
            kind: ResourceType::Uav,
            root: RootType::Table,
            table_offset_dwords: 0,
            lower_bound: 0,
            stride: 0,
            dwords: 4,
            user_data: 4,
            slot: 1,
        }],
        io: StageIo::Cs(ComputeIo {
            rsrc2: 5 << 1,
            threads: [threads, 1, 1],
        }),
        code: words.into_iter().flat_map(u32::to_le_bytes).collect(),
    }
}

#[test]
fn readlane_and_writelane_mask_lane_select_modulo_wave_size() {
    const WRITTEN: u32 = 0x7788_99AA;
    let _one = super::valu_tests::ONE_AT_A_TIME
        .lock()
        .unwrap_or_else(|e| e.into_inner());
    for (backend, warp) in [("hardware", false), ("WARP", true)] {
        let Ok(host) = Host::new(warp) else {
            eprintln!("no SM 6.6 {backend}; skipped");
            continue;
        };
        let mut options =
            windows::Win32::Graphics::Direct3D12::D3D12_FEATURE_DATA_D3D12_OPTIONS1::default();
        // SAFETY: options is the correctly sized output for this feature query.
        unsafe {
            host.device
                .CheckFeatureSupport(
                    windows::Win32::Graphics::Direct3D12::D3D12_FEATURE_D3D12_OPTIONS1,
                    (&raw mut options).cast(),
                    size_of_val(&options) as u32,
                )
                .expect("wave capabilities");
        }
        if options.WaveLaneCountMax < 32 || options.WaveLaneCountMin > 32 {
            continue;
        }
        let mut host_waves = vec![32u32];
        if options.WaveLaneCountMax >= 64 {
            host_waves.push(64);
        }
        for &host_wave in &host_waves {
            for &(wave64, threads) in &[(false, 64u32), (true, 128), (true, 32)] {
                if !wave64 && host_wave != 32 {
                    continue;
                }
                let shader = lowered(&lane_select_program(threads, wave64), host_wave)
                    .expect("lower lane_select_program");
                let (root, pso) = host
                    .compute_pipeline(&shader)
                    .expect("lane_select_program pipeline");
                let wave_lanes = if wave64 { 64u32 } else { 32u32 };
                let mask = wave_lanes - 1;
                let cases: &[(u32, u32)] = if wave64 {
                    &[
                        (67, 131),           // 67 & 63 == 3, 131 & 63 == 3
                        (96, 160),           // 96 & 63 == 32 (upper half), 160 & 63 == 32
                        (u32::MAX, 67),      // -1 & 63 == 63, read 3
                        (64, u32::MAX),      // 64 & 63 == 0, read 63
                        (133, 99),           // write 5, read 35 (inactive when threads == 32)
                    ]
                } else {
                    &[
                        (35, 67),            // 35 & 31 == 3, 67 & 31 == 3
                        (u32::MAX, 99),      // -1 & 31 == 31, 99 & 31 == 3
                        (64, u32::MAX),      // 64 & 31 == 0, -1 & 31 == 31
                    ]
                };
                for &(w_sel, r_sel) in cases {
                    let constants = [w_sel, WRITTEN, r_sel, 0];
                    let mut ud = vec![0; shader.user_data_dwords() as usize];
                    for u in &shader.user_data {
                        if u.root == RootType::Constants {
                            ud[u.index as usize] = constants[u.dword as usize];
                        }
                    }
                    let out = host
                        .dispatch(
                            &root,
                            &pso,
                            &Dispatch {
                                roots: vec![(0, Root::Constants(ud))],
                                views: &[View::RawUav(threads * 2)],
                                samplers: &[],
                                groups: [1, 1, 1],
                            },
                        )
                        .expect("dispatch lane_select_program");
                    let w_lane = w_sel & mask;
                    let r_lane = r_sel & mask;
                    for t in 0..threads {
                        let wave_start = t & !mask;
                        let lane = t & mask;
                        let want_written = if lane == w_lane { WRITTEN } else { BASE + t };
                        let src_t = wave_start + r_lane;
                        let want_read = if src_t >= threads {
                            0
                        } else if r_lane == w_lane {
                            WRITTEN
                        } else {
                            BASE + src_t
                        };
                        assert_eq!(
                            out[0][t as usize],
                            want_written,
                            "{backend} host_wave={host_wave} wave64={wave64} threads={threads} w_sel={w_sel:#x} lane={t}: writelane mismatch"
                        );
                        assert_eq!(
                            out[0][(threads + t) as usize],
                            want_read,
                            "{backend} host_wave={host_wave} wave64={wave64} threads={threads} w_sel={w_sel:#x} r_sel={r_sel:#x} lane={t}: readlane mismatch"
                        );
                    }
                }
            }
        }
    }
}

#[test]
fn pixel_computed_readlane_masks_to_host_wave_and_validates() {
    use crate::dxbc::{Container, FourCC};
    use crate::emit::container::SigElement;
    use crate::emit::emit;
    use crate::native::{PsInput, Usage};

    let sig = |name: &'static str, kind: u8, usage: u8| SigElement {
        name: name.into(),
        index: 0,
        kind,
        comp: 9,
        interp: 0,
        row: Some(0),
        col: 0,
        cols: 4,
        usage,
        half: false,
    };
    for wave64 in [false, true] {
        let words: &[u32] = &[
            0xD765_0000, 0x0001_00C1, // v_mbcnt_lo_u32_b32 v0, -1, 0
            0x7E14_0500,              // v_readfirstlane_b32 s10, v0
            0x810A_C10A,              // s_add_i32 s10, s10, -1
            0xD760_000B, 0x0000_1500, // v_readlane_b32 s11, v0, s10
            0x7E02_0C0B,              // v_cvt_f32_u32 v1, s11
            0xF800_180F, 0x0101_0101, // exp mrt0 v1 v1 v1 v1 done vm
            0xBF81_0000,              // s_endpgm
        ];
        let native = NativeProgram {
            stage: HwStage::Ps,
            wave64,
            vgprs: 8,
            layout: vec![],
            used: vec![],
            io: StageIo::Ps {
                inputs: vec![PsInput {
                    usage: Usage::Generic,
                    row: 0,
                    mask: 0b1111,
                    attr: 0,
                    flat: false,
                    fp16: false,
                    pack2: false,
                    row2: 0,
                }],
                input_addr: 0,
                db_shader_control: 0,
                patches: vec![],
            },
            code: words.iter().flat_map(|w| w.to_le_bytes()).collect(),
        };
        let isg1 = crate::emit::container::signature(&[sig("TEXCOORD", 0, 0)], true);
        let osg1 = crate::emit::container::signature(&[sig("SV_Target", 16, 15)], false);
        let bytes = crate::dxbc::build(&[(FourCC::ISG1, &isg1), (FourCC::OSG1, &osg1)]);
        let c = Container::parse(&bytes).unwrap();
        let p = ir::build(&native.patched_code(), native.wave64, native.vgprs).unwrap();
        let res = resource::track(&p, &native.layout, native.stage, None).unwrap();
        let io = io::raise(&p, &native).unwrap();
        let st = structure::structure(&p).unwrap();
        let mut shader = emit(&c, &native, &p, &st, &io, &res).unwrap();
        if let Ok(mut v) = crate::emit::validator::Validator::load(
            crate::emit::validator::Validator::default_path().as_deref(),
        ) {
            v.validate(&mut shader.dxil)
                .unwrap_or_else(|e| panic!("wave64={wave64}: {e}"));
        }
    }
}

/// Under `with_local_waves_any`, `v_writelane_b32` must compare `console_lane()` (`0..63`)
/// against `lane & 63` rather than `lane_index()` (`0..31`) against `lane & 31`, so writing
/// console lane `L + 32` does not overwrite console lane `L` in the lower host wave.
#[test]
fn local_any_writelane_does_not_alias_lower_and_upper_halves() {
    const VAL_LO: u32 = 0x1111_2222;
    const VAL_HI: u32 = 0x3333_4444;
    let smov = |dst: u32, src: u32| 0xBE80_0300 | dst << 16 | src;
    let mov = |dst: u32, src: u32| 0x7E00_0200 | dst << 17 | src;
    let prog = |threads: u32| {
        let words = [
            smov(5, 128),
            0x3D << 26 | 0x02 << 18 | 8 << 6 | 2,
            125 << 25,          // s_load_dwordx4 s[8:11], s[4:5], 0
            0x340E_0082,        // v_lshlrev_b32 v7, 2, v0
            0x4A0A_00FF,
            BASE,               // v_add_nc_u32 v5, BASE, v0
            0xD761_0005, 1,     // v_writelane_b32 v5, s1, s0
            0xD761_0005, 0x403, // v_writelane_b32 v5, s3, s2
            0xD760_0006, 0x105, // v_readlane_b32 s6, v5, s0
            0xD760_0007, 0x505, // v_readlane_b32 s7, v5, s2
            mov(6, 6),          // v_mov_b32 v6, s6
            mov(4, 7),          // v_mov_b32 v4, s7
            0xE070_1000,
            0x8002_0507, // buffer_store_dword v5, v7, s[8:11], 0 offen
            0xE070_1000 | (threads * 4),
            0x8002_0607, // buffer_store_dword v6, v7, s[8:11], 0 offen offset:threads*4
            0xE070_1000 | (threads * 8),
            0x8002_0407, // buffer_store_dword v4, v7, s[8:11], 0 offen offset:threads*8
            0xBF81_0000,
        ];
        NativeProgram {
            stage: HwStage::Cs,
            wave64: true,
            vgprs: 8,
            layout: vec![
                RootParam {
                    root: RootType::Constants,
                    table_offset_dwords: 0,
                    sgprs: 4,
                    base_register: 0,
                    user_data: 0,
                    slot: 0,
                },
                RootParam {
                    root: RootType::Table,
                    table_offset_dwords: 0,
                    sgprs: 1,
                    base_register: 0,
                    user_data: 4,
                    slot: 1,
                },
            ],
            used: vec![UsedResource {
                kind: ResourceType::Uav,
                root: RootType::Table,
                table_offset_dwords: 0,
                lower_bound: 0,
                stride: 0,
                dwords: 4,
                user_data: 4,
                slot: 1,
            }],
            io: StageIo::Cs(ComputeIo {
                rsrc2: 5 << 1,
                threads: [threads, 1, 1],
            }),
            code: words.into_iter().flat_map(u32::to_le_bytes).collect(),
        }
    };

    let _one = super::valu_tests::ONE_AT_A_TIME
        .lock()
        .unwrap_or_else(|e| e.into_inner());
    for (backend, warp) in [("hardware", false), ("WARP", true)] {
        let Ok(host) = Host::new(warp) else {
            continue;
        };
        let mut options =
            windows::Win32::Graphics::Direct3D12::D3D12_FEATURE_DATA_D3D12_OPTIONS1::default();
        // SAFETY: options is the correctly sized output for this feature query.
        unsafe {
            host.device
                .CheckFeatureSupport(
                    windows::Win32::Graphics::Direct3D12::D3D12_FEATURE_D3D12_OPTIONS1,
                    (&raw mut options).cast(),
                    size_of_val(&options) as u32,
                )
                .expect("wave capabilities");
        }
        if options.WaveLaneCountMax < 32 || options.WaveLaneCountMin > 32 {
            continue;
        }

        let shader64 =
            lower::with_local_waves_any(|| lowered(&prog(64), 32)).expect("lower local_any 64");
        let (root64, pso64) = host.compute_pipeline(&shader64).expect("pso 64");
        for &(sel_lo, sel_hi) in &[(8u32, 40u32), (72, 104), (31, u32::MAX)] {
            let constants = [sel_lo, VAL_LO, sel_hi, VAL_HI];
            let mut ud = vec![0; shader64.user_data_dwords() as usize];
            for u in &shader64.user_data {
                if u.root == RootType::Constants {
                    ud[u.index as usize] = constants[u.dword as usize];
                }
            }
            let out = host
                .dispatch(
                    &root64,
                    &pso64,
                    &Dispatch {
                        roots: vec![(0, Root::Constants(ud))],
                        views: &[View::RawUav(64 * 3)],
                        samplers: &[],
                        groups: [1, 1, 1],
                    },
                )
                .expect("dispatch local_any 64");
            let lo = sel_lo & 63;
            let hi = sel_hi & 63;
            for t in 0..64u32 {
                let want_v5 = if t == lo {
                    VAL_LO
                } else if t == hi {
                    VAL_HI
                } else {
                    BASE + t
                };
                assert_eq!(
                    out[0][t as usize], want_v5,
                    "{backend} sel=({sel_lo},{sel_hi}) lane={t}: v5 writelane mismatch"
                );
                if t < 32 {
                    assert_eq!(
                        out[0][(64 + t) as usize], VAL_LO,
                        "{backend} sel=({sel_lo},{sel_hi}) wave0 lane={t}: readlane(lo) corrupted"
                    );
                } else {
                    assert_eq!(
                        out[0][(128 + t) as usize], VAL_HI,
                        "{backend} sel=({sel_lo},{sel_hi}) wave1 lane={t}: readlane(hi) mismatch"
                    );
                }
            }
        }

        // Single-half group (32 threads) under `with_local_waves_any`: lanes 32..63 never run and read 0.
        let shader32 =
            lower::with_local_waves_any(|| lowered(&prog(32), 32)).expect("lower local_any 32");
        let (root32, pso32) = host.compute_pipeline(&shader32).expect("pso 32");
        let constants = [8u32, VAL_LO, 40u32, VAL_HI];
        let mut ud = vec![0; shader32.user_data_dwords() as usize];
        for u in &shader32.user_data {
            if u.root == RootType::Constants {
                ud[u.index as usize] = constants[u.dword as usize];
            }
        }
        let out32 = host
            .dispatch(
                &root32,
                &pso32,
                &Dispatch {
                    roots: vec![(0, Root::Constants(ud))],
                    views: &[View::RawUav(32 * 3)],
                    samplers: &[],
                    groups: [1, 1, 1],
                },
            )
            .expect("dispatch local_any 32");
        for t in 0..32u32 {
            let want_v5 = if t == 8 { VAL_LO } else { BASE + t };
            assert_eq!(out32[0][t as usize], want_v5, "{backend} 32-thread lane={t}: v5");
            assert_eq!(out32[0][(32 + t) as usize], VAL_LO, "{backend} 32-thread lane={t}: read lo");
            assert_eq!(out32[0][(64 + t) as usize], 0, "{backend} 32-thread lane={t}: read hi -> 0");
        }
    }
}
