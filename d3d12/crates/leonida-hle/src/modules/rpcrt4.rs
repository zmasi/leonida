//! `rpcrt4.dll`: the GUID calls a title loads the RPC runtime for.
//!
//! The runtime itself is not passed through (its own `ws2_32` imports would
//! give a title a network path outside `modules::net`): the handful of calls
//! titles use are implemented here, and everything else stays refused as
//! before.
#![allow(non_snake_case)]
#![allow(clippy::not_unsafe_ptr_arg_deref)]

use std::ffi::{c_char, c_void};

use super::xgameruntime::Guid;

/// `RPC_S_OK`.
const OK: i32 = 0;
/// `RPC_S_INVALID_STRING_UUID`.
const INVALID_STRING_UUID: i32 = 1705;
/// `RPC_S_OUT_OF_MEMORY`.
const OUT_OF_MEMORY: i32 = 14;

/// A version 4 GUID: 16 random bytes, the version nibble and the variant
/// bits set.
extern "system" fn UuidCreate(uuid: *mut Guid) -> i32 {
    create(uuid, false)
}

/// As [`UuidCreate`]: sequential UUIDs carry a MAC address and a clock on
/// Windows, which a title must not learn about this machine.
extern "system" fn UuidCreateSequential(uuid: *mut Guid) -> i32 {
    create(uuid, true)
}

fn create(uuid: *mut Guid, _sequential: bool) -> i32 {
    if uuid.is_null() {
        return INVALID_STRING_UUID;
    }
    let mut bytes = [0u8; 16];
    if !random(&mut bytes) {
        return OUT_OF_MEMORY;
    }
    // The third group's high nibble is the version, the fourth the variant.
    bytes[7] = (bytes[7] & 0x0f) | 0x40;
    bytes[8] = (bytes[8] & 0x3f) | 0x80;
    // SAFETY: the caller passes a GUID to fill.
    unsafe { uuid.write_unaligned(from_bytes(bytes)) };
    OK
}

/// 16 bytes from the host's CSPRNG.
fn random(bytes: &mut [u8; 16]) -> bool {
    #[cfg(windows)]
    {
        use windows_sys::Win32::Security::Cryptography::{
            BCRYPT_USE_SYSTEM_PREFERRED_RNG, BCryptGenRandom,
        };
        // SAFETY: our own buffer, the system's preferred generator.
        let status = unsafe {
            BCryptGenRandom(
                std::ptr::null_mut(),
                bytes.as_mut_ptr(),
                bytes.len() as u32,
                BCRYPT_USE_SYSTEM_PREFERRED_RNG,
            )
        };
        status >= 0
    }
    #[cfg(not(windows))]
    {
        // A counter mixed with the time: enough for identifiers in tests.
        use std::sync::atomic::{AtomicU64, Ordering};
        static N: AtomicU64 = AtomicU64::new(0);
        let n = N.fetch_add(1, Ordering::Relaxed)
            ^ std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .map_or(0, |d| d.as_nanos() as u64);
        for (i, b) in bytes.iter_mut().enumerate() {
            *b = (n >> (8 * (i % 8))) as u8 ^ (i as u8).wrapping_mul(31);
        }
        true
    }
}

extern "system" fn UuidFromStringA(text: *const c_char, uuid: *mut Guid) -> i32 {
    if text.is_null() || uuid.is_null() {
        return INVALID_STRING_UUID;
    }
    // SAFETY: a NUL-terminated string from the caller.
    let bytes = unsafe { std::ffi::CStr::from_ptr(text.cast()) }.to_bytes();
    match parse(bytes.iter().copied()) {
        // SAFETY: the caller passes a GUID to fill; it may be misaligned,
        // so it is written unaligned.
        Some(g) => unsafe {
            uuid.write_unaligned(g);
            OK
        },
        None => INVALID_STRING_UUID,
    }
}

