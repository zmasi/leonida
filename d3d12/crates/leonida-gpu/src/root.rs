//! Root signatures.
//!
//! Serializing goes to the host's `D3D12SerializeRootSignature` (or its
//! versioned form): the description is laid out as on PC, and the blob it
//! returns works as the console's, whose two methods sit in the same slots.
//! The console's own flags (bit 24 and up) are dropped first.
#![allow(clippy::not_unsafe_ptr_arg_deref)]

use std::ffi::{CStr, c_void};
use std::sync::OnceLock;

use crate::object::{
    self, E_INVALIDARG, E_NOTIMPL, HRESULT, Iid, Object, ROOT_SIGNATURE, Table, This, guid,
};
use crate::resource::PAGEABLE_IIDS;

const ROOT_SIGNATURE_IIDS: [Iid; 3] = [
    guid(
        0xc54a6b66,
        0x72df,
        0x4ee8,
        [0x8b, 0xe5, 0xa9, 0x46, 0xa1, 0x42, 0x92, 0x14],
    ),
    PAGEABLE_IIDS[1],
    PAGEABLE_IIDS[2],
];

const CONSOLE_FLAGS: u32 = 0xff00_0000;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Desc {
    parameter_count: u32,
    parameters: *const c_void,
    sampler_count: u32,
    samplers: *const c_void,
    flags: u32,
}

/// Every version's description starts with the fields of [`Desc`].
#[repr(C)]
#[derive(Clone, Copy)]
pub struct VersionedDesc {
    version: u32,
    desc: Desc,
}

type Serialize =
    unsafe extern "system" fn(*const Desc, u32, *mut *mut c_void, *mut *mut c_void) -> HRESULT;
type SerializeVersioned =
    unsafe extern "system" fn(*const VersionedDesc, *mut *mut c_void, *mut *mut c_void) -> HRESULT;

/// An export of the host's `d3d12.dll` in System32, looked up once.
fn host_export(cache: &OnceLock<Option<usize>>, name: &CStr) -> Option<usize> {
    *cache.get_or_init(|| {
        #[cfg(windows)]
        {
            use windows_sys::Win32::System::LibraryLoader::{
                GetProcAddress, LOAD_LIBRARY_SEARCH_SYSTEM32, LoadLibraryExW,
            };
            let dll: Vec<u16> = "d3d12.dll".encode_utf16().chain([0]).collect();
            // SAFETY: a NUL-terminated name; System32 only.
            let module = unsafe {
                LoadLibraryExW(
                    dll.as_ptr(),
                    std::ptr::null_mut(),
                    LOAD_LIBRARY_SEARCH_SYSTEM32,
                )
            };
            if module.is_null() {
                return None;
            }
            // SAFETY: a loaded module and a NUL-terminated name.
            unsafe { GetProcAddress(module, name.as_ptr().cast()) }.map(|f| f as usize)
        }
        #[cfg(not(windows))]
        None
    })
}

fn without_console_flags(desc: Desc) -> Desc {
    if desc.flags & CONSOLE_FLAGS != 0 {
        tracing::debug!(
            flags = format_args!("{:#x}", desc.flags),
            "console root signature flags dropped"
        );
    }
    Desc {
        flags: desc.flags & !CONSOLE_FLAGS,
        ..desc
    }
}

fn clear_ptr<T>(out: *mut *mut T) {
    if !out.is_null() {
        // SAFETY: caller passed a non-null output slot; `write_unaligned` tolerates unaligned callers.
        unsafe { out.write_unaligned(std::ptr::null_mut()) };
    }
}

fn clear_const_ptr<T>(out: *mut *const T) {
    if !out.is_null() {
        // SAFETY: caller passed a non-null output slot; `write_unaligned` tolerates unaligned callers.
        unsafe { out.write_unaligned(std::ptr::null()) };
    }
}

/// Computes the 16-byte DXBC container checksum over `blob[20..]` (MD5 with
/// d3dcompiler's bit-length tail in words 0 and 15). Caller ensures
/// `blob.len() >= 20` and `blob.len() - 20 <= 0x1fff_ffff`.
fn dxbc_checksum(blob: &[u8]) -> [u8; 16] {
    const K: [u32; 64] = [
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613,
        0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193,
        0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d,
        0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
        0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122,
        0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
        0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244,
        0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb,
        0xeb86d391,
    ];
    const R: [u32; 16] = [7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21];
    let step = |state: &mut [u32; 4], m: &[u32; 16]| {
        let [mut a, mut b, mut c, mut d] = *state;
        for i in 0..64 {
            let (f, g) = match i {
                0..16 => ((b & c) | (!b & d), i),
                16..32 => ((d & b) | (!d & c), (5 * i + 1) % 16),
                32..48 => (b ^ c ^ d, (3 * i + 5) % 16),
                _ => (c ^ (b | !d), (7 * i) % 16),
            };
            let next_b = b.wrapping_add(
                a.wrapping_add(f)
                    .wrapping_add(K[i])
                    .wrapping_add(m[g])
                    .rotate_left(R[(i / 16) * 4 + (i % 4)]),
            );
            (a, d, c, b) = (d, c, b, next_b);
        }
        state[0] = state[0].wrapping_add(a);
        state[1] = state[1].wrapping_add(b);
        state[2] = state[2].wrapping_add(c);
        state[3] = state[3].wrapping_add(d);
    };
    let words = |block: &[u8; 64]| -> [u32; 16] {
        std::array::from_fn(|i| {
            let mut w = [0u8; 4];
            w.copy_from_slice(&block[i * 4..i * 4 + 4]);
            u32::from_le_bytes(w)
        })
    };

    let data = &blob[20..];
    let bits = (data.len() as u32) * 8;
    let mut state = [0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476];
    let full = (data.len() / 64) * 64;
    for chunk in data[..full].chunks_exact(64) {
        let mut raw = [0u8; 64];
        raw.copy_from_slice(chunk);
        step(&mut state, &words(&raw));
    }
    let left = data.len() - full;
    let mut block = [0u8; 64];
    let mut m = if left >= 56 {
        block[..left].copy_from_slice(&data[full..]);
        block[left] = 0x80;
        step(&mut state, &words(&block));
        [0u32; 16]
    } else {
        block[4..4 + left].copy_from_slice(&data[full..]);
        block[4 + left] = 0x80;
        words(&block)
    };
    m[0] = bits;
    m[15] = (bits >> 2) | 1;
    step(&mut state, &m);
    let mut out = [0u8; 16];
    for (i, w) in state.into_iter().enumerate() {
        out[i * 4..i * 4 + 4].copy_from_slice(&w.to_le_bytes());
    }
    out
}

