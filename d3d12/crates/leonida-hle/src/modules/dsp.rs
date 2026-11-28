//! The console's audio DSP (`chad.dll`): streams that convolve blocks of
//! float samples with an impulse response (reverb), or run other block
//! transforms, on a coprocessor.
//!
//! A title connects once with a memory block for the DSP, activates
//! streams, and submits one command per block (gains, an input and an
//! output buffer). Activation hands back a status block whose second word is
//! the ticket of the last completed command and whose fourth is its result;
//! the title checks it one block later and treats a late command as an
//! overrun. Commands complete when submitted here.
//!
//! The transforms themselves are not computed: the hardware takes impulse
//! responses already in its own partitioned frequency-domain layout, which
//! is not known. Outputs are silence, so reverb returns are dry.
// shortcut: no convolution or block transform; outputs are zeros. Needs the
// coprocessor's impulse response and spectrum layouts to do better.
#![allow(non_snake_case)]
#![allow(clippy::not_unsafe_ptr_arg_deref)]

use std::collections::HashMap;
use std::ffi::c_void;
use std::sync::atomic::{AtomicI32, AtomicU32, AtomicUsize, Ordering};
use std::sync::{Arc, OnceLock, Mutex, RwLock};

use super::xasync::E_INVALIDARG;
use super::xgameruntime::{HRESULT, S_OK};
use crate::{HleModule, hle_export};

const E_POINTER: HRESULT = 0x8000_4003_u32 as i32;
const E_OUTOFMEMORY: HRESULT = 0x8007_000E_u32 as i32;

const CONVOLUTION: u32 = 1;
/// Option bits that need a stereo stream, and one convolution refuses.
const STEREO_OPTIONS: u32 = 2 | 8;
const NOT_FOR_CONVOLUTION: u32 = 4;

pub struct Dsp {
    max_streams: u32,
    active: Mutex<u32>,
}

/// What activation describes.
#[repr(C, packed)]
#[derive(Clone, Copy)]
pub struct StreamDesc {
    kind: u32,
    options: u32,
    frames: u32,
    channels: u32,
    ir_floats: u32,
    ir: *const f32,
}

/// The status block a stream's owner polls across threads.
#[repr(C)]
pub struct Status {
    submitted: AtomicU32,
    completed: AtomicU32,
    reserved: AtomicU32,
    result: AtomicI32,
}

const _: () = assert!(size_of::<Status>() == 16);
const _: () = assert!(align_of::<Status>() == 4);

pub struct Stream {
    dsp: Arc<Dsp>,
    frames: u32,
    channels: u32,
    submit: Mutex<()>,
    status: Status,
}

/// One block of work.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Command {
    gains: [f32; 2],
    input: *const f32,
    output: *mut f32,
    reserved: u64,
}

fn next_handle() -> usize {
    static NEXT: AtomicUsize = AtomicUsize::new(1);
    loop {
        let id = NEXT.fetch_add(1, Ordering::Relaxed);
        if id != 0 {
            return id;
        }
    }
}

fn connections() -> &'static RwLock<HashMap<usize, Arc<Dsp>>> {
    static CONNECTIONS: OnceLock<RwLock<HashMap<usize, Arc<Dsp>>>> = OnceLock::new();
    CONNECTIONS.get_or_init(|| RwLock::new(HashMap::new()))
}

fn streams() -> &'static RwLock<HashMap<usize, Arc<Stream>>> {
    static STREAMS: OnceLock<RwLock<HashMap<usize, Arc<Stream>>>> = OnceLock::new();
    STREAMS.get_or_init(|| RwLock::new(HashMap::new()))
}

pub extern "system" fn XDspConnectWithMaximumStreamLimit(
    base: *mut c_void,
    base_length: u32,
    ir_seconds: u32,
    max_streams: u32,
    max_channels: u32,
    out: *mut *mut Dsp,
) -> HRESULT {
    if out.is_null() {
        return E_POINTER;
    }
    // SAFETY: checked non-null; `write_unaligned` tolerates unaligned caller slots.
    unsafe { out.write_unaligned(std::ptr::null_mut()) };
    // `runtime/shims/chad.cpp`: a measured title call site passes `base_length == 0`.
    if base.is_null()
        || base_length > 0x1F40_0000
        || ir_seconds > 0x80
        || !(1..=0x100).contains(&max_streams)
        || !(1..=2).contains(&max_channels)
    {
        return E_INVALIDARG;
    }
    tracing::info!(
        base_length,
        ir_seconds,
        max_streams,
        max_channels,
        "audio DSP connected"
    );
    let dsp = Arc::new(Dsp {
        max_streams,
        active: Mutex::new(0),
    });
    let handle = next_handle();
    connections()
        .write()
        .unwrap_or_else(|e| e.into_inner())
        .insert(handle, dsp);
    // SAFETY: checked non-null.
    unsafe { out.write_unaligned(std::ptr::without_provenance_mut(handle)) };
    S_OK
}