extern "system" fn UuidFromStringW(text: *const u16, uuid: *mut Guid) -> i32 {
    if text.is_null() || uuid.is_null() {
        return INVALID_STRING_UUID;
    }
    let mut units = Vec::new();
    // SAFETY: a NUL-terminated wide string from the caller.
    unsafe {
        let mut at = text;
        while *at != 0 {
            units.push(*at);
            at = at.add(1);
        }
    }
    match parse(units.into_iter().map(|u| u as u32)) {
        // SAFETY: the caller passes a GUID to fill; it may be misaligned,
        // so it is written unaligned.
        Some(g) => unsafe {
            uuid.write_unaligned(g);
            OK
        },
        None => INVALID_STRING_UUID,
    }
}

extern "system" fn UuidToStringA(uuid: *const Guid, out: *mut *mut c_char) -> i32 {
    if uuid.is_null() {
        return INVALID_STRING_UUID;
    }
    // SAFETY: the caller passes a GUID to read, as the import declares; it
    // may be misaligned, so it is read unaligned.
    let g = unsafe { uuid.read_unaligned() };
    if out.is_null() {
        return INVALID_STRING_UUID;
    }
    let text = format(&g);
    let mut bytes = Vec::with_capacity(37);
    bytes.extend_from_slice(&text);
    bytes.push(0);
    publish(out, allocate(&bytes).cast())
}

extern "system" fn UuidToStringW(uuid: *const Guid, out: *mut *mut u16) -> i32 {
    if uuid.is_null() {
        return INVALID_STRING_UUID;
    }
    // SAFETY: the caller passes a GUID to read, as the import declares; it
    // may be misaligned, so it is read unaligned.
    let g = unsafe { uuid.read_unaligned() };
    if out.is_null() {
        return INVALID_STRING_UUID;
    }
    let text = format(&g);
    let mut units: Vec<u16> = text.iter().map(|&b| u16::from(b)).collect();
    units.push(0);
    let bytes: Vec<u8> = units.iter().flat_map(|u| u.to_le_bytes()).collect();
    publish(out, allocate(&bytes).cast())
}

extern "system" fn RpcStringFreeA(string: *mut *mut c_char) -> i32 {
    free(string.cast());
    OK
}

extern "system" fn RpcStringFreeW(string: *mut *mut u16) -> i32 {
    free(string.cast());
    OK
}

/// `[data1, data2, data3, data4]` as the console lays a GUID out.
fn from_bytes(b: [u8; 16]) -> Guid {
    Guid {
        data1: u32::from_le_bytes([b[0], b[1], b[2], b[3]]),
        data2: u16::from_le_bytes([b[4], b[5]]),
        data3: u16::from_le_bytes([b[6], b[7]]),
        data4: [b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]],
    }
}

/// `xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx` in lower case.
fn format(g: &Guid) -> [u8; 36] {
    const HEX: &[u8; 16] = b"0123456789abcdef";
    let mut text = [b'-'; 36];
    let pairs = [
        (0, g.data1 as u64, 8),
        (9, g.data2 as u64, 4),
        (14, g.data3 as u64, 4),
        (19, g.data4[0] as u64, 2),
        (21, g.data4[1] as u64, 2),
        (24, g.data4[2] as u64, 2),
        (26, g.data4[3] as u64, 2),
        (28, g.data4[4] as u64, 2),
        (30, g.data4[5] as u64, 2),
        (32, g.data4[6] as u64, 2),
        (34, g.data4[7] as u64, 2),
    ];
    for (at, value, digits) in pairs {
        for i in 0..digits {
            let shift = 4 * (digits - 1 - i);
            text[at + i] = HEX[((value >> shift) & 0xf) as usize];
        }
    }
    text
}