fn read_u32_le(bytes: &[u8], at: usize) -> Option<u32> {
    let slice = bytes.get(at..at.checked_add(4)?)?;
    Some(u32::from_le_bytes([slice[0], slice[1], slice[2], slice[3]]))
}

/// If `bytes` is a valid DXBC container whose `RTS0` chunk (v1.0..v1.2) carries
/// Xbox console high-byte flags (`CONSOLE_FLAGS`), returns a copy with those
/// bits cleared and any non-zero DXBC checksum recomputed. Malformed containers,
/// invalid chunk tables, or mismatched incoming non-zero hashes return `None`
/// so the caller forwards the original bytes unchanged (fail closed).
fn strip_console_flags_from_rts0(bytes: &[u8]) -> Option<Vec<u8>> {
    if bytes.len() < 36 || bytes.len() - 20 > 0x1fff_ffff || &bytes[0..4] != b"DXBC" {
        return None;
    }
    let total_size = read_u32_le(bytes, 24)? as usize;
    if total_size != bytes.len() {
        return None;
    }
    let chunk_count = read_u32_le(bytes, 28)? as usize;
    if chunk_count == 0 || chunk_count > 16 {
        return None;
    }
    let min_chunk_off = 32usize.checked_add(chunk_count.checked_mul(4)?)?;
    if min_chunk_off > bytes.len() {
        return None;
    }

    let zero_hash = bytes[4..20] == [0u8; 16];
    if !zero_hash && dxbc_checksum(bytes) != bytes[4..20] {
        return None;
    }

    let mut rts0_off = None;
    for i in 0..chunk_count {
        let off = read_u32_le(bytes, 32 + i * 4)? as usize;
        if off < min_chunk_off || off > bytes.len() || bytes.len() - off < 8 {
            return None;
        }
        let chunk_size = read_u32_le(bytes, off + 4)? as usize;
        if chunk_size > bytes.len() - off - 8 {
            return None;
        }
        if rts0_off.is_none() && &bytes[off..off + 4] == b"RTS0" {
            if chunk_size < 24 {
                return None;
            }
            rts0_off = Some(off);
        }
    }
    let rts0_off = rts0_off?;
    let rts_version = read_u32_le(bytes, rts0_off + 8)?;
    if !(1..=3).contains(&rts_version) {
        return None;
    }
    let flags_off = rts0_off + 28;
    let flags = read_u32_le(bytes, flags_off)?;
    let cleared = flags & !CONSOLE_FLAGS;
    if cleared == flags {
        return None;
    }

    tracing::debug!(
        flags = format_args!("{flags:#x}"),
        "console root signature flags dropped from serialized RTS0 blob"
    );
    let mut copy = bytes.to_vec();
    copy[flags_off..flags_off + 4].copy_from_slice(&cleared.to_le_bytes());
    if !zero_hash {
        let hash = dxbc_checksum(&copy);
        copy[4..20].copy_from_slice(&hash);
    }
    Some(copy)
}

/// `d3d12_xs!D3D12SerializeRootSignature`.
pub extern "system" fn serialize(
    desc: *const Desc,
    version: u32,
    blob: *mut *mut c_void,
    error: *mut *mut c_void,
) -> HRESULT {
    if desc.is_null() {
        clear_ptr(blob);
        clear_ptr(error);
        return E_INVALIDARG;
    }
    // SAFETY: checked non-null; `read_unaligned` tolerates unaligned callers.
    let desc = unsafe { desc.read_unaligned() };
    static FUNCTION: OnceLock<Option<usize>> = OnceLock::new();
    let Some(address) = host_export(&FUNCTION, c"D3D12SerializeRootSignature") else {
        tracing::warn!("no host d3d12.dll to serialize root signatures");
        clear_ptr(blob);
        clear_ptr(error);
        return E_NOTIMPL;
    };
    // SAFETY: the export has this signature; it takes what the title passed.
    unsafe {
        std::mem::transmute::<usize, Serialize>(address)(
            &without_console_flags(desc),
            version,
            blob,
            error,
        )
    }
}

