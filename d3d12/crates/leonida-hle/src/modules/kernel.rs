//! Overrides of shared Windows APIs whose console meaning differs.
//!
//! Only the differences live here; everything else passes through to the
//! host (see `HostPolicy`).
#![allow(non_snake_case)]
// Title code calls these through its import table, passing pointers the real
// API would trust.
#![allow(clippy::not_unsafe_ptr_arg_deref)]

use std::ffi::{CStr, c_char, c_void};

use crate::{HleModule, HostPolicy, hle_export, registry, runtime};

/// `GetModuleHandleW(NULL)` must return the title's image, not our exe;
/// title images and our console modules are found by name.
pub extern "system" fn GetModuleHandleW(name: *const u16) -> *mut c_void {
    if name.is_null() {
        return runtime::main_image().unwrap_or(0) as *mut c_void;
    }
    find_module(&wide(name), false).unwrap_or_else(|| host::get_module_handle_w(name))
}

/// See [`GetModuleHandleW`].
pub extern "system" fn GetModuleHandleA(name: *const u8) -> *mut c_void {
    if name.is_null() {
        return runtime::main_image().unwrap_or(0) as *mut c_void;
    }
    find_module(&narrow(name), false).unwrap_or_else(|| host::get_module_handle_a(name))
}

/// A NUL-terminated string from the title.
fn narrow(text: *const u8) -> String {
    // SAFETY: the title passes a NUL-terminated string.
    unsafe { CStr::from_ptr(text.cast()) }
        .to_string_lossy()
        .into_owned()
}

/// See [`narrow`].
fn wide(text: *const u16) -> String {
    // SAFETY: the title passes a NUL-terminated string.
    unsafe {
        let len = (0..).take_while(|&i| *text.add(i) != 0).count();
        String::from_utf16_lossy(std::slice::from_raw_parts(text, len))
    }
}

/// A title image's path is its original file, not our cache copy;
/// `NULL` means the title's exe.
pub extern "system" fn GetModuleFileNameW(module: *mut c_void, out: *mut u16, size: u32) -> u32 {
    let Some(path) = title_path(module) else {
        return host::get_module_file_name_w(module, out, size);
    };
    let wide: Vec<u16> = path.encode_utf16().collect();
    // SAFETY: `out` is either null or writable for `size` `u16` elements.
    unsafe { write_module_path(&wide, out, size) }
}

/// See [`GetModuleFileNameW`].
pub extern "system" fn GetModuleFileNameA(module: *mut c_void, out: *mut u8, size: u32) -> u32 {
    let Some(path) = title_path(module) else {
        return host::get_module_file_name_a(module, out, size);
    };
    // SAFETY: `out` is either null or writable for `size` `u8` elements.
    unsafe { write_module_path(path.as_bytes(), out, size) }
}

fn title_path(module: *mut c_void) -> Option<String> {
    let base = if module.is_null() {
        runtime::main_image()?
    } else {
        module as usize
    };
    Some(runtime::image_path(base)?.to_string_lossy().into_owned())
}