/// A GUID from its text, with or without braces and the dashes in place.
///
/// Win32 takes the canonical form only: 32 hex digits with the dashes at
/// 8, 13, 18 and 23, and the braces both there or both not. Every other
/// text is `RPC_S_INVALID_STRING_UUID`.
fn parse(mut chars: impl Iterator<Item = impl Into<u32>>) -> Option<Guid> {
    /// The longest legal text: the canonical 36 characters and the braces.
    const MAX: usize = 38;
    /// Where the dashes of the canonical form stand.
    const DASHES: [usize; 4] = [8, 13, 18, 23];

    let mut text = [0u8; MAX];
    let mut n = 0;
    for c in &mut chars {
        let c = c.into();
        if n == MAX {
            // Longer than any legal form.
            return None;
        }
        if c > 0x7f {
            // The canonical form is ASCII.
            return None;
        }
        text[n] = c as u8;
        n += 1;
    }
    if n == 0 {
        return None;
    }
    let body = if text[0] == b'{' {
        // A brace without its match is not a GUID.
        if n < 2 || text[n - 1] != b'}' {
            return None;
        }
        &text[1..n - 1]
    } else {
        &text[..n]
    };
    if body.len() != 36 {
        return None;
    }
    let mut digits = [0u8; 32];
    let mut d = 0;
    for (at, &c) in body.iter().enumerate() {
        if DASHES.contains(&at) {
            if c != b'-' {
                return None;
            }
            continue;
        }
        let digit = match c {
            0x30..=0x39 => c - 0x30,
            0x61..=0x66 => c - 0x61 + 10,
            0x41..=0x46 => c - 0x41 + 10,
            _ => return None,
        };
        digits[d] = digit;
        d += 1;
    }
    Some(assemble(&digits))
}

fn assemble(digits: &[u8; 32]) -> Guid {
    let byte = |i: usize| (digits[2 * i] << 4) | digits[2 * i + 1];
    // The text is the number's big-endian form; the fields are words.
    Guid {
        data1: u32::from_be_bytes([byte(0), byte(1), byte(2), byte(3)]),
        data2: u16::from_be_bytes([byte(4), byte(5)]),
        data3: u16::from_be_bytes([byte(6), byte(7)]),
        data4: [
            byte(8),
            byte(9),
            byte(10),
            byte(11),
            byte(12),
            byte(13),
            byte(14),
            byte(15),
        ],
    }
}

/// An allocation only [`RpcStringFreeA`] and [`RpcStringFreeW`] give back.
///
/// The length is kept in a word before the bytes, so a title that hands the
/// pointer back through either free (A or W) releases the whole thing.
fn allocate(bytes: &[u8]) -> *mut u8 {
    let len = 8 + bytes.len();
    let layout = std::alloc::Layout::from_size_align(len, 8).expect("a small allocation");
    // SAFETY: a fresh allocation of `len` bytes.
    unsafe {
        let base = std::alloc::alloc(layout);
        if base.is_null() {
            return std::ptr::null_mut();
        }
        base.cast::<u64>().write_unaligned(bytes.len() as u64);
        std::ptr::copy_nonoverlapping(bytes.as_ptr(), base.add(8), bytes.len());
        base.add(8)
    }
}

/// The caller's pointer variable, with the string we made.
///
/// A failed allocation is `RPC_S_OUT_OF_MEMORY`, and the caller's pointer
/// is left as it was.
fn publish<T>(out: *mut *mut T, pointer: *mut T) -> i32 {
    if pointer.is_null() {
        return OUT_OF_MEMORY;
    }
    // SAFETY: `out` is the caller's pointer variable.
    unsafe { out.write(pointer) };
    OK
}

fn free(pointer: *mut *mut c_void) {
    if pointer.is_null() {
        return;
    }
    // SAFETY: the caller's pointer variable, holding what `allocate` made
    // (or null, which is a no-op).
    unsafe {
        let at = pointer.read();
        if at.is_null() {
            return;
        }
        let base = at.cast::<u8>().sub(8);
        let len = base.cast::<u64>().read_unaligned() as usize;
        let layout = std::alloc::Layout::from_size_align(8 + len, 8).expect("a small allocation");
        std::alloc::dealloc(base, layout);
        pointer.write(std::ptr::null_mut());
    }
}