/// `d3d12_xs!D3D12SerializeVersionedRootSignature`.
pub extern "system" fn serialize_versioned(
    desc: *const VersionedDesc,
    blob: *mut *mut c_void,
    error: *mut *mut c_void,
) -> HRESULT {
    if desc.is_null() {
        clear_ptr(blob);
        clear_ptr(error);
        return E_INVALIDARG;
    }
    // SAFETY: checked non-null; `read_unaligned` tolerates unaligned callers.
    let raw = unsafe { desc.read_unaligned() };
    static FUNCTION: OnceLock<Option<usize>> = OnceLock::new();
    let Some(address) = host_export(&FUNCTION, c"D3D12SerializeVersionedRootSignature") else {
        tracing::warn!("no host d3d12.dll to serialize root signatures");
        clear_ptr(blob);
        clear_ptr(error);
        return E_NOTIMPL;
    };
    // Only versions 1.0..1.2 (`1..=3`) share `Desc`'s `flags` layout; forward
    // unknown versions unchanged so the host rejects them cleanly.
    if !(1..=3).contains(&raw.version) {
        // SAFETY: `desc` is non-null and forwarded unchanged to the host serializer.
        return unsafe { std::mem::transmute::<usize, SerializeVersioned>(address)(desc, blob, error) };
    }
    let cleaned = VersionedDesc {
        version: raw.version,
        desc: without_console_flags(raw.desc),
    };
    // SAFETY: the export has this signature; it takes what the title passed.
    unsafe { std::mem::transmute::<usize, SerializeVersioned>(address)(&cleaned, blob, error) }
}

type Deserialize =
    unsafe extern "system" fn(*const u8, usize, *const Iid, *mut *mut c_void) -> HRESULT;

/// `d3d12_xs!D3D12CreateRootSignatureDeserializer`: our blobs are the
/// host's serialization, so the host's deserializer reads them, and its
/// object (IUnknown, then the description getter) is laid out as the
/// console's. The title's upscaler asks with the PC interface id; pre-built
/// console DXBC `RTS0` blobs have their high-byte console flags cleared first.
pub extern "system" fn create_deserializer(
    blob: *const u8,
    size: usize,
    iid: *const Iid,
    out: *mut *mut c_void,
) -> HRESULT {
    if out.is_null() {
        return E_INVALIDARG;
    }
    clear_ptr(out);
    // The host dereferences the interface id without checking it, so a null
    // one is answered here instead of crashing there; an empty blob is
    // malformed and rejected the same way.
    if blob.is_null() || size == 0 || iid.is_null() {
        return E_INVALIDARG;
    }
    static FUNCTION: OnceLock<Option<usize>> = OnceLock::new();
    let Some(address) = host_export(&FUNCTION, c"D3D12CreateRootSignatureDeserializer") else {
        tracing::warn!("no host d3d12.dll to deserialize root signatures");
        return E_NOTIMPL;
    };
    let rewritten = (36..=0x1fff_ffff + 20)
        .contains(&size)
        // SAFETY: `blob` is non-null and `size` fits within DXBC container bounds.
        .then(|| unsafe { std::slice::from_raw_parts(blob, size) })
        .and_then(strip_console_flags_from_rts0);
    let (data, len) = match &rewritten {
        Some(buf) => (buf.as_ptr(), buf.len()),
        None => (blob, size),
    };
    // SAFETY: the export has this signature; `data` lives past the call.
    unsafe { std::mem::transmute::<usize, Deserialize>(address)(data, len, iid, out) }
}

#[repr(C)]
struct RootSignature {
    object: Object,
    /// The serialized form, kept for translating pipelines later.
    blob: Vec<u8>,
}

/// Device method.
pub(crate) extern "system" fn create(
    _: This,
    _nodes: u32,
    blob: *const u8,
    size: usize,
    iid: *const Iid,
    out: *mut This,
) -> HRESULT {
    if blob.is_null() || size == 0 {
        clear_const_ptr(out);
        return E_INVALIDARG;
    }
    // SAFETY: the title passes `size` bytes.
    let blob = unsafe { std::slice::from_raw_parts(blob, size) }.to_vec();
    tracing::debug!(size, "root signature");
    let root = Box::new(RootSignature {
        object: Object::new(&FUNCTIONS),
        blob,
    });
    object::hand_out(ROOT_SIGNATURE, root, iid, &ROOT_SIGNATURE_IIDS, out)
}

extern "system" fn query(this: This, iid: *const Iid, out: *mut This) -> HRESULT {
    let found = object::is(iid, &ROOT_SIGNATURE_IIDS).then_some(this);
    object::answer(ROOT_SIGNATURE, iid, found, out)
}
extern "system" fn release(this: This) -> u32 {
    object::release_boxed::<RootSignature>(this)
}

/// IUnknown 3, object 4, device child 1.
static FUNCTIONS: Table<24> = {
    let mut f = object::d3d12_object(object::missing_table::<ROOT_SIGNATURE, 24>());
    f[0] = query as *const ();
    f[2] = release as *const ();
    f[7] = object::parent_device as *const (); // 0x38
    Table(f)
};

#[cfg(test)]
mod tests {
    use super::*;
    use crate::object::{E_NOINTERFACE, E_POINTER, S_FALSE, S_OK, slot};

    /// An interface we do not implement.
    const OTHER_IID: Iid = guid(0x5a5a_5a5a, 0x5a5a, 0x5a5a, [0x5a; 8]);

    /// The shim hands the blob to the host unchanged; `create` keeps it, so
    /// any bytes serve.
    const BLOB: [u8; 4] = [0x01, 0x02, 0x03, 0x04];

    fn created(iid: &Iid) -> This {
        let mut out = std::ptr::null();
        let hr = create(
            std::ptr::null(),
            0,
            BLOB.as_ptr(),
            BLOB.len(),
            iid,
            &mut out,
        );
        assert_eq!(hr, S_OK);
        assert!(!out.is_null());
        out
    }