/// `GetModuleFileName`'s contract: write directly with `write_unaligned` (never
/// forming `&mut [T]` over uninitialized or misaligned caller memory) plus a
/// trailing NUL; if it does not fit, truncate to `size`, report
/// `ERROR_INSUFFICIENT_BUFFER` and return `size`; otherwise clear last error to
/// `ERROR_SUCCESS` (`0`) and return the copied length.
unsafe fn write_module_path<T: Copy + Default>(text: &[T], out: *mut T, size: u32) -> u32 {
    let cap = size as usize;
    let Some(room) = cap.checked_sub(1).filter(|_| !out.is_null()) else {
        host::set_last_error(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    };
    let n = text.len().min(room);
    for (i, &unit) in text[..n].iter().enumerate() {
        // SAFETY: `i < n < cap` and `out` is non-null with room for `cap` elements.
        unsafe { out.add(i).write_unaligned(unit) };
    }
    // SAFETY: `n <= room < cap`.
    unsafe { out.add(n).write_unaligned(T::default()) };
    if n < text.len() {
        host::set_last_error(ERROR_INSUFFICIENT_BUFFER);
        size
    } else {
        host::set_last_error(0);
        n as u32
    }
}

const ERROR_INSUFFICIENT_BUFFER: u32 = 122;
const ERROR_PROC_NOT_FOUND: u32 = 127;

/// Console-only DLLs we implement load as a pseudo handle (titles load
/// `xgameruntime.dll` this way), the title's DLLs as the images we map
/// (on first use if nothing imports them); anything else loads on the host.
pub extern "system" fn LoadLibraryExA(
    name: *const u8,
    file: *mut c_void,
    flags: u32,
) -> *mut c_void {
    if !name.is_null()
        && let Some(handle) = find_module(&narrow(name), runs_code(flags))
    {
        return handle;
    }
    host::load_library_ex_a(name, file, flags)
}

/// See [`LoadLibraryExA`].
pub extern "system" fn LoadLibraryExW(
    name: *const u16,
    file: *mut c_void,
    flags: u32,
) -> *mut c_void {
    if !name.is_null()
        && let Some(handle) = find_module(&wide(name), runs_code(flags))
    {
        return handle;
    }
    host::load_library_ex_w(name, file, flags)
}

/// False for loads as data or resources (`DONT_RESOLVE_DLL_REFERENCES`,
/// `LOAD_LIBRARY_AS_DATAFILE(_EXCLUSIVE)`, `AS_IMAGE_RESOURCE`): those run
/// no code, so a title DLL we have not mapped can go to the host.
fn runs_code(flags: u32) -> bool {
    flags & (0x1 | 0x2 | 0x20 | 0x40) == 0
}

pub extern "system" fn LoadLibraryA(name: *const u8) -> *mut c_void {
    LoadLibraryExA(name, std::ptr::null_mut(), 0)
}

pub extern "system" fn LoadLibraryW(name: *const u16) -> *mut c_void {
    LoadLibraryExW(name, std::ptr::null_mut(), 0)
}

/// Looks up our implementations behind a pseudo handle; real modules go to
/// the host.
pub extern "system" fn GetProcAddress(module: *mut c_void, name: *const u8) -> *mut c_void {
    let saved = host::get_last_error();
    // `NULL` is the process's exe, which for the title is its own image.
    let module = if module.is_null() {
        runtime::main_image().unwrap_or(0) as *mut c_void
    } else {
        module
    };
    let Some(m) = registry().iter().find(|m| handle(m) == module) else {
        let found = host_override(module, name);
        host::set_last_error(saved);
        return found.unwrap_or_else(|| host::get_proc_address(module, name));
    };
    if name.is_null() {
        host::set_last_error(ERROR_PROC_NOT_FOUND);
        return std::ptr::null_mut();
    }
    // Values below 0x10000 are ordinals (`#n` in `m.exports`, e.g. `gameinput.dll` `#1`).
    let symbol = if (name as usize) < 0x1_0000 {
        std::borrow::Cow::Owned(format!("#{}", name as usize))
    } else {
        // SAFETY: not an ordinal, so the title passes a NUL-terminated name.
        unsafe { CStr::from_ptr(name.cast()) }.to_string_lossy()
    };
    match m.exports.iter().find(|e| e.name == symbol) {
        Some(e) => {
            host::set_last_error(saved);
            e.address as *mut c_void
        }
        None => {
            tracing::warn!(module = m.name, name = %symbol, "GetProcAddress: not implemented");
            host::set_last_error(ERROR_PROC_NOT_FOUND);
            std::ptr::null_mut()
        }
    }
}

/// What a host module's `name` (or ordinal) is bound to at import binding
/// when we override it: a title that looks a function up by name must not
/// get past our overrides (the network guard, path rewriting).
fn host_override(module: *mut c_void, name: *const u8) -> Option<*mut c_void> {
    if module.is_null() || name.is_null() || runtime::image_path(module as usize).is_some() {
        return None;
    }
    let symbol = if (name as usize) < 0x1_0000 {
        format!("#{}", name as usize)
    } else {
        narrow(name)
    };
    let path = read_module_path(|buf, cap| host::get_module_file_name_w(module, buf, cap))?;
    let file = path.rsplit(['\\', '/']).next()?;
    crate::registry::lookup(file, &symbol).map(|address| address as *mut c_void)
}

fn read_module_path(mut get: impl FnMut(*mut u16, u32) -> u32) -> Option<String> {
    let mut buf = vec![0u16; 260];
    loop {
        let len = get(buf.as_mut_ptr(), buf.len() as u32) as usize;
        if len == 0 {
            return None;
        }
        if len < buf.len() {
            return Some(String::from_utf16_lossy(&buf[..len]));
        }
        if buf.len() >= 32_768 {
            return None;
        }
        buf.resize((buf.len() * 2).min(32_768), 0);
    }
}

/// Our pseudo modules and the title's images stay loaded: `LoadLibrary`
/// took no reference on them, so the host's count must not drop.
pub extern "system" fn FreeLibrary(module: *mut c_void) -> i32 {
    if registry().iter().any(|m| handle(m) == module)
        || runtime::image_path(module as usize).is_some()
    {
        return 1;
    }
    host::free_library(module)
}

/// Title images hear of thread starts and ends from us, not from the
/// Windows loader (`leonida_loader::disable_thread_calls`).
pub extern "system" fn DisableThreadLibraryCalls(module: *mut c_void) -> i32 {
    if registry().iter().any(|m| handle(m) == module) {
        return 1;
    }
    host::disable_thread_library_calls(module)
}

/// Title debug output goes to our log (no debugger needs to be attached).
pub extern "system" fn OutputDebugStringA(text: *const u8) {
    if !text.is_null() {
        let text = narrow(text);
        host::note_title_output(text.trim());
        tracing::info!(target: "title", "{}", text.trim_end());
    }
}

/// See [`OutputDebugStringA`].
pub extern "system" fn OutputDebugStringW(text: *const u16) {
    if !text.is_null() {
        let text = wide(text);
        host::note_title_output(text.trim());
        tracing::info!(target: "title", "{}", text.trim_end());
    }
}

/// The CRT's `exit`, through which the title's entry point returns: logs
/// the code and the caller, then exits on the host.
pub extern "C" fn exit(code: i32) -> ! {
    tracing::warn!(code, from = %host::callers(), "title exits");
    host::exit(code)
}

/// The CRT's `abort` (also reached from `terminate`): logged like [`exit`].
pub extern "C" fn abort() -> ! {
    tracing::error!(from = %host::callers(), "title aborts");
    host::abort()
}

/// The CRT's `terminate` and the STL's invalid-argument exit: logged, then
/// the host's `abort`.
pub extern "C" fn terminate() -> ! {
    tracing::error!(from = %host::callers(), "title calls terminate");
    host::abort()
}

/// See [`terminate`].
pub extern "C" fn _invalid_parameter_noinfo_noreturn() -> ! {
    tracing::error!(from = %host::callers(), "title passed an invalid parameter");
    host::abort()
}

/// The title's unhandled-exception filter. Ours stands in front of it and
/// reports the crash first; the title still gets its own back.
static TITLE_FILTER: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);