pub(crate) fn module() -> crate::HleModule {
    crate::HleModule {
        name: "rpcrt4",
        dlls: &["rpcrt4.dll"],
        exports: vec![
            crate::hle_export!(UuidCreate),
            crate::hle_export!(UuidCreateSequential),
            crate::hle_export!(UuidFromStringA),
            crate::hle_export!(UuidFromStringW),
            crate::hle_export!(UuidToStringA),
            crate::hle_export!(UuidToStringW),
            crate::hle_export!(RpcStringFreeA),
            crate::hle_export!(RpcStringFreeW),
        ],
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_guid_formats_and_parses_back() {
        let g = Guid::new(
            0x1234_abcd,
            0x00ef,
            0x4a5b,
            [0x80, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde],
        );
        let text = format(&g);
        assert_eq!(
            std::str::from_utf8(&text).unwrap(),
            "1234abcd-00ef-4a5b-8012-3456789abcde"
        );
        assert_eq!(parse(text.iter().copied()), Some(g));
        // Upper case and braces are what `UuidFromString` also takes.
        let upper: Vec<u8> = std::iter::once(b'{')
            .chain(text.iter().map(|b| b.to_ascii_uppercase()))
            .chain(std::iter::once(b'}'))
            .collect();
        assert_eq!(parse(upper.iter().copied()), Some(g));
    }

    #[test]
    fn garbage_is_refused() {
        assert_eq!(parse(b"1234".iter().copied()), None);
        assert_eq!(
            parse(b"1234abcd-00ef-4a5b-8012-3456789abcde-".iter().copied()),
            None
        );
        assert_eq!(
            parse(b"1234abcd-00ef-4a5b-8012-3456789abcdeZ".iter().copied()),
            None
        );
    }

    #[test]
    fn a_created_guid_is_version_four() {
        for _ in 0..64 {
            let mut g = Guid::new(0, 0, 0, [0; 8]);
            assert_eq!(UuidCreate(&mut g), OK);
            assert_eq!((g.data3 >> 12) & 0xf, 4, "{g}");
            assert_eq!(g.data4[0] >> 6, 0b10, "{g}");
            assert_ne!((g.data1, g.data2, g.data3, g.data4), (0, 0, 0, [0; 8]));
        }
    }

    #[test]
    fn a_string_is_freed_once_through_either_call() {
        let g = Guid::new(1, 2, 3, [4, 5, 6, 7, 8, 9, 10, 11]);
        let mut text: *mut c_char = std::ptr::null_mut();
        assert_eq!(UuidToStringA(&g, &mut text), OK);
        assert_eq!(
            // SAFETY: the string we just made, NUL-terminated.
            unsafe { std::ffi::CStr::from_ptr(text) }.to_str().unwrap(),
            "00000001-0002-0003-0405-060708090a0b"
        );
        assert_eq!(RpcStringFreeA(&mut text), OK);
        assert!(text.is_null());

        let mut wide: *mut u16 = std::ptr::null_mut();
        assert_eq!(UuidToStringW(&g, &mut wide), OK);
        // SAFETY: the wide string we just made, NUL-terminated.
        let units = unsafe {
            let mut n = 0;
            while *wide.add(n) != 0 {
                n += 1;
            }
            std::slice::from_raw_parts(wide, n)
        };
        let text: String = units.iter().map(|&u| u as u8 as char).collect();
        assert_eq!(text, "00000001-0002-0003-0405-060708090a0b");
        assert_eq!(RpcStringFreeW(&mut wide), OK);
        assert!(wide.is_null());

        // Freeing nothing is fine, as the runtime's does.
        let mut none: *mut c_char = std::ptr::null_mut();
        assert_eq!(RpcStringFreeA(&mut none), OK);
        assert_eq!(RpcStringFreeA(std::ptr::null_mut()), OK);
    }

    #[test]
    fn uuid_from_string_writes_what_it_reads() {
        let text = c"1234abcd-00ef-4a5b-8012-3456789abcde";
        let mut g = Guid::new(0, 0, 0, [0; 8]);
        assert_eq!(UuidFromStringA(text.as_ptr().cast(), &mut g), OK);
        assert_eq!(
            g,
            Guid::new(
                0x1234_abcd,
                0x00ef,
                0x4a5b,
                [0x80, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde]
            )
        );
        // The wide call takes the same text.
        let wide: Vec<u16> = "1234abcd-00ef-4a5b-8012-3456789abcde\0"
            .chars()
            .map(|c| c as u16)
            .collect();
        let mut w = Guid::new(0, 0, 0, [0; 8]);
        assert_eq!(UuidFromStringW(wide.as_ptr(), &mut w), OK);
        assert_eq!(w, g);
        assert_eq!(
            UuidFromStringA(c"nope".as_ptr().cast(), &mut w),
            INVALID_STRING_UUID
        );
    }

    #[test]
    fn a_malformed_guid_is_refused() {
        // A brace without its match.
        assert_eq!(
            parse(b"{1234abcd-00ef-4a5b-8012-3456789abcde".iter().copied()),
            None
        );
        assert_eq!(
            parse(b"1234abcd-00ef-4a5b-8012-3456789abcde}".iter().copied()),
            None
        );
        // Text after the closing brace.
        assert_eq!(
            parse(b"{1234abcd-00ef-4a5b-8012-3456789abcde}x".iter().copied()),
            None
        );
        // A dash where the canonical form has none, and one where it has one.
        assert_eq!(
            parse(b"1234abc-d00ef-4a5b-8012-3456789abcde".iter().copied()),
            None
        );
        assert_eq!(
            parse(b"1234abcd00ef-4a5b-8012-3456789abcde".iter().copied()),
            None
        );
        // Nothing at all, and more text than any legal form is long.
        assert_eq!(parse(b"".iter().copied()), None);
        assert_eq!(
            parse(b"{1234abcd-00ef-4a5b-8012-3456789abcde}{}".iter().copied()),
            None
        );
    }

    #[test]
    fn a_text_outside_ascii_is_refused() {
        // The canonical form is ASCII: a wide string with a unit above it is
        // not a GUID, whatever it looks like.
        let mut units: Vec<u16> = "1234abcd-00ef-4a5b-8012-3456789abcde"
            .chars()
            .map(|c| c as u16)
            .collect();
        units[0] = 0x00e9;
        units.push(0);
        let mut g = Guid::new(0, 0, 0, [0; 8]);
        assert_eq!(UuidFromStringW(units.as_ptr(), &mut g), INVALID_STRING_UUID);
        assert_eq!(g, Guid::new(0, 0, 0, [0; 8]));
    }

    #[test]
    fn a_refused_text_leaves_the_guid_alone() {
        let mut g = Guid::new(9, 9, 9, [9; 8]);
        assert_eq!(
            UuidFromStringA(
                c"{1234abcd-00ef-4a5b-8012-3456789abcde".as_ptr().cast(),
                &mut g
            ),
            INVALID_STRING_UUID
        );
        assert_eq!(g, Guid::new(9, 9, 9, [9; 8]));
    }

    #[test]
    fn a_failed_allocation_leaves_the_callers_pointer_alone() {
        let mut buffer = [0u8; 4];
        let mut text: *mut c_char = buffer.as_mut_ptr().cast();
        assert_eq!(publish(&mut text, std::ptr::null_mut()), OUT_OF_MEMORY);
        assert_eq!(text as usize, buffer.as_ptr() as usize);
    }

    #[test]
    fn a_misaligned_guid_is_written_and_read() {
        // A GUID that does not start on its own alignment: it comes from the
        // title's own buffer, not from Rust.
        let mut storage = [0u64; 4];
        let at = unsafe { storage.as_mut_ptr().cast::<u8>().add(1) }.cast::<Guid>();
        let text = c"1234abcd-00ef-4a5b-8012-3456789abcde";
        assert_eq!(UuidFromStringA(text.as_ptr().cast(), at), OK);
        // SAFETY: the GUID we just wrote, inside the buffer.
        let g = unsafe { at.read_unaligned() };
        assert_eq!(
            g,
            Guid::new(
                0x1234_abcd,
                0x00ef,
                0x4a5b,
                [0x80, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde]
            )
        );
        // And the string comes back the same way.
        let mut out: *mut c_char = std::ptr::null_mut();
        assert_eq!(UuidToStringA(at, &mut out), OK);
        assert_eq!(
            // SAFETY: the string we just made, NUL-terminated.
            unsafe { std::ffi::CStr::from_ptr(out) }.to_str().unwrap(),
            "1234abcd-00ef-4a5b-8012-3456789abcde"
        );
        assert_eq!(RpcStringFreeA(&mut out), OK);
        assert!(out.is_null());
    }
}