    /// The host's structures, field for field (64-bit layout).
    #[cfg(target_pointer_width = "64")]
    #[test]
    fn descriptions_keep_the_pc_layout() {
        assert_eq!(std::mem::size_of::<Desc>(), 40);
        assert_eq!(std::mem::offset_of!(Desc, parameter_count), 0);
        assert_eq!(std::mem::offset_of!(Desc, parameters), 8);
        assert_eq!(std::mem::offset_of!(Desc, sampler_count), 16);
        assert_eq!(std::mem::offset_of!(Desc, samplers), 24);
        assert_eq!(std::mem::offset_of!(Desc, flags), 32);
        assert_eq!(std::mem::size_of::<VersionedDesc>(), 48);
        assert_eq!(std::mem::offset_of!(VersionedDesc, version), 0);
        assert_eq!(std::mem::offset_of!(VersionedDesc, desc), 8);
    }

    #[test]
    fn console_flags_are_dropped_and_the_rest_kept() {
        assert_eq!(CONSOLE_FLAGS, 0xff00_0000);
        let desc = Desc {
            parameter_count: 2,
            parameters: BLOB.as_ptr().cast(),
            sampler_count: 1,
            samplers: BLOB.as_ptr().cast(),
            flags: 0xffff_ffff,
        };
        let plain = without_console_flags(desc);
        assert_eq!(plain.flags, 0x00ff_ffff);
        assert_eq!((plain.parameter_count, plain.sampler_count), (2, 1));
        assert!(std::ptr::eq(plain.parameters, desc.parameters));
        assert!(std::ptr::eq(plain.samplers, desc.samplers));

        let low = Desc {
            flags: 0x00ab_cdef,
            ..desc
        };
        assert_eq!(without_console_flags(low).flags, 0x00ab_cdef);
        let none = Desc { flags: 0, ..desc };
        assert_eq!(without_console_flags(none).flags, 0);
    }

    #[test]
    fn serializers_reject_a_null_description() {
        let mut blob = std::ptr::null_mut();
        let mut error = std::ptr::null_mut();
        assert_eq!(
            serialize(std::ptr::null(), 1, &mut blob, &mut error),
            E_INVALIDARG
        );
        assert_eq!(
            serialize_versioned(std::ptr::null(), &mut blob, &mut error),
            E_INVALIDARG
        );
        assert!(blob.is_null() && error.is_null());
    }

    /// A null `riid` used to be forwarded to the host, which dereferences it
    /// (an access violation in `d3d12.dll`); an empty blob is malformed.
    #[test]
    fn deserializer_creation_rejects_missing_arguments() {
        let iid = ROOT_SIGNATURE_IIDS[0];
        let mut out = std::ptr::null_mut();
        assert_eq!(
            create_deserializer(std::ptr::null(), BLOB.len(), &iid, &mut out),
            E_INVALIDARG
        );
        assert_eq!(
            create_deserializer(BLOB.as_ptr(), BLOB.len(), std::ptr::null(), &mut out),
            E_INVALIDARG
        );
        assert_eq!(
            create_deserializer(BLOB.as_ptr(), 0, &iid, &mut out),
            E_INVALIDARG
        );
        assert_eq!(
            create_deserializer(BLOB.as_ptr(), BLOB.len(), &iid, std::ptr::null_mut()),
            E_INVALIDARG
        );
        assert!(out.is_null());
    }

    #[test]
    fn creating_a_root_signature_checks_its_arguments() {
        let iid = ROOT_SIGNATURE_IIDS[0];
        let mut out = std::ptr::null();
        assert_eq!(
            create(
                std::ptr::null(),
                0,
                std::ptr::null(),
                BLOB.len(),
                &iid,
                &mut out
            ),
            E_INVALIDARG
        );
        assert_eq!(
            create(std::ptr::null(), 0, BLOB.as_ptr(), 0, &iid, &mut out),
            E_INVALIDARG
        );
        assert!(out.is_null());

        // An unknown interface is dropped, and the answer is null.
        assert_eq!(
            create(
                std::ptr::null(),
                0,
                BLOB.as_ptr(),
                BLOB.len(),
                &OTHER_IID,
                &mut out
            ),
            E_NOINTERFACE
        );
        assert!(out.is_null());
        assert_eq!(
            create(
                std::ptr::null(),
                0,
                BLOB.as_ptr(),
                BLOB.len(),
                std::ptr::null(),
                &mut out
            ),
            E_NOINTERFACE
        );
        assert!(out.is_null());

        // Nowhere to write means the title only asked whether creation works.
        assert_eq!(
            create(
                std::ptr::null(),
                0,
                BLOB.as_ptr(),
                BLOB.len(),
                &iid,
                std::ptr::null_mut()
            ),
            S_FALSE
        );
    }

    #[test]
    fn every_known_interface_creates_a_working_object() {
        let root = created(&ROOT_SIGNATURE_IIDS[0]);
        let query = slot::<extern "system" fn(This, *const Iid, *mut This) -> HRESULT>(root, 0);
        for iid in &ROOT_SIGNATURE_IIDS {
            let mut out = std::ptr::null();
            assert_eq!(query(root, iid, &mut out), S_OK);
            assert!(std::ptr::eq(out, root));
            assert_eq!(object::release(out), 1, "the original reference remains");
        }
        let mut out = std::ptr::null();
        assert_eq!(query(root, &OTHER_IID, &mut out), E_NOINTERFACE);
        assert!(out.is_null());
        assert_eq!(query(root, std::ptr::null(), &mut out), E_NOINTERFACE);
        assert_eq!(
            query(root, &ROOT_SIGNATURE_IIDS[0], std::ptr::null_mut()),
            E_POINTER
        );
        assert_eq!(object::release(root), 0);
    }