pub extern "system" fn XDspDisconnect(dsp: *mut Dsp) -> HRESULT {
    if dsp.is_null() {
        return E_INVALIDARG;
    }
    let removed = connections()
        .write()
        .unwrap_or_else(|e| e.into_inner())
        .remove(&(dsp as usize));
    if removed.is_none() {
        return E_INVALIDARG;
    }
    S_OK
}

fn valid(desc: &StreamDesc) -> bool {
    let (kind, options, frames, channels) = (desc.kind, desc.options, desc.frames, desc.channels);
    let shape = if kind == CONVOLUTION {
        (1..=2).contains(&channels)
            && matches!(frames, 0x200 | 0x400)
            && options & NOT_FOR_CONVOLUTION == 0
    } else {
        channels == 1 && matches!(frames, 0x200 | 0x400 | 0x800)
    };
    shape && (options & STEREO_OPTIONS == 0 || channels == 2)
}

pub extern "system" fn XDspActivate(
    dsp: *mut Dsp,
    desc: *const StreamDesc,
    status: *mut *const Status,
    stream: *mut *mut Stream,
) -> HRESULT {
    if status.is_null() || stream.is_null() {
        return E_POINTER;
    }
    // SAFETY: both output slots checked non-null above.
    unsafe {
        status.write_unaligned(std::ptr::null());
        stream.write_unaligned(std::ptr::null_mut());
    }
    if dsp.is_null() || desc.is_null() {
        return E_INVALIDARG;
    }
    let Some(conn) = connections()
        .read()
        .unwrap_or_else(|e| e.into_inner())
        .get(&(dsp as usize))
        .cloned()
    else {
        return E_INVALIDARG;
    };
    // SAFETY: checked non-null; `StreamDesc` is `#[repr(C, packed)]`.
    let desc = unsafe { desc.read_unaligned() };
    if !valid(&desc) {
        return E_INVALIDARG;
    }
    let (kind, options, frames, channels) = (desc.kind, desc.options, desc.frames, desc.channels);
    tracing::debug!(
        kind,
        options,
        frames,
        channels,
        "audio DSP stream activated"
    );
    {
        let mut active = conn.active.lock().unwrap_or_else(|e| e.into_inner());
        if *active >= conn.max_streams {
            return E_OUTOFMEMORY;
        }
        *active += 1;
    }
    let new = Arc::new(Stream {
        dsp: conn,
        frames,
        channels,
        submit: Mutex::new(()),
        status: Status {
            submitted: AtomicU32::new(0),
            completed: AtomicU32::new(0),
            reserved: AtomicU32::new(0),
            result: AtomicI32::new(S_OK),
        },
    });
    let status_ptr = &raw const new.status;
    let handle = next_handle();
    streams()
        .write()
        .unwrap_or_else(|e| e.into_inner())
        .insert(handle, new);
    // SAFETY: checked non-null.
    unsafe {
        status.write_unaligned(status_ptr);
        stream.write_unaligned(std::ptr::without_provenance_mut(handle));
    }
    S_OK
}

pub extern "system" fn XDspDeactivate(stream: *mut Stream) -> HRESULT {
    if stream.is_null() {
        return E_INVALIDARG;
    }
    let removed = streams()
        .write()
        .unwrap_or_else(|e| e.into_inner())
        .remove(&(stream as usize));
    let Some(removed) = removed else {
        return E_INVALIDARG;
    };
    let mut active = removed.dsp.active.lock().unwrap_or_else(|e| e.into_inner());
    *active = active.saturating_sub(1);
    S_OK
}