pub extern "system" fn SetUnhandledExceptionFilter(filter: usize) -> usize {
    host::set_unhandled_exception_filter(report_unhandled);
    TITLE_FILTER.swap(filter, std::sync::atomic::Ordering::AcqRel)
}

extern "system" fn report_unhandled(info: *const c_void) -> i32 {
    host::report_unhandled(info);
    match TITLE_FILTER.load(std::sync::atomic::Ordering::Acquire) {
        0 => 0, // EXCEPTION_CONTINUE_SEARCH
        filter => {
            type Filter = extern "system" fn(*const c_void) -> i32;
            // SAFETY: the title registered this filter.
            let filter: Filter = unsafe { std::mem::transmute(filter) };
            filter(info)
        }
    }
}

/// See [`exit`]; only the current process is logged.
pub extern "system" fn TerminateProcess(process: *mut c_void, code: u32) -> i32 {
    if process as isize == -1 {
        tracing::warn!(code, from = %host::callers(), "title terminates the process");
    }
    host::terminate_process(process, code)
}

/// The pseudo handle of one of our modules: its address, never a real image.
fn handle(m: &HleModule) -> *mut c_void {
    m as *const HleModule as *mut c_void
}

fn module_file_name(path: &str) -> Option<String> {
    let file = path.rsplit(['\\', '/']).next().unwrap_or(path);
    if file.is_empty() {
        return None;
    }
    // As the Windows loader does, a name without '.' gets `.dll`; when '.' is
    // present no extension is added and trailing dots are stripped.
    if file.contains('.') {
        let trimmed = file.trim_end_matches('.');
        (!trimmed.is_empty()).then(|| trimmed.to_owned())
    } else {
        Some(format!("{file}.dll"))
    }
}

/// The title's images and our console modules by name; `None` leaves the
/// name to the host, which only DLLs `HostPolicy` passes through get (as at
/// import binding); others fail. With `load`, a title DLL not mapped yet is
/// mapped now.
fn find_module(path: &str, load: bool) -> Option<*mut c_void> {
    let file = module_file_name(path)?;
    // Loads go through the title DLL loader first: it waits for a DLL that
    // another thread is still initialising, as the Windows loader lock does.
    if load {
        match runtime::load_title_dll(&file) {
            Ok(Some(base)) => return Some(base as *mut c_void),
            Ok(None) => {}
            // Not the host: it would load the unpatched DLL from the title's folder.
            Err(error) => {
                tracing::error!(dll = file, %error, "mapping a title DLL failed");
                host::set_last_error(ERROR_MOD_NOT_FOUND);
                return Some(std::ptr::null_mut());
            }
        }
    }
    if let Some(base) = runtime::image_named(&file) {
        return Some(base as *mut c_void);
    }
    // DLLs the host has too are real host modules, whatever `--strict` says.
    let host_dll = HostPolicy::default().allows(&file);
    if !host_dll && let Some(m) = registry().iter().find(|m| m.serves(&file)) {
        if load {
            tracing::info!(dll = file, "loaded as a pseudo module");
        }
        return Some(handle(m));
    }
    if host_dll {
        return None;
    }
    // Any other host DLL (wininet, urlmon, ...) is beyond what we guard, and
    // the console does not have it.
    refuse(&file);
    host::set_last_error(ERROR_MOD_NOT_FOUND);
    Some(std::ptr::null_mut())
}

/// Logs a DLL name the title is refused, once.
fn refuse(file: &str) {
    static REFUSED: std::sync::Mutex<Vec<String>> = std::sync::Mutex::new(Vec::new());
    let file = file.to_ascii_lowercase();
    let mut refused = REFUSED.lock().unwrap_or_else(|e| e.into_inner());
    if !refused.contains(&file) {
        tracing::warn!(dll = file, "a host DLL outside the host policy: refused");
        refused.push(file);
    }
}

const ERROR_MOD_NOT_FOUND: u32 = 126;

/// The title's argument list: the program, then what was given after `--`.
/// The host's own would be ours (`leonida run <exe> ...`), and the title
/// looks at fixed places in it (`-skipToIIS` only as the first argument).
struct TitleArgv {
    count: std::cell::UnsafeCell<i32>,
    list: std::cell::UnsafeCell<*mut *mut c_char>,
}

// SAFETY: never written here after it was made; the title's C runtime start
// reads it once, on one thread.
unsafe impl Send for TitleArgv {}
// SAFETY: as above.
unsafe impl Sync for TitleArgv {}