    #[test]
    fn the_table_has_the_object_and_device_child_slots() {
        let root = created(&ROOT_SIGNATURE_IIDS[1]);
        // SetPrivateDataInterface is the one object method left out.
        assert_eq!(
            slot::<extern "system" fn() -> HRESULT>(root, 0x28)(),
            E_NOTIMPL
        );
        assert_eq!(
            slot::<extern "system" fn(This, *const u16) -> HRESULT>(root, 0x30)(
                root,
                std::ptr::null()
            ),
            S_OK
        );
        // GetDevice: an unknown interface is answered, not dereferenced.
        let mut device = std::ptr::null();
        let get_device =
            slot::<extern "system" fn(This, *const Iid, *mut This) -> HRESULT>(root, 0x38);
        assert_eq!(
            get_device(root, std::ptr::null(), &mut device),
            E_NOINTERFACE
        );
        assert!(device.is_null());
        assert_eq!(object::release(root), 0);
    }

    /// One 32-bit constant at b0, serialized by the host, then created.
    #[cfg(windows)]
    #[test]
    fn host_serialized_blobs_create_root_signatures() {
        use windows::Win32::Graphics::Direct3D12::{
            D3D12_ROOT_CONSTANTS, D3D12_ROOT_PARAMETER, D3D12_ROOT_PARAMETER_0,
            D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS, D3D12_SHADER_VISIBILITY_ALL,
        };
        let parameter = D3D12_ROOT_PARAMETER {
            ParameterType: D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS,
            Anonymous: D3D12_ROOT_PARAMETER_0 {
                Constants: D3D12_ROOT_CONSTANTS {
                    ShaderRegister: 0,
                    RegisterSpace: 0,
                    Num32BitValues: 1,
                },
            },
            ShaderVisibility: D3D12_SHADER_VISIBILITY_ALL,
        };
        let desc = Desc {
            parameter_count: 1,
            parameters: (&raw const parameter).cast(),
            sampler_count: 0,
            samplers: std::ptr::null(),
            flags: 0x0100_0001,
        };
        let (mut blob, mut error) = (std::ptr::null_mut(), std::ptr::null_mut());
        assert_eq!(serialize(&desc, 1, &mut blob, &mut error), S_OK);
        let blob = blob.cast_const().cast::<Object>();
        let data = slot::<extern "system" fn(This) -> *const u8>(blob, 0x18)(blob);
        let size = slot::<extern "system" fn(This) -> usize>(blob, 0x20)(blob);
        assert!(size > 0);
        let null = std::ptr::null();
        let mut root = null;
        assert_eq!(
            create(null, 0, data, size, &ROOT_SIGNATURE_IIDS[0], &mut root),
            S_OK
        );
        assert_eq!(slot::<extern "system" fn(This) -> u32>(root, 0x10)(root), 0);
        slot::<extern "system" fn(This) -> u32>(blob, 0x10)(blob);
    }

    /// The host's deserializer reads our blobs back (the upscaler's call).
    #[cfg(windows)]
    #[test]
    fn our_blobs_deserialize_on_the_host() {
        use windows::Win32::Graphics::Direct3D12::ID3D12RootSignatureDeserializer;
        use windows::core::Interface;
        let desc = VersionedDesc {
            version: 1,
            desc: Desc {
                parameter_count: 0,
                parameters: std::ptr::null(),
                sampler_count: 0,
                samplers: std::ptr::null(),
                flags: 1,
            },
        };
        let (mut blob, mut error) = (std::ptr::null_mut(), std::ptr::null_mut());
        assert_eq!(serialize_versioned(&desc, &mut blob, &mut error), S_OK);
        let blob = blob.cast_const().cast::<Object>();
        let data = slot::<extern "system" fn(This) -> *const u8>(blob, 0x18)(blob);
        let size = slot::<extern "system" fn(This) -> usize>(blob, 0x20)(blob);
        let iid: Iid = ID3D12RootSignatureDeserializer::IID.to_u128().to_be_bytes();
        let iid = guid(
            u32::from_be_bytes(iid[0..4].try_into().unwrap()),
            u16::from_be_bytes(iid[4..6].try_into().unwrap()),
            u16::from_be_bytes(iid[6..8].try_into().unwrap()),
            iid[8..16].try_into().unwrap(),
        );
        let mut out = std::ptr::null_mut();
        assert_eq!(create_deserializer(data, size, &iid, &mut out), S_OK);
        // SAFETY: the host handed out this interface.
        let deserializer = unsafe { ID3D12RootSignatureDeserializer::from_raw(out) };
        // SAFETY: valid while the deserializer lives.
        let read = unsafe { &*deserializer.GetRootSignatureDesc() };
        assert_eq!((read.NumParameters, read.Flags.0), (0, 1));
        slot::<extern "system" fn(This) -> u32>(blob, 0x10)(blob);
    }