/// Completes the command at once: its output block is silence.
pub extern "system" fn XDspSubmitCommand(
    stream: *mut Stream,
    command: *const Command,
    ticket: *mut u32,
) -> HRESULT {
    if stream.is_null() || command.is_null() {
        return E_INVALIDARG;
    }
    let Some(active_stream) = streams()
        .read()
        .unwrap_or_else(|e| e.into_inner())
        .get(&(stream as usize))
        .cloned()
    else {
        return E_INVALIDARG;
    };
    // SAFETY: checked non-null; `read_unaligned` avoids 8-byte alignment UB.
    let command = unsafe { command.read_unaligned() };
    let samples = (active_stream.frames as usize) * (active_stream.channels as usize);
    let byte_len = samples * size_of::<f32>();
    if !command.output.is_null() && (command.output as usize).checked_add(byte_len).is_none() {
        return E_INVALIDARG;
    }
    let next = {
        let _guard = active_stream.submit.lock().unwrap_or_else(|e| e.into_inner());
        if !command.output.is_null() {
            // SAFETY: caller's output buffer holds one block (`byte_len` bytes);
            // `cast::<u8>()` avoids `f32` alignment preconditions.
            unsafe { std::ptr::write_bytes(command.output.cast::<u8>(), 0, byte_len) };
        }
        let next = active_stream
            .status
            .submitted
            .load(Ordering::Relaxed)
            .wrapping_add(1);
        active_stream.status.submitted.store(next, Ordering::Relaxed);
        active_stream.status.result.store(S_OK, Ordering::Relaxed);
        active_stream.status.completed.store(next, Ordering::Release);
        next
    };
    if !ticket.is_null() {
        // SAFETY: optional out parameter.
        unsafe { ticket.write_unaligned(next) };
    }
    S_OK
}