/// Kept for the whole run: the title holds on to the pointers.
fn title_argv(program: &str, args: &[String]) -> TitleArgv {
    let list: Vec<*mut c_char> = std::iter::once(program)
        .chain(args.iter().map(String::as_str))
        .filter_map(|a| std::ffi::CString::new(a).ok())
        .map(std::ffi::CString::into_raw)
        .collect();
    let count = list.len() as i32;
    // `argv[argc]` is null.
    let list: Box<[*mut c_char]> = list.into_iter().chain(Some(std::ptr::null_mut())).collect();
    TitleArgv {
        count: count.into(),
        list: Box::leak(list).as_mut_ptr().into(),
    }
}

fn argv() -> &'static TitleArgv {
    static ARGV: std::sync::OnceLock<TitleArgv> = std::sync::OnceLock::new();
    ARGV.get_or_init(|| {
        // The program's name stays what the host says (as it was before).
        let program = std::env::args_os().next().unwrap_or_default();
        let args = runtime::title_args();
        tracing::info!(?args, "the title's arguments");
        title_argv(&program.to_string_lossy(), args)
    })
}

/// See [`TitleArgv`].
pub extern "C" fn __p___argc() -> *mut i32 {
    argv().count.get()
}

/// See [`TitleArgv`].
pub extern "C" fn __p___argv() -> *mut *mut *mut c_char {
    argv().list.get()
}

/// The title's thread priorities, but for the lowest two, which become
/// "below normal". Six of the title's threads run at "idle", the streaming
/// pager's among them; on the console they have what the rest of the title
/// leaves of its cores. A PC's processors are also wanted by other programs
/// and by our own workers (sixteen when pipelines are made): with every
/// one of them busy such a thread ran once in four seconds or so, when the
/// host's scheduler took pity, and the loading that waits for it with it
/// (run stream-3: loads seen to be done 1.1 s late on average, 4.8 s at
/// worst). `LEONIDA_TITLE_PRIORITIES=0`: as the title sets them.
fn title_priority(asked: i32) -> i32 {
    const IDLE: i32 = -15;
    const LOWEST: i32 = -2;
    const BELOW_NORMAL: i32 = -1;
    match asked {
        IDLE..=LOWEST => BELOW_NORMAL,
        other => other,
    }
}

/// [`title_priority`].
pub extern "system" fn SetThreadPriority(thread: *mut c_void, priority: i32) -> i32 {
    static AS_SET: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    let as_set = *AS_SET.get_or_init(|| std::env::var_os("LEONIDA_TITLE_PRIORITIES").is_some_and(|v| v == "0"));
    let priority = if as_set { priority } else { title_priority(priority) };
    // SAFETY: the title passes a thread handle.
    unsafe { windows_sys::Win32::System::Threading::SetThreadPriority(thread, priority) }
}

pub(crate) fn module() -> HleModule {
    HleModule {
        name: "kernel",
        dlls: &[
            "kernel32.dll",
            "kernelbase.dll",
            "api-ms-win-core-libraryloader-*",
            "api-ms-win-core-processthreads-*",
            "api-ms-win-core-debug-*",
            "api-ms-win-core-errorhandling-*",
            "api-ms-win-crt-runtime-*",
        ],
        exports: vec![
            hle_export!(__p___argc),
            hle_export!(__p___argv),
            hle_export!(GetModuleHandleW),
            hle_export!(GetModuleHandleA),
            hle_export!(GetModuleFileNameW),
            hle_export!(GetModuleFileNameA),
            hle_export!(LoadLibraryExA),
            hle_export!(LoadLibraryExW),
            hle_export!(LoadLibraryA),
            hle_export!(LoadLibraryW),
            hle_export!(GetProcAddress),
            hle_export!(FreeLibrary),
            hle_export!(DisableThreadLibraryCalls),
            hle_export!(OutputDebugStringA),
            hle_export!(OutputDebugStringW),
            hle_export!(exit),
            hle_export!(abort),
            hle_export!(terminate),
            hle_export!(_invalid_parameter_noinfo_noreturn),
            hle_export!(TerminateProcess),
            hle_export!(SetUnhandledExceptionFilter),
            hle_export!(SetThreadPriority),
        ],
    }
}

/// The return addresses above the caller, as `module+rva`, for logs.
#[cfg(feature = "diagnostics")]
pub(crate) fn callers() -> String {
    host::callers()
}

#[cfg(windows)]
mod host {
    use std::ffi::c_void;

    use windows_sys::Win32::Foundation::{FreeLibrary, GetLastError, SetLastError};
    use windows_sys::Win32::System::Diagnostics::Debug::RtlCaptureStackBackTrace;
    use windows_sys::Win32::System::LibraryLoader::{
        DisableThreadLibraryCalls, GetModuleFileNameA, GetModuleFileNameW, GetModuleHandleA,
        GetModuleHandleW, GetProcAddress, LoadLibraryExA, LoadLibraryExW,
    };
    use windows_sys::Win32::System::Threading::TerminateProcess;

    unsafe extern "C" {
        #[link_name = "exit"]
        fn crt_exit(code: i32) -> !;
        #[link_name = "abort"]
        fn crt_abort() -> !;
    }

    // SAFETY (every function): forwarding the title's own arguments to the
    // host API of the same name.