    /// Version 1.0 through the versioned entry point, with a console flag.
    #[cfg(windows)]
    #[test]
    fn versioned_descriptions_serialize_on_the_host() {
        let desc = VersionedDesc {
            version: 1,
            desc: Desc {
                parameter_count: 0,
                parameters: std::ptr::null(),
                sampler_count: 0,
                samplers: std::ptr::null(),
                flags: 0x0200_0001,
            },
        };
        let (mut blob, mut error) = (std::ptr::null_mut(), std::ptr::null_mut());
        assert_eq!(serialize_versioned(&desc, &mut blob, &mut error), S_OK);
        let blob = blob.cast_const().cast::<Object>();
        assert!(slot::<extern "system" fn(This) -> usize>(blob, 0x20)(blob) > 0);
        slot::<extern "system" fn(This) -> u32>(blob, 0x10)(blob);
        assert_eq!(
            serialize_versioned(std::ptr::null(), &mut blob.cast_mut().cast(), &mut error),
            E_INVALIDARG
        );
    }

    /// Truncated and nonsense blobs reach the host, which rejects them
    /// without writing an object.
    #[cfg(windows)]
    #[test]
    fn malformed_blobs_are_rejected_by_the_host() {
        use windows::Win32::Graphics::Direct3D12::ID3D12RootSignatureDeserializer;
        use windows::core::Interface;
        let desc = VersionedDesc {
            version: 1,
            desc: Desc {
                parameter_count: 0,
                parameters: std::ptr::null(),
                sampler_count: 0,
                samplers: std::ptr::null(),
                flags: 1,
            },
        };
        let (mut blob, mut error) = (std::ptr::null_mut(), std::ptr::null_mut());
        assert_eq!(serialize_versioned(&desc, &mut blob, &mut error), S_OK);
        let blob = blob.cast_const().cast::<Object>();
        let data = slot::<extern "system" fn(This) -> *const u8>(blob, 0x18)(blob);
        let size = slot::<extern "system" fn(This) -> usize>(blob, 0x20)(blob);
        let iid: Iid = ID3D12RootSignatureDeserializer::IID.to_u128().to_be_bytes();
        let iid = guid(
            u32::from_be_bytes(iid[0..4].try_into().unwrap()),
            u16::from_be_bytes(iid[4..6].try_into().unwrap()),
            u16::from_be_bytes(iid[6..8].try_into().unwrap()),
            iid[8..16].try_into().unwrap(),
        );
        let mut out = std::ptr::null_mut();
        assert_eq!(
            create_deserializer(data, size - 1, &iid, &mut out),
            E_INVALIDARG
        );
        assert!(out.is_null());
        let garbage = [0xffu8; 64];
        assert_eq!(
            create_deserializer(garbage.as_ptr(), garbage.len(), &iid, &mut out),
            E_INVALIDARG
        );
        assert!(out.is_null());
        slot::<extern "system" fn(This) -> u32>(blob, 0x10)(blob);
    }

    #[test]
    fn early_validation_failures_clear_non_null_outputs() {
        let iid = ROOT_SIGNATURE_IIDS[0];
        let sentinel: *mut c_void = std::ptr::without_provenance_mut(0xdead_beef);

        let (mut blob, mut error) = (sentinel, sentinel);
        assert_eq!(
            serialize(std::ptr::null(), 1, &mut blob, &mut error),
            E_INVALIDARG
        );
        assert!(blob.is_null() && error.is_null());

        let (mut blob, mut error) = (sentinel, sentinel);
        assert_eq!(
            serialize_versioned(std::ptr::null(), &mut blob, &mut error),
            E_INVALIDARG
        );
        assert!(blob.is_null() && error.is_null());

        for (ptr, len, riid) in [
            (std::ptr::null(), BLOB.len(), (&raw const iid)),
            (BLOB.as_ptr(), 0, (&raw const iid)),
            (BLOB.as_ptr(), BLOB.len(), std::ptr::null()),
        ] {
            let mut out = sentinel;
            assert_eq!(create_deserializer(ptr, len, riid, &mut out), E_INVALIDARG);
            assert!(out.is_null());
        }

        let mut root: This = sentinel.cast_const().cast();
        assert_eq!(
            create(std::ptr::null(), 0, std::ptr::null(), 4, &iid, &mut root),
            E_INVALIDARG
        );
        assert!(root.is_null());
    }