pub(crate) fn module() -> HleModule {
    HleModule {
        name: "dsp",
        dlls: &["chad.dll"],
        exports: vec![
            hle_export!(XDspConnectWithMaximumStreamLimit),
            hle_export!(XDspDisconnect),
            hle_export!(XDspActivate),
            hle_export!(XDspDeactivate),
            hle_export!(XDspSubmitCommand),
        ],
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn convolution_stream_as_the_title_drives_it() {
        let mut base = vec![0u8; 0x10_0000];
        let mut dsp = std::ptr::null_mut();
        assert_eq!(
            XDspConnectWithMaximumStreamLimit(
                base.as_mut_ptr().cast(),
                0x10_0000,
                24,
                0x80,
                1,
                &mut dsp
            ),
            S_OK
        );
        let ir = vec![0f32; 0x800];
        // Stereo convolution with option 2, as the title asks for.
        let desc = StreamDesc {
            kind: CONVOLUTION,
            options: 2,
            frames: 0x400,
            channels: 2,
            ir_floats: 0x800,
            ir: ir.as_ptr(),
        };
        let (mut status, mut stream) = (std::ptr::null(), std::ptr::null_mut());
        assert_eq!(XDspActivate(dsp, &desc, &mut status, &mut stream), S_OK);
        let mut output = vec![1f32; 0x800];
        let input = vec![0.5f32; 0x800];
        let command = Command {
            gains: [1.0, 1.0],
            input: input.as_ptr(),
            output: output.as_mut_ptr(),
            reserved: 0,
        };
        let mut ticket = 0;
        assert_eq!(XDspSubmitCommand(stream, &command, &mut ticket), S_OK);
        // SAFETY: the stream's status block.
        let (completed, result) = unsafe {
            (
                (*status).completed.load(Ordering::Acquire),
                (*status).result.load(Ordering::Acquire),
            )
        };
        assert_eq!((completed, result), (ticket, S_OK));
        assert!(output.iter().all(|&s| s == 0.0));
        assert_eq!(XDspDeactivate(stream), S_OK);

        // Mono streams refuse stereo options; transforms are mono only.
        let bad = StreamDesc {
            channels: 1,
            ..desc
        };
        assert_eq!(
            XDspActivate(dsp, &bad, &mut status, &mut stream),
            E_INVALIDARG
        );
        let transform = StreamDesc {
            kind: 2,
            options: 0,
            channels: 1,
            ir_floats: 0,
            ir: std::ptr::null(),
            ..desc
        };
        assert_eq!(
            XDspActivate(dsp, &transform, &mut status, &mut stream),
            S_OK
        );
        assert_eq!(XDspDeactivate(stream), S_OK);
        assert_eq!(XDspDisconnect(dsp), S_OK);
    }

    #[test]
    fn connect_validates_zero_limits_accepts_zero_base_length_and_clears_outputs() {
        let mut base = [0u8; 64];
        let sentinel = std::ptr::without_provenance_mut::<Dsp>(0x1234);

        assert_eq!(
            XDspConnectWithMaximumStreamLimit(base.as_mut_ptr().cast(), 0, 1, 1, 1, std::ptr::null_mut()),
            E_POINTER
        );

        for (ptr, len, ir, streams, channels) in [
            (std::ptr::null_mut(), 0x1_0000, 1, 1, 1),
            (base.as_mut_ptr().cast(), 0x1F40_0001, 1, 1, 1),
            (base.as_mut_ptr().cast(), 0x1_0000, 0x81, 1, 1),
            (base.as_mut_ptr().cast(), 0x1_0000, 1, 0, 1),
            (base.as_mut_ptr().cast(), 0x1_0000, 1, 0x101, 1),
            (base.as_mut_ptr().cast(), 0x1_0000, 1, 1, 0),
            (base.as_mut_ptr().cast(), 0x1_0000, 1, 1, 3),
        ] {
            let mut out = sentinel;
            assert_eq!(
                XDspConnectWithMaximumStreamLimit(ptr, len, ir, streams, channels, &mut out),
                E_INVALIDARG
            );
            assert!(out.is_null(), "failed connect must clear *out");
        }

        // `chad.cpp`: a measured title call site passes `base_length == 0`.
        let mut dsp = std::ptr::null_mut();
        assert_eq!(
            XDspConnectWithMaximumStreamLimit(base.as_mut_ptr().cast(), 0, 24, 2, 1, &mut dsp),
            S_OK
        );
        assert!(!dsp.is_null());

        let mut status: *const Status = std::ptr::without_provenance(0x5678);
        let mut stream: *mut Stream = std::ptr::without_provenance_mut(0x9abc);
        assert_eq!(
            XDspActivate(dsp, std::ptr::null(), &mut status, &mut stream),
            E_INVALIDARG
        );
        assert!(status.is_null() && stream.is_null(), "failed activate must clear outputs");
        assert_eq!(XDspDisconnect(dsp), S_OK);
    }

    #[test]
    fn submit_handles_unaligned_command_output_and_ticket_and_rejects_wrapping_span() {
        let mut base = vec![0u8; 0x1_0000];
        let mut dsp = std::ptr::null_mut();
        assert_eq!(
            XDspConnectWithMaximumStreamLimit(base.as_mut_ptr().cast(), 0x1_0000, 24, 4, 2, &mut dsp),
            S_OK
        );
        let desc = StreamDesc {
            kind: CONVOLUTION,
            options: 2,
            frames: 0x200,
            channels: 2,
            ir_floats: 0,
            ir: std::ptr::null(),
        };
        let (mut status, mut stream) = (std::ptr::null(), std::ptr::null_mut());
        assert_eq!(XDspActivate(dsp, &desc, &mut status, &mut stream), S_OK);

        let byte_len = 0x200 * 2 * size_of::<f32>();
        let mut raw_out = vec![0xAAu8; byte_len + 8];
        // SAFETY: `raw_out` has `byte_len + 8` bytes; offset +1 is 1-byte-misaligned.
        let unaligned_output = unsafe { raw_out.as_mut_ptr().add(1).cast::<f32>() };
        let cmd_val = Command {
            gains: [1.0, 1.0],
            input: std::ptr::null(),
            output: unaligned_output,
            reserved: 0,
        };
        let mut raw_cmd = vec![0u8; size_of::<Command>() + 8];
        let mut raw_ticket = [0xFFu8; 8];
        // SAFETY: copying `Command` to offset +1 creates an unaligned `*const Command` and `*mut u32`.
        let (cmd_ptr, ticket_ptr) = unsafe {
            let cp = raw_cmd.as_mut_ptr().add(1).cast::<Command>();
            cp.write_unaligned(cmd_val);
            let tp = raw_ticket.as_mut_ptr().add(1).cast::<u32>();
            tp.write_unaligned(0);
            (cp.cast_const(), tp)
        };

        assert_eq!(XDspSubmitCommand(stream, cmd_ptr, ticket_ptr), S_OK);
        // SAFETY: `ticket_ptr` and `status` are valid.
        let (ticket, completed, hr) = unsafe {
            (
                ticket_ptr.read_unaligned(),
                status.cast::<u32>().add(1).read(),
                status.cast::<i32>().add(3).read(),
            )
        };
        assert_eq!((ticket, completed, hr), (1, 1, S_OK));
        assert_eq!(raw_out[0], 0xAA);
        assert!(raw_out[1..1 + byte_len].iter().all(|&b| b == 0));
        assert_eq!(raw_out[1 + byte_len], 0xAA);

        // Wrapping output pointer fails without advancing the stream ticket.
        let bad_cmd = Command {
            gains: [0.0, 0.0],
            input: std::ptr::null(),
            output: std::ptr::without_provenance_mut::<f32>(usize::MAX - 8),
            reserved: 0,
        };
        let mut unchanged = 99u32;
        assert_eq!(XDspSubmitCommand(stream, &bad_cmd, &mut unchanged), E_INVALIDARG);
        // SAFETY: `status` remains valid while the stream is active.
        assert_eq!(unsafe { status.cast::<u32>().add(1).read() }, 1);

        assert_eq!(XDspDeactivate(stream), S_OK);
        assert_eq!(XDspDisconnect(dsp), S_OK);
    }

    #[test]
    fn disconnect_and_deactivate_track_live_handles_without_uaf_or_double_free() {
        let mut base = vec![0u8; 0x1_0000];
        let mut dsp = std::ptr::null_mut();
        assert_eq!(
            XDspConnectWithMaximumStreamLimit(base.as_mut_ptr().cast(), 0x1_0000, 16, 1, 2, &mut dsp),
            S_OK
        );
        let desc = StreamDesc {
            kind: CONVOLUTION,
            options: 0,
            frames: 0x200,
            channels: 1,
            ir_floats: 0,
            ir: std::ptr::null(),
        };
        let (mut status, mut stream) = (std::ptr::null(), std::ptr::null_mut());
        assert_eq!(XDspActivate(dsp, &desc, &mut status, &mut stream), S_OK);

        let (mut extra_status, mut extra_stream) = (std::ptr::null(), std::ptr::null_mut());
        assert_eq!(
            XDspActivate(dsp, &desc, &mut extra_status, &mut extra_stream),
            E_OUTOFMEMORY
        );

        // Concurrent submissions and status reads on one active stream.
        let stream_addr = stream as usize;
        let status_addr = status as usize;
        std::thread::scope(|s| {
            for _ in 0..4 {
                s.spawn(move || {
                    let mut out = vec![1.0f32; 0x200];
                    let cmd = Command {
                        gains: [1.0, 1.0],
                        input: std::ptr::null(),
                        output: out.as_mut_ptr(),
                        reserved: 0,
                    };
                    for _ in 0..32 {
                        let mut ticket = 0;
                        assert_eq!(
                            XDspSubmitCommand(stream_addr as *mut Stream, &cmd, &mut ticket),
                            S_OK
                        );
                        // SAFETY: `status` stays alive for the life of `stream`.
                        let hr = unsafe { (status_addr as *const i32).add(3).read_volatile() };
                        assert_eq!(hr, S_OK);
                    }
                });
            }
        });
        // SAFETY: all 128 submissions completed before joining.
        assert_eq!(unsafe { status.cast::<u32>().add(1).read() }, 128);

        // Disconnecting before deactivating keeps `Dsp` backing alive for `stream`,
        // while refusing duplicate disconnects and new activations on `dsp`.
        assert_eq!(XDspDisconnect(dsp), S_OK);
        assert_eq!(XDspDisconnect(dsp), E_INVALIDARG);
        assert_eq!(
            XDspActivate(dsp, &desc, &mut extra_status, &mut extra_stream),
            E_INVALIDARG
        );

        assert_eq!(XDspDeactivate(stream), S_OK);
        assert_eq!(XDspDeactivate(stream), E_INVALIDARG);

        let noop_cmd = Command {
            gains: [0.0, 0.0],
            input: std::ptr::null(),
            output: std::ptr::null_mut(),
            reserved: 0,
        };
        assert_eq!(XDspSubmitCommand(stream, &noop_cmd, std::ptr::null_mut()), E_INVALIDARG);
        let bogus_dsp = std::ptr::without_provenance_mut::<Dsp>(0xDEAD);
        let bogus_stream = std::ptr::without_provenance_mut::<Stream>(0xBEEF);
        assert_eq!(XDspDisconnect(bogus_dsp), E_INVALIDARG);
        assert_eq!(XDspDeactivate(bogus_stream), E_INVALIDARG);
        assert_eq!(XDspSubmitCommand(bogus_stream, &noop_cmd, std::ptr::null_mut()), E_INVALIDARG);
    }
}