    pub(super) fn get_module_handle_w(name: *const u16) -> *mut c_void {
        // SAFETY: see above.
        unsafe { GetModuleHandleW(name) }
    }

    pub(super) fn get_module_handle_a(name: *const u8) -> *mut c_void {
        // SAFETY: see above.
        unsafe { GetModuleHandleA(name) }
    }

    pub(super) fn get_module_file_name_w(module: *mut c_void, out: *mut u16, size: u32) -> u32 {
        // SAFETY: see above.
        unsafe { GetModuleFileNameW(module, out, size) }
    }

    pub(super) fn get_module_file_name_a(module: *mut c_void, out: *mut u8, size: u32) -> u32 {
        // SAFETY: see above.
        unsafe { GetModuleFileNameA(module, out, size) }
    }

    pub(super) fn get_last_error() -> u32 {
        // SAFETY: see above.
        unsafe { GetLastError() }
    }

    pub(super) fn set_last_error(code: u32) {
        // SAFETY: see above.
        unsafe { SetLastError(code) }
    }

    pub(super) fn load_library_ex_a(name: *const u8, file: *mut c_void, flags: u32) -> *mut c_void {
        // SAFETY: see above.
        unsafe { LoadLibraryExA(name, file, flags) }
    }

    pub(super) fn load_library_ex_w(
        name: *const u16,
        file: *mut c_void,
        flags: u32,
    ) -> *mut c_void {
        // SAFETY: see above.
        unsafe { LoadLibraryExW(name, file, flags) }
    }

    pub(super) fn get_proc_address(module: *mut c_void, name: *const u8) -> *mut c_void {
        // SAFETY: see above.
        unsafe { GetProcAddress(module, name) }.map_or(std::ptr::null_mut(), |f| f as *mut c_void)
    }

    pub(super) fn free_library(module: *mut c_void) -> i32 {
        // SAFETY: see above.
        unsafe { FreeLibrary(module) }
    }

    pub(super) fn disable_thread_library_calls(module: *mut c_void) -> i32 {
        match leonida_loader::disable_thread_calls(module as usize) {
            Some(done) => i32::from(done),
            // SAFETY: see above.
            None => unsafe { DisableThreadLibraryCalls(module) },
        }
    }

    pub(super) fn exit(code: i32) -> ! {
        // SAFETY: see above.
        unsafe { crt_exit(code) }
    }

    pub(super) fn abort() -> ! {
        // SAFETY: see above.
        unsafe { crt_abort() }
    }

    pub(super) fn terminate_process(process: *mut c_void, code: u32) -> i32 {
        // SAFETY: see above.
        unsafe { TerminateProcess(process, code) }
    }

    pub(super) fn set_unhandled_exception_filter(filter: extern "system" fn(*const c_void) -> i32) {
        // SAFETY: our filter takes the exception pointers as an opaque pointer.
        unsafe {
            windows_sys::Win32::System::Diagnostics::Debug::SetUnhandledExceptionFilter(
                std::mem::transmute::<
                    extern "system" fn(*const c_void) -> i32,
                    windows_sys::Win32::System::Diagnostics::Debug::LPTOP_LEVEL_EXCEPTION_FILTER,
                >(filter),
            )
        };
    }

    pub(super) fn report_unhandled(info: *const c_void) {
        // SAFETY: the system's exception pointers, passed to the filter.
        unsafe { leonida_loader::report_unhandled(info.cast()) };
    }

    pub(super) fn note_title_output(line: &str) {
        leonida_loader::note_title_output(line);
    }

    /// Our caller's callers, as `module+rva`.
    pub(super) fn callers() -> String {
        let mut frames = [std::ptr::null_mut(); 12];
        // SAFETY: fills at most `frames.len()` return addresses.
        let n = unsafe {
            RtlCaptureStackBackTrace(
                2,
                frames.len() as u32,
                frames.as_mut_ptr(),
                std::ptr::null_mut(),
            )
        };
        let names: Vec<_> = frames[..usize::from(n)]
            .iter()
            .map(|&f| leonida_loader::locate(f as usize))
            .collect();
        names.join(" < ")
    }
}

#[cfg(not(windows))]
mod host {
    use std::ffi::c_void;

    pub(super) fn get_module_handle_w(_: *const u16) -> *mut c_void {
        std::ptr::null_mut()
    }

    pub(super) fn get_module_handle_a(_: *const u8) -> *mut c_void {
        std::ptr::null_mut()
    }

    pub(super) fn load_library_ex_a(_: *const u8, _: *mut c_void, _: u32) -> *mut c_void {
        std::ptr::null_mut()
    }

    pub(super) fn get_module_file_name_w(_: *mut c_void, _: *mut u16, _: u32) -> u32 {
        0
    }

    pub(super) fn get_module_file_name_a(_: *mut c_void, _: *mut u8, _: u32) -> u32 {
        0
    }

    pub(super) fn get_last_error() -> u32 {
        0
    }

    pub(super) fn set_last_error(_: u32) {}

    pub(super) fn load_library_ex_w(_: *const u16, _: *mut c_void, _: u32) -> *mut c_void {
        std::ptr::null_mut()
    }

    pub(super) fn get_proc_address(_: *mut c_void, _: *const u8) -> *mut c_void {
        std::ptr::null_mut()
    }

    pub(super) fn free_library(_: *mut c_void) -> i32 {
        0
    }