    /// Pre-serialized console DXBC `RTS0` blobs carry Xbox high-byte flags
    /// (`0xff00_0000`) and a DXBC container checksum; `create_deserializer`
    /// strips those flags and re-signs valid containers so the upscaler can
    /// re-serialize and create a host `ID3D12RootSignature` on hardware and WARP,
    /// while still failing closed on corrupted hashes or malformed chunk tables.
    #[cfg(windows)]
    #[test]
    fn console_flagged_rts0_blobs_deserialize_cleanly_and_bad_hashes_fail_closed() {
        use windows::Win32::Graphics::Direct3D::D3D_FEATURE_LEVEL_11_0;
        use windows::Win32::Graphics::Direct3D12::{
            D3D_ROOT_SIGNATURE_VERSION_1_0, D3D12_ROOT_CONSTANTS, D3D12_ROOT_PARAMETER,
            D3D12_ROOT_PARAMETER_0, D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS,
            D3D12_SHADER_VISIBILITY_ALL, D3D12CreateDevice, D3D12SerializeRootSignature,
            ID3D12Device, ID3D12RootSignature, ID3D12RootSignatureDeserializer,
        };
        use windows::Win32::Graphics::Dxgi::{CreateDXGIFactory1, IDXGIAdapter, IDXGIFactory4};
        use windows::core::Interface;

        fn test_dxbc_hash(blob: &[u8]) -> [u8; 16] {
            const K: [u32; 64] = [
                0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613,
                0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193,
                0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d,
                0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
                0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122,
                0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
                0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244,
                0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
                0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb,
                0xeb86d391,
            ];
            const R: [u32; 16] = [7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21];
            let step = |state: &mut [u32; 4], m: &[u32; 16]| {
                let [mut a, mut b, mut c, mut d] = *state;
                for i in 0..64 {
                    let (f, g) = match i {
                        0..16 => ((b & c) | (!b & d), i),
                        16..32 => ((d & b) | (!d & c), (5 * i + 1) % 16),
                        32..48 => (b ^ c ^ d, (3 * i + 5) % 16),
                        _ => (c ^ (b | !d), (7 * i) % 16),
                    };
                    let next_b = b.wrapping_add(
                        a.wrapping_add(f)
                            .wrapping_add(K[i])
                            .wrapping_add(m[g])
                            .rotate_left(R[(i / 16) * 4 + (i % 4)]),
                    );
                    (a, d, c, b) = (d, c, b, next_b);
                }
                state[0] = state[0].wrapping_add(a);
                state[1] = state[1].wrapping_add(b);
                state[2] = state[2].wrapping_add(c);
                state[3] = state[3].wrapping_add(d);
            };
            let words = |block: &[u8; 64]| -> [u32; 16] {
                std::array::from_fn(|i| u32::from_le_bytes(block[i * 4..i * 4 + 4].try_into().unwrap()))
            };
            let data = &blob[20..];
            let bits = (data.len() as u32) * 8;
            let mut state = [0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476];
            let full = (data.len() / 64) * 64;
            for chunk in data[..full].chunks_exact(64) {
                step(&mut state, &words(chunk.try_into().unwrap()));
            }
            let left = data.len() - full;
            let mut block = [0u8; 64];
            let mut m = if left >= 56 {
                block[..left].copy_from_slice(&data[full..]);
                block[left] = 0x80;
                step(&mut state, &words(&block));
                [0u32; 16]
            } else {
                block[4..4 + left].copy_from_slice(&data[full..]);
                block[4 + left] = 0x80;
                words(&block)
            };
            m[0] = bits;
            m[15] = (bits >> 2) | 1;
            step(&mut state, &m);
            let mut out = [0u8; 16];
            for (i, w) in state.into_iter().enumerate() {
                out[i * 4..i * 4 + 4].copy_from_slice(&w.to_le_bytes());
            }
            out
        }

        let iid_bytes = ID3D12RootSignatureDeserializer::IID.to_u128().to_be_bytes();
        let iid = guid(
            u32::from_be_bytes(iid_bytes[0..4].try_into().unwrap()),
            u16::from_be_bytes(iid_bytes[4..6].try_into().unwrap()),
            u16::from_be_bytes(iid_bytes[6..8].try_into().unwrap()),
            iid_bytes[8..16].try_into().unwrap(),
        );

        let params: [D3D12_ROOT_PARAMETER; 3] = std::array::from_fn(|reg| D3D12_ROOT_PARAMETER {
            ParameterType: D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS,
            Anonymous: D3D12_ROOT_PARAMETER_0 {
                Constants: D3D12_ROOT_CONSTANTS {
                    ShaderRegister: reg as u32,
                    RegisterSpace: 0,
                    Num32BitValues: 2,
                },
            },
            ShaderVisibility: D3D12_SHADER_VISIBILITY_ALL,
        });

        // Exercise both DXBC MD5 tail branches: 0 parameters (`data.len() % 64 < 56`)
        // and 3 parameters (`data.len() % 64 >= 56`).
        for count in [0u32, 3u32] {
            let desc = VersionedDesc {
                version: 1,
                desc: Desc {
                    parameter_count: count,
                    parameters: if count == 0 {
                        std::ptr::null()
                    } else {
                        params.as_ptr().cast()
                    },
                    sampler_count: 0,
                    samplers: std::ptr::null(),
                    flags: 1,
                },
            };
            let (mut raw_blob, mut error) = (std::ptr::null_mut(), std::ptr::null_mut());
            assert_eq!(serialize_versioned(&desc, &mut raw_blob, &mut error), S_OK);
            let blob_obj = raw_blob.cast_const().cast::<Object>();
            let data = slot::<extern "system" fn(This) -> *const u8>(blob_obj, 0x18)(blob_obj);
            let size = slot::<extern "system" fn(This) -> usize>(blob_obj, 0x20)(blob_obj);
            // SAFETY: serialized blob bytes are valid until released below.
            let mut xbox_blob = unsafe { std::slice::from_raw_parts(data, size) }.to_vec();
            slot::<extern "system" fn(This) -> u32>(blob_obj, 0x10)(blob_obj);

            // Sanity-check test helper matches d3d12.dll's own DXBC container hash.
            assert_eq!(&xbox_blob[4..20], &test_dxbc_hash(&xbox_blob));

            let rts0_off = u32::from_le_bytes(xbox_blob[32..36].try_into().unwrap()) as usize;
            assert_eq!(&xbox_blob[rts0_off..rts0_off + 4], b"RTS0");
            xbox_blob[rts0_off + 28..rts0_off + 32].copy_from_slice(&0x0300_0001u32.to_le_bytes());
            let signed = test_dxbc_hash(&xbox_blob);
            xbox_blob[4..20].copy_from_slice(&signed);

            // 1. Signed Xbox-flagged container deserializes with console flags stripped,
            //    and the deserialized description re-serializes and builds on host + WARP.
            let mut out = std::ptr::null_mut();
            assert_eq!(
                create_deserializer(xbox_blob.as_ptr(), xbox_blob.len(), &iid, &mut out),
                S_OK
            );
            // SAFETY: `create_deserializer` returned `S_OK` for `ID3D12RootSignatureDeserializer`.
            let deserializer = unsafe { ID3D12RootSignatureDeserializer::from_raw(out) };
            // SAFETY: valid for the lifetime of `deserializer`.
            let read = unsafe { &*deserializer.GetRootSignatureDesc() };
            assert_eq!((read.NumParameters, read.Flags.0), (count, 1));

            let mut reserialized = None;
            // SAFETY: `read` comes from the host deserializer and now has PC-legal flags.
            unsafe {
                D3D12SerializeRootSignature(
                    read,
                    D3D_ROOT_SIGNATURE_VERSION_1_0,
                    &mut reserialized,
                    None,
                )
            }
            .expect("PC D3D12SerializeRootSignature accepts the stripped description");
            let reserialized = reserialized.unwrap();
            // SAFETY: `reserialized` owns a valid DXBC root-signature buffer.
            let reserialized_bytes = unsafe {
                std::slice::from_raw_parts(
                    reserialized.GetBufferPointer().cast::<u8>(),
                    reserialized.GetBufferSize(),
                )
            };
            if let Some(host) = crate::host::host() {
                // SAFETY: valid root signature bytes on the host GPU device.
                let _rs: ID3D12RootSignature = unsafe {
                    host.device
                        .CreateRootSignature(0, reserialized_bytes)
                        .expect("host GPU accepts stripped root signature")
                };
            }
            // SAFETY: standard DXGI + WARP device creation for headless/software verification.
            unsafe {
                let factory: IDXGIFactory4 = CreateDXGIFactory1().unwrap();
                let warp_adapter: IDXGIAdapter = factory.EnumWarpAdapter().unwrap();
                let mut warp_device: Option<ID3D12Device> = None;
                D3D12CreateDevice(&warp_adapter, D3D_FEATURE_LEVEL_11_0, &mut warp_device)
                    .expect("WARP D3D12 device");
                let _rs: ID3D12RootSignature = warp_device
                    .unwrap()
                    .CreateRootSignature(0, reserialized_bytes)
                    .expect("WARP accepts stripped root signature");
            }

            // 2. Zero-hash Xbox-flagged container also strips console flags and keeps a zero hash.
            let mut zero_hash_blob = xbox_blob.clone();
            zero_hash_blob[4..20].fill(0);
            let mut out_zero = std::ptr::null_mut();
            assert_eq!(
                create_deserializer(
                    zero_hash_blob.as_ptr(),
                    zero_hash_blob.len(),
                    &iid,
                    &mut out_zero,
                ),
                S_OK
            );
            // SAFETY: `S_OK` above.
            let deserializer_zero = unsafe { ID3D12RootSignatureDeserializer::from_raw(out_zero) };
            // SAFETY: valid while `deserializer_zero` lives.
            assert_eq!(unsafe { (*deserializer_zero.GetRootSignatureDesc()).Flags.0 }, 1);

            // 3. Corrupted non-zero hash fails closed (forwarded unchanged and rejected by host).
            let mut corrupted = xbox_blob.clone();
            corrupted[4] ^= 0x01;
            let mut out_bad: *mut c_void = std::ptr::without_provenance_mut(0x1234);
            assert_ne!(
                create_deserializer(corrupted.as_ptr(), corrupted.len(), &iid, &mut out_bad),
                S_OK
            );
            assert!(out_bad.is_null());

            // 4. Malformed chunk tables or unknown RTS0 versions also fail closed.
            let mut bad_chunk = xbox_blob.clone();
            bad_chunk[32..36].copy_from_slice(&(xbox_blob.len() as u32).to_le_bytes());
            let bad_chunk_hash = test_dxbc_hash(&bad_chunk);
            bad_chunk[4..20].copy_from_slice(&bad_chunk_hash);
            let mut out_chunk: *mut c_void = std::ptr::without_provenance_mut(0x5678);
            assert_ne!(
                create_deserializer(bad_chunk.as_ptr(), bad_chunk.len(), &iid, &mut out_chunk),
                S_OK
            );
            assert!(out_chunk.is_null());

            let mut bad_rts_ver = xbox_blob.clone();
            bad_rts_ver[rts0_off + 8..rts0_off + 12].copy_from_slice(&99u32.to_le_bytes());
            let bad_ver_hash = test_dxbc_hash(&bad_rts_ver);
            bad_rts_ver[4..20].copy_from_slice(&bad_ver_hash);
            let mut out_ver: *mut c_void = std::ptr::without_provenance_mut(0x9abc);
            assert_ne!(
                create_deserializer(
                    bad_rts_ver.as_ptr(),
                    bad_rts_ver.len(),
                    &iid,
                    &mut out_ver,
                ),
                S_OK
            );
            assert!(out_ver.is_null());
        }

        // Unknown VersionedDesc.version is forwarded unchanged and rejected by the host.
        let unknown_ver = VersionedDesc {
            version: 99,
            desc: Desc {
                parameter_count: 0,
                parameters: std::ptr::null(),
                sampler_count: 0,
                samplers: std::ptr::null(),
                flags: 0x0200_0001,
            },
        };
        let (mut bad_blob, mut bad_err) = (std::ptr::null_mut(), std::ptr::null_mut());
        assert_eq!(
            serialize_versioned(&unknown_ver, &mut bad_blob, &mut bad_err),
            E_INVALIDARG
        );
        if !bad_err.is_null() {
            let err_obj = bad_err.cast_const().cast::<Object>();
            slot::<extern "system" fn(This) -> u32>(err_obj, 0x10)(err_obj);
        }
    }
}