    pub(super) fn disable_thread_library_calls(_: *mut c_void) -> i32 {
        0
    }

    pub(super) fn exit(code: i32) -> ! {
        std::process::exit(code)
    }

    pub(super) fn abort() -> ! {
        std::process::abort()
    }

    pub(super) fn terminate_process(_: *mut c_void, code: u32) -> i32 {
        std::process::exit(code as i32)
    }

    pub(super) fn set_unhandled_exception_filter(_: extern "system" fn(*const c_void) -> i32) {}

    pub(super) fn report_unhandled(_: *const c_void) {}

    pub(super) fn note_title_output(_: &str) {}

    pub(super) fn callers() -> String {
        String::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The title's idle and lowest threads run below normal; the others as
    /// it sets them.
    #[test]
    fn no_title_thread_waits_for_idle_processors() {
        let given: Vec<i32> = [-15, -2, -1, 0, 1, 2, 15].into_iter().map(title_priority).collect();
        assert_eq!(given, [-1, -1, -1, 0, 1, 2, 15]);
        use windows_sys::Win32::System::Threading::{GetCurrentThread, GetThreadPriority};
        std::thread::spawn(|| {
            // SAFETY: plain calls on this thread.
            let thread = unsafe { GetCurrentThread() };
            assert_eq!(SetThreadPriority(thread, -15), 1);
            // SAFETY: as above.
            assert_eq!(unsafe { GetThreadPriority(thread) }, -1);
        })
        .join()
        .unwrap();
    }

    #[test]
    fn title_images_report_their_original_path() {
        let base = 0x7000_0000_0000;
        runtime::set_image_path(base, std::path::Path::new(r"C:\title\game.exe"));
        let module = base as *mut c_void;
        let mut buf = [0xFFFFu16; 32];
        let n = GetModuleFileNameW(module, buf.as_mut_ptr(), buf.len() as u32);
        assert_eq!(
            String::from_utf16_lossy(&buf[..n as usize]),
            r"C:\title\game.exe"
        );
        assert_eq!(buf[n as usize], 0);

        // Too small: truncated, NUL-terminated, the buffer size returned.
        let mut small = [0u8; 4];
        assert_eq!(GetModuleFileNameA(module, small.as_mut_ptr(), 4), 4);
        assert_eq!(&small, &[b'C', b':', b'\\', 0]);
    }

    /// The title's arguments come right after the program, and the list ends
    /// in a null pointer; without any there is only the program.
    #[test]
    fn the_title_gets_its_own_arguments() {
        let read = |made: &TitleArgv| {
            // SAFETY: made above, kept for the whole run.
            unsafe {
                let (count, list) = (*made.count.get() as usize, *made.list.get());
                assert!((*list.add(count)).is_null());
                (0..count)
                    .map(|i| CStr::from_ptr(*list.add(i)).to_str().unwrap().to_owned())
                    .collect::<Vec<_>>()
            }
        };
        let some = title_argv("leonida.exe", &["-skipToIIS".into(), "-x=1".into()]);
        assert_eq!(read(&some), ["leonida.exe", "-skipToIIS", "-x=1"]);
        assert_eq!(read(&title_argv("leonida.exe", &[])), ["leonida.exe"]);
        // What the title is given: the same list every time.
        assert_eq!(__p___argc(), __p___argc());
        // SAFETY: as above.
        unsafe { assert_eq!((*__p___argc(), (**__p___argv()).is_null()), (1, false)) };
    }

    fn w(text: &str) -> Vec<u16> {
        text.encode_utf16().chain(Some(0)).collect()
    }

    #[cfg(windows)]
    #[test]
    fn host_lookups_by_name_get_our_overrides() {
        let kernel32 = GetModuleHandleW(w("kernel32.dll").as_ptr());
        assert!(!kernel32.is_null());
        let ours = crate::registry::lookup("kernel32.dll", "CreateFileW").unwrap();
        assert_eq!(
            GetProcAddress(kernel32, c"CreateFileW".as_ptr().cast()) as usize,
            ours
        );
        // A plain name stays the host's.
        let host = host::get_proc_address(kernel32, c"GetTickCount".as_ptr().cast());
        assert!(!host.is_null());
        assert_eq!(
            GetProcAddress(kernel32, c"GetTickCount".as_ptr().cast()),
            host
        );
        // Host DLLs outside the host policy do not exist for the title.
        assert!(LoadLibraryW(w("wininet.dll").as_ptr()).is_null());
        assert!(LoadLibraryA(c"C:\\Windows\\System32\\urlmon".as_ptr().cast()).is_null());
        assert!(GetModuleHandleW(w("WININET.DLL").as_ptr()).is_null());
        assert_eq!(
            // SAFETY: reads this thread's last error.
            unsafe { windows_sys::Win32::Foundation::GetLastError() },
            ERROR_MOD_NOT_FOUND
        );
        assert!(!LoadLibraryW(w("kernel32").as_ptr()).is_null());
        // By ordinal too (ws2_32 #4 is `connect`).
        let ws2 = LoadLibraryW(w("ws2_32.dll").as_ptr());
        assert_eq!(
            GetProcAddress(ws2, 4 as *const u8) as usize,
            crate::registry::lookup("ws2_32.dll", "connect").unwrap()
        );
    }

    #[test]
    fn title_dlls_and_pseudo_modules_are_found_by_name() {
        let base = 0x7100_0000_0000usize;
        runtime::set_image_path(base, std::path::Path::new(r"C:\title\Mapped.dll"));
        runtime::set_title_dll_loader(Box::new(|file| match file {
            ".dll" | "" => panic!("empty module name reached title DLL loader: {file:?}"),
            "late.dll" => Ok(Some(0x7200_0000_0000)),
            "broken.dll" => Err("bad image".into()),
            _ => Ok(None),
        }));
        let mapped = base as *mut c_void;
        assert_eq!(GetModuleHandleW(w("MAPPED").as_ptr()), mapped);
        assert_eq!(GetModuleHandleA(c"sub\\mapped.dll".as_ptr().cast()), mapped);
        assert_eq!(LoadLibraryW(w("mapped.dll").as_ptr()), mapped);
        assert_eq!(FreeLibrary(mapped), 1, "title images stay loaded");

        // Not mapped yet: `LoadLibrary` maps it, `GetModuleHandle` does not.
        assert!(GetModuleHandleW(w("late.dll").as_ptr()).is_null());
        assert_eq!(
            LoadLibraryA(c"late".as_ptr().cast()) as usize,
            0x7200_0000_0000
        );
        assert!(LoadLibraryA(c"broken.dll".as_ptr().cast()).is_null());
        // As data it runs no code, so the host loads it (and finds nothing).
        let data = LoadLibraryExW(w("late.dll").as_ptr(), std::ptr::null_mut(), 0x2);
        assert_ne!(data as usize, 0x7200_0000_0000);

        let runtime = LoadLibraryA(c"xgameruntime.dll".as_ptr().cast());
        assert!(!runtime.is_null());
        assert_eq!(GetModuleHandleW(w("XGameRuntime.dll").as_ptr()), runtime);
    }

    /// No buffer: nothing written, the error says it was too small.
    #[test]
    fn a_title_image_name_into_no_buffer() {
        let base = 0x7300_0000_0000;
        runtime::set_image_path(base, std::path::Path::new(r"C:\title\game.exe"));
        let module = base as *mut c_void;
        assert_eq!(GetModuleFileNameW(module, std::ptr::null_mut(), 0), 0);
        assert_eq!(GetModuleFileNameA(module, std::ptr::null_mut(), 0), 0);
        #[cfg(windows)]
        {
            // SAFETY: reads this thread's last error.
            let last = unsafe { windows_sys::Win32::Foundation::GetLastError() };
            assert_eq!(last, ERROR_INSUFFICIENT_BUFFER);
        }
    }

    #[cfg(windows)]
    #[test]
    fn pseudo_modules_resolve_ordinals_report_errors_and_disable_thread_calls() {
        use windows_sys::Win32::Foundation::{GetLastError, SetLastError};
        const TEST_ERROR_PROC_NOT_FOUND: u32 = 127;

        let gi = LoadLibraryA(c"gameinput.dll".as_ptr().cast());
        assert!(!gi.is_null());
        assert_eq!(DisableThreadLibraryCalls(gi), 1);
        let gi_create = crate::registry::lookup("gameinput.dll", "#1").unwrap();
        assert_eq!(
            GetProcAddress(gi, std::ptr::without_provenance(1)) as usize,
            gi_create
        );

        unsafe { SetLastError(0) };
        assert!(GetProcAddress(gi, std::ptr::without_provenance(2)).is_null());
        assert_eq!(unsafe { GetLastError() }, TEST_ERROR_PROC_NOT_FOUND);

        unsafe { SetLastError(0) };
        assert!(GetProcAddress(gi, std::ptr::null()).is_null());
        assert_eq!(unsafe { GetLastError() }, TEST_ERROR_PROC_NOT_FOUND);

        unsafe { SetLastError(0) };
        assert!(GetProcAddress(gi, c"MissingExport".as_ptr().cast()).is_null());
        assert_eq!(unsafe { GetLastError() }, TEST_ERROR_PROC_NOT_FOUND);
    }

    #[cfg(windows)]
    #[test]
    fn host_proc_overrides_handle_long_paths_and_preserve_last_error() {
        use std::os::windows::ffi::OsStrExt;
        use std::path::PathBuf;
        use windows_sys::Win32::Foundation::{FreeLibrary as HostFreeLibrary, GetLastError, SetLastError};
        use windows_sys::Win32::System::LibraryLoader::LoadLibraryW as HostLoadLibraryW;

        let kernel32 = GetModuleHandleW(w("kernel32.dll").as_ptr());
        assert!(!kernel32.is_null());
        unsafe { SetLastError(0x1234) };
        assert_eq!(
            GetProcAddress(kernel32, c"CreateFileW".as_ptr().cast()) as usize,
            crate::registry::lookup("kernel32.dll", "CreateFileW").unwrap()
        );
        assert_eq!(unsafe { GetLastError() }, 0x1234);

        unsafe { SetLastError(0) };
        assert!(GetProcAddress(kernel32, std::ptr::null()).is_null());
        assert_ne!(unsafe { GetLastError() }, 0);

        struct LongModuleGuard {
            handle: *mut c_void,
            root: PathBuf,
        }
        impl Drop for LongModuleGuard {
            fn drop(&mut self) {
                if !self.handle.is_null() {
                    unsafe { HostFreeLibrary(self.handle) };
                }
                let _ = std::fs::remove_dir_all(&self.root);
            }
        }

        let stamp = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap_or_default()
            .as_nanos();
        let temp_base = std::env::temp_dir().join(format!("leo_hle_lp_{}_{stamp}", std::process::id()));
        let raw_base = temp_base.to_string_lossy();
        let verbatim_root = if raw_base.starts_with(r"\\?\") {
            PathBuf::from(raw_base.as_ref())
        } else {
            PathBuf::from(format!(r"\\?\{raw_base}"))
        };
        let deep_dir = verbatim_root.join("d".repeat(120)).join("e".repeat(120));
        std::fs::create_dir_all(&deep_dir).unwrap();
        let deep_dll = deep_dir.join("winhttp.dll");
        let sys_root = std::env::var_os("SystemRoot").unwrap_or_else(|| "C:\\Windows".into());
        let src_dll = PathBuf::from(sys_root).join("System32").join("winhttp.dll");
        std::fs::copy(&src_dll, &deep_dll).unwrap();
        let deep_wide: Vec<u16> = deep_dll.as_os_str().encode_wide().chain(Some(0)).collect();
        let h_long = unsafe { HostLoadLibraryW(deep_wide.as_ptr()) };
        let _guard = LongModuleGuard {
            handle: h_long,
            root: verbatim_root,
        };
        assert!(!h_long.is_null());
        unsafe { SetLastError(0x2345) };
        assert_eq!(
            GetProcAddress(h_long, c"WinHttpOpen".as_ptr().cast()) as usize,
            crate::registry::lookup("winhttp.dll", "WinHttpOpen").unwrap()
        );
        assert_eq!(unsafe { GetLastError() }, 0x2345);
    }

    #[cfg(windows)]
    #[test]
    fn empty_and_trailing_dot_module_names_follow_win32_rules() {
        use windows_sys::Win32::Foundation::{GetLastError, SetLastError};
        const TEST_ERROR_INVALID_PARAMETER: u32 = 87;

        let kernel32 = GetModuleHandleW(w("kernel32.dll").as_ptr());
        assert!(!kernel32.is_null());

        unsafe { SetLastError(0) };
        assert!(LoadLibraryW(w("").as_ptr()).is_null());
        assert_eq!(unsafe { GetLastError() }, TEST_ERROR_INVALID_PARAMETER);
        assert!(LoadLibraryW(w(r"C:\Windows\").as_ptr()).is_null());
        assert_eq!(GetModuleHandleW(w("kernel32.dll.").as_ptr()), kernel32);
        assert_eq!(GetModuleHandleW(w("kernel32.dll..").as_ptr()), kernel32);
        assert_eq!(
            LoadLibraryA(c"xgameruntime.dll.".as_ptr().cast()),
            LoadLibraryA(c"xgameruntime.dll".as_ptr().cast())
        );
        assert!(GetModuleHandleW(w("kernel32.").as_ptr()).is_null());
        assert!(LoadLibraryA(c"wininet.dll.".as_ptr().cast()).is_null());
    }

    #[cfg(windows)]
    #[test]
    fn title_module_filenames_accept_unaligned_uninit_buffers_and_clear_last_error() {
        use std::mem::MaybeUninit;
        use windows_sys::Win32::Foundation::{GetLastError, SetLastError};

        let base = 0x7400_0000_0000usize;
        runtime::set_image_path(base, std::path::Path::new(r"C:\title\unaligned.exe"));
        let title_mod = base as *mut c_void;
        let mut raw_uninit = [MaybeUninit::<u8>::uninit(); 128];
        let odd_w: *mut u16 = unsafe { raw_uninit.as_mut_ptr().add(1).cast() };

        let trunc_len = GetModuleFileNameW(title_mod, odd_w, 5);
        assert_eq!(trunc_len, 5);
        assert_eq!(unsafe { GetLastError() }, ERROR_INSUFFICIENT_BUFFER);
        let trunc_units: Vec<u16> = (0..5)
            .map(|i| unsafe { odd_w.add(i).read_unaligned() })
            .collect();
        assert_eq!(trunc_units, [b'C' as u16, b':' as u16, b'\\' as u16, b't' as u16, 0]);

        let full_len = GetModuleFileNameW(title_mod, odd_w, 40);
        assert_eq!(full_len, 22);
        assert_eq!(unsafe { GetLastError() }, 0);
        let full_units: Vec<u16> = (0..22)
            .map(|i| unsafe { odd_w.add(i).read_unaligned() })
            .collect();
        assert_eq!(
            String::from_utf16_lossy(&full_units),
            r"C:\title\unaligned.exe"
        );
        assert_eq!(unsafe { odd_w.add(22).read_unaligned() }, 0);

        let mut ansi_uninit = [MaybeUninit::<u8>::uninit(); 40];
        unsafe { SetLastError(ERROR_INSUFFICIENT_BUFFER) };
        let ansi_len = GetModuleFileNameA(title_mod, ansi_uninit.as_mut_ptr().cast(), 40);
        assert_eq!(ansi_len, 22);
        assert_eq!(unsafe { GetLastError() }, 0);
    }
}
