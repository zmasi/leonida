//! Leonida: Sony pads (DualSense, DualShock 4) over Windows HID, read without SDL or a remapper.
//!
//! The report layouts are the public ones (the Linux `hid-playstation` driver documents them; this is our own
//! code from them): input reports carry sticks, triggers and buttons at fixed offsets, which differ by model,
//! by cable (USB) or radio (Bluetooth), and by the report in use; output reports set the rumble motors, and
//! over Bluetooth every output report ends in a CRC-32 seeded with one byte of its own.
//!
//! | model | USB input | Bluetooth input | USB output | Bluetooth output |
//! |---|---|---|---|---|
//! | DualSense | `0x01`, 64 bytes, data from byte 1 | `0x31`, 78 bytes, data from byte 2 | `0x02`, 63 | `0x31`, 78 |
//! | DualShock 4 | `0x01`, 64 bytes, data from byte 1 | `0x11`, data from byte 3 | `0x05`, 32 | `0x11`, 78 |
//!
//! A Bluetooth pad sends a short `0x01` report (the layout the DualShock 4 uses on USB) until a feature report is
//! read; reading calibration feature report `0x05` switches it to the full one.
use super::{
    A, B, DOWN, LEFT, LEFT_SHOULDER, LEFT_STICK, MENU, PadState, RIGHT, RIGHT_SHOULDER, RIGHT_STICK, UP, VIEW, X, Y,
};

pub(super) const SONY: u16 = 0x054C;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum Model {
    DualSense,
    DualShock4,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum Link {
    Usb,
    Bluetooth,
}

/// The pad a Sony vendor/product id is: DualSense (`0CE6`) and DualSense Edge (`0DF2`); DualShock 4 (`05C4`,
/// `09CC`) and its USB adapter (`0BA0`).
pub(super) fn model(vendor: u16, product: u16) -> Option<Model> {
    if vendor != SONY {
        return None;
    }
    match product {
        0x0CE6 | 0x0DF2 => Some(Model::DualSense),
        0x05C4 | 0x09CC | 0x0BA0 => Some(Model::DualShock4),
        _ => None,
    }
}

/// A stick axis 0..255 (128 at rest) as -1..1.
fn axis(v: u8) -> f32 {
    ((f32::from(v) - 128.0) / 127.0).clamp(-1.0, 1.0)
}

/// What a decoded input report holds.
pub(super) fn parse(model: Model, report: &[u8]) -> Option<PadState> {
    match (model, *report.first()?) {
        // The full DualSense reports: sticks, then both triggers, a counter, then the buttons.
        (Model::DualSense, 0x01) if report.len() >= 64 => extended(report, 1),
        (Model::DualSense, 0x31) if report.len() >= 12 => extended(report, 2),
        // The short layout: a DualShock 4's USB report, a Bluetooth pad's before the feature report.
        (_, 0x01) if report.len() >= 10 => simple(report, 1),
        (Model::DualShock4, 0x11) if report.len() >= 12 => simple(report, 3),
        _ => None,
    }
}

/// Sticks at `base`..+3, L2 and R2 at +4 and +5, the counter at +6, the buttons from +7.
fn extended(r: &[u8], base: usize) -> Option<PadState> {
    buttons(r, base, base + 4, base + 7)
}

/// Sticks at `base`..+3, the buttons from +4, L2 and R2 at +7 and +8.
fn simple(r: &[u8], base: usize) -> Option<PadState> {
    buttons(r, base, base + 7, base + 4)
}

fn buttons(r: &[u8], sticks: usize, triggers: usize, face: usize) -> Option<PadState> {
    let r = r.get(..(face + 3).max(triggers + 2).max(sticks + 4))?;
    let (face_byte, shoulder, system) = (r[face], r[face + 1], r[face + 2]);
    let mut pad = PadState {
        left_x: axis(r[sticks]),
        left_y: -axis(r[sticks + 1]),
        right_x: axis(r[sticks + 2]),
        right_y: -axis(r[sticks + 3]),
        left_trigger: f32::from(r[triggers]) / 255.0,
        right_trigger: f32::from(r[triggers + 1]) / 255.0,
        ..Default::default()
    };
    // The d-pad is a hat switch: 0 up, then clockwise by eighths; 8 and above, released.
    pad.buttons |= match face_byte & 0x0F {
        0 => UP,
        1 => UP | RIGHT,
        2 => RIGHT,
        3 => DOWN | RIGHT,
        4 => DOWN,
        5 => DOWN | LEFT,
        6 => LEFT,
        7 => UP | LEFT,
        _ => 0,
    };
    // Square, Cross, Circle and Triangle sit where the console's X, A, B and Y do (and are named by them in the
    // title's own mapping); L2 and R2 are read as triggers only.
    for (mask, button) in [
        (face_byte, [(0x10, X), (0x20, A), (0x40, B), (0x80, Y)].as_slice()),
        (
            shoulder,
            [(0x01, LEFT_SHOULDER), (0x02, RIGHT_SHOULDER), (0x10, VIEW), (0x20, MENU), (0x40, LEFT_STICK), (0x80, RIGHT_STICK)]
                .as_slice(),
        ),
        // The touchpad's click is the map, as on the PlayStation.
        (system, [(0x02, VIEW)].as_slice()),
    ] {
        for &(bit, out) in button {
            if mask & bit != 0 {
                pad.buttons |= out;
            }
        }
    }
    Some(pad)
}

/// CRC-32 (IEEE, reflected) of `seed` and then `data`: Bluetooth output reports end with it, seeded with `0xA2`
/// (input reports are seeded with `0xA1`).
pub(super) fn crc32(seed: u8, data: &[u8]) -> u32 {
    let mut crc = !0u32;
    for &byte in std::iter::once(&seed).chain(data) {
        crc ^= u32::from(byte);
        for _ in 0..8 {
            crc = (crc >> 1) ^ (0xEDB8_8320 & 0u32.wrapping_sub(crc & 1));
        }
    }
    !crc
}

/// The output report that sets both rumble motors (`low`: the big one on the left, `high`: the small one),
/// `len` bytes (the device's output report size; never shorter than the report needs). `seq` numbers Bluetooth
/// DualSense reports.
pub(super) fn rumble_report(model: Model, link: Link, len: usize, low: u16, high: u16, seq: u8) -> Vec<u8> {
    let (low, high) = ((low >> 8) as u8, (high >> 8) as u8);
    let bluetooth = link == Link::Bluetooth;
    let (needed, first) = match (model, bluetooth) {
        (Model::DualSense, false) => (63, 1),
        (Model::DualSense, true) => (78, 3),
        (Model::DualShock4, false) => (32, 1),
        (Model::DualShock4, true) => (78, 3),
    };
    let mut r = vec![0u8; if bluetooth { needed } else { len.max(needed) }];
    match model {
        Model::DualSense => {
            r[0] = if bluetooth { 0x31 } else { 0x02 };
            if bluetooth {
                r[1] = (seq & 0x0F) << 4;
                r[2] = 0x10;
            }
            // Valid flags: the rumble motors (compatible vibration, haptics select), and compatible vibration 2
            // (flags byte 2) which newer firmware wants.
            r[first] = 0x03;
            r[first + 2] = high;
            r[first + 3] = low;
            r[first + 38] = 0x04;
        }
        Model::DualShock4 => {
            r[0] = if bluetooth { 0x11 } else { 0x05 };
            if bluetooth {
                // HID report with a CRC.
                r[1] = 0xC0;
            }
            // Valid flags: the motors; then (one reserved byte after the second flags byte) right and left.
            r[first] = 0x01;
            r[first + 3] = high;
            r[first + 4] = low;
        }
    }
    if bluetooth {
        let end = r.len() - 4;
        let crc = crc32(0xA2, &r[..end]);
        r[end..].copy_from_slice(&crc.to_le_bytes());
    }
    r
}

#[cfg(windows)]
pub(super) use imp::{Sony, View};

#[cfg(windows)]
mod imp {
    use std::collections::HashSet;
    use std::sync::atomic::{AtomicBool, Ordering};
    use std::sync::{Arc, Mutex};
    use std::time::{Duration, Instant};

    use windows_sys::Win32::Devices::DeviceAndDriverInstallation::{
        DIGCF_DEVICEINTERFACE, DIGCF_PRESENT, SP_DEVICE_INTERFACE_DATA, SP_DEVICE_INTERFACE_DETAIL_DATA_W,
        SetupDiDestroyDeviceInfoList, SetupDiEnumDeviceInterfaces, SetupDiGetClassDevsW,
        SetupDiGetDeviceInterfaceDetailW,
    };
    use windows_sys::Win32::Devices::HumanInterfaceDevice::{
        HIDD_ATTRIBUTES, HIDP_CAPS, HidD_FreePreparsedData, HidD_GetAttributes, HidD_GetFeature, HidD_GetHidGuid,
        HidD_GetPreparsedData, HidD_GetProductString, HidP_GetCaps,
    };
    use windows_sys::Win32::Foundation::{CloseHandle, GENERIC_READ, GENERIC_WRITE, HANDLE, INVALID_HANDLE_VALUE};
    use windows_sys::Win32::Storage::FileSystem::{
        CreateFileW, FILE_SHARE_READ, FILE_SHARE_WRITE, OPEN_EXISTING, ReadFile, WriteFile,
    };

    use super::*;
    use crate::modules::gameinput::merged;

    /// A rescan for pads that came or went (the enumeration costs milliseconds).
    const RESCAN: Duration = Duration::from_secs(2);

    /// A report a reader thread decoded last.
    #[derive(Default)]
    struct Latest {
        pad: Option<PadState>,
    }

    struct Shared {
        latest: Mutex<Latest>,
        alive: AtomicBool,
    }

    /// An open Sony pad. The handles are plain numbers so the pad can cross threads.
    struct Pad {
        path: Vec<u16>,
        name: String,
        model: Model,
        link: Link,
        out_len: usize,
        write: usize,
        shared: Arc<Shared>,
        seq: u8,
    }

    impl Drop for Pad {
        fn drop(&mut self) {
            // SAFETY: the write handle is ours alone; the reader owns the other.
            unsafe { CloseHandle(self.write as HANDLE) };
        }
    }

    /// What the poll sees of the Sony pads.
    #[derive(Clone, Debug, Default)]
    pub(in crate::modules::gameinput) struct View {
        /// A Sony pad is plugged in or paired (whether or not we can read it).
        pub present: bool,
        /// One is open and has sent a report.
        pub readable: bool,
        /// All readable pads as one.
        pub pad: Option<PadState>,
    }

    #[derive(Default)]
    pub(in crate::modules::gameinput) struct Sony {
        pads: Vec<Pad>,
        /// Paths known not to be Sony gamepads.
        ignored: HashSet<Vec<u16>>,
        /// Sony pads we could not open (another program holds them exclusively, or they are hidden).
        blocked: usize,
        scanned: Option<Instant>,
        last_rumble: (u16, u16),
    }

    impl Sony {
        /// Looks for pads now and then, drops those that went away and reads the rest.
        pub(in crate::modules::gameinput) fn poll(&mut self) -> View {
            if self.scanned.is_none_or(|at| at.elapsed() >= RESCAN) {
                self.scanned = Some(Instant::now());
                self.pads.retain(|p| {
                    let alive = p.shared.alive.load(Ordering::Acquire);
                    if !alive {
                        tracing::info!(pad = %p.name, "Sony pad: gone");
                    }
                    alive
                });
                self.scan();
            }
            let mut view = View { present: !self.pads.is_empty() || self.blocked > 0, ..Default::default() };
            for pad in &self.pads {
                let latest = pad.shared.latest.lock().unwrap_or_else(|e| e.into_inner()).pad;
                if let Some(state) = latest {
                    view.readable = true;
                    view.pad = Some(view.pad.map_or(state, |all| merged(all, state)));
                }
            }
            view
        }

        /// Both motors, as XInput speeds. Only what changed is sent.
        pub(in crate::modules::gameinput) fn rumble(&mut self, low: u16, high: u16) {
            if self.last_rumble == (low, high) {
                return;
            }
            self.last_rumble = (low, high);
            for pad in &mut self.pads {
                pad.seq = pad.seq.wrapping_add(1);
                let report = rumble_report(pad.model, pad.link, pad.out_len, low, high, pad.seq);
                let mut written = 0;
                // SAFETY: a report buffer and a handle we own.
                let ok = unsafe {
                    WriteFile(pad.write as HANDLE, report.as_ptr(), report.len() as u32, &mut written, std::ptr::null_mut())
                };
                if ok == 0 {
                    tracing::debug!(pad = %pad.name, "Sony pad: rumble not written");
                }
            }
        }

        fn scan(&mut self) {
            let mut guid: windows_sys::core::GUID = unsafe { std::mem::zeroed() };
            // SAFETY: an output.
            unsafe { HidD_GetHidGuid(&mut guid) };
            // SAFETY: the class guid; no enumerator or window.
            let set = unsafe {
                SetupDiGetClassDevsW(&guid, std::ptr::null(), std::ptr::null_mut(), DIGCF_PRESENT | DIGCF_DEVICEINTERFACE)
            };
            if set == -1 || set == 0 {
                return;
            }
            let mut blocked = 0;
            for index in 0.. {
                // SAFETY: zeroed plain data, then its size set.
                let mut interface: SP_DEVICE_INTERFACE_DATA = unsafe { std::mem::zeroed() };
                interface.cbSize = size_of::<SP_DEVICE_INTERFACE_DATA>() as u32;
                // SAFETY: the set and the guid are ours.
                if unsafe { SetupDiEnumDeviceInterfaces(set, std::ptr::null(), &guid, index, &mut interface) } == 0 {
                    break;
                }
                let mut need = 0;
                // SAFETY: asks for the size.
                unsafe { SetupDiGetDeviceInterfaceDetailW(set, &interface, std::ptr::null_mut(), 0, &mut need, std::ptr::null_mut()) };
                if need == 0 {
                    continue;
                }
                // u64 units for the alignment of the detail structure.
                let mut buffer = vec![0u64; (need as usize).div_ceil(8)];
                let detail = buffer.as_mut_ptr().cast::<SP_DEVICE_INTERFACE_DETAIL_DATA_W>();
                // SAFETY: the buffer holds `need` bytes.
                let ok = unsafe {
                    (*detail).cbSize = size_of::<SP_DEVICE_INTERFACE_DETAIL_DATA_W>() as u32;
                    SetupDiGetDeviceInterfaceDetailW(set, &interface, detail, need, std::ptr::null_mut(), std::ptr::null_mut())
                };
                if ok == 0 {
                    continue;
                }
                // SAFETY: the path is a NUL-terminated string in the buffer.
                let path: Vec<u16> = unsafe {
                    let start = std::ptr::addr_of!((*detail).DevicePath).cast::<u16>();
                    (0..).map(|i| *start.add(i)).take_while(|&c| c != 0).chain(Some(0)).collect()
                };
                if self.ignored.contains(&path) || self.pads.iter().any(|p| p.path == path) {
                    continue;
                }
                match self.open(&path) {
                    Opened::Pad(pad) => {
                        tracing::info!(pad = %pad.name, model = ?pad.model, link = ?pad.link, "Sony pad: reading it over HID");
                        self.pads.push(pad);
                    }
                    Opened::Blocked => blocked += 1,
                    Opened::Other => {
                        self.ignored.insert(path);
                    }
                }
            }
            // SAFETY: the set is ours.
            unsafe { SetupDiDestroyDeviceInfoList(set) };
            if blocked != self.blocked {
                tracing::info!(blocked, "Sony pads present that cannot be opened (hidden or held by another program)");
            }
            self.blocked = blocked;
        }

        fn open(&self, path: &[u16]) -> Opened {
            let create = |access: u32| -> HANDLE {
                // SAFETY: a NUL-terminated path.
                unsafe {
                    CreateFileW(
                        path.as_ptr(),
                        access,
                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                        std::ptr::null(),
                        OPEN_EXISTING,
                        0,
                        std::ptr::null_mut(),
                    )
                }
            };
            // Who is it: a handle with no access answers attribute queries and bothers no one.
            let probe = create(0);
            if probe == INVALID_HANDLE_VALUE {
                return Opened::Other;
            }
            let mut attributes = HIDD_ATTRIBUTES { Size: size_of::<HIDD_ATTRIBUTES>() as u32, ..unsafe { std::mem::zeroed() } };
            // SAFETY: an output.
            let known = unsafe { HidD_GetAttributes(probe, &mut attributes) };
            let model = known.then(|| model(attributes.VendorID, attributes.ProductID)).flatten();
            let caps = model.and_then(|_| {
                let mut data = 0isize;
                // SAFETY: outputs.
                unsafe {
                    if !HidD_GetPreparsedData(probe, &mut data) {
                        return None;
                    }
                    let mut caps: HIDP_CAPS = std::mem::zeroed();
                    let ok = HidP_GetCaps(data, &mut caps) == 0x0011_0000;
                    HidD_FreePreparsedData(data);
                    ok.then_some(caps)
                }
            });
            // SAFETY: ours.
            unsafe { CloseHandle(probe) };
            let (Some(model), Some(caps)) = (model, caps) else {
                return Opened::Other;
            };
            // A game pad (generic desktop, game pad), not the motion or audio collections.
            if caps.UsagePage != 1 || caps.Usage != 5 {
                return Opened::Other;
            }
            let (read, write) = (create(GENERIC_READ), create(GENERIC_READ | GENERIC_WRITE));
            if read == INVALID_HANDLE_VALUE || write == INVALID_HANDLE_VALUE {
                // SAFETY: closing what opened.
                unsafe {
                    for h in [read, write] {
                        if h != INVALID_HANDLE_VALUE {
                            CloseHandle(h);
                        }
                    }
                }
                return Opened::Blocked;
            }
            let link = if caps.InputReportByteLength > 64 { Link::Bluetooth } else { Link::Usb };
            let mut name = [0u16; 128];
            // SAFETY: a 256-byte buffer.
            let name = if unsafe { HidD_GetProductString(write, name.as_mut_ptr().cast(), 256) } {
                let n = name.iter().position(|&c| c == 0).unwrap_or(name.len());
                String::from_utf16_lossy(&name[..n])
            } else {
                format!("{model:?}")
            };
            if link == Link::Bluetooth {
                // Reading the calibration feature report moves the pad to its full input report.
                let mut feature = [0u8; 41];
                feature[0] = 0x05;
                // SAFETY: a 41-byte buffer, as the report is.
                unsafe { HidD_GetFeature(write, feature.as_mut_ptr().cast(), feature.len() as u32) };
            }
            let shared = Arc::new(Shared { latest: Mutex::default(), alive: AtomicBool::new(true) });
            let size = usize::from(caps.InputReportByteLength);
            let reader = Arc::clone(&shared);
            let read = read as usize;
            std::thread::Builder::new()
                .name("sony-pad".into())
                .spawn(move || {
                    let mut buffer = vec![0u8; size.max(78)];
                    loop {
                        let mut got = 0;
                        // SAFETY: a buffer of `size` bytes and a handle this thread owns.
                        let ok = unsafe { ReadFile(read as HANDLE, buffer.as_mut_ptr(), size as u32, &mut got, std::ptr::null_mut()) };
                        if ok == 0 {
                            break;
                        }
                        if let Some(pad) = parse(model, &buffer[..got as usize]) {
                            reader.latest.lock().unwrap_or_else(|e| e.into_inner()).pad = Some(pad);
                        }
                    }
                    reader.alive.store(false, Ordering::Release);
                    // SAFETY: the handle is this thread's.
                    unsafe { CloseHandle(read as HANDLE) };
                })
                .ok();
            Opened::Pad(Pad {
                path: path.to_vec(),
                name,
                model,
                link,
                out_len: usize::from(caps.OutputReportByteLength),
                write: write as usize,
                shared,
                seq: 0,
            })
        }
    }

    enum Opened {
        Pad(Pad),
        /// A Sony pad that cannot be opened.
        Blocked,
        /// Not a Sony game pad.
        Other,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A 64-byte DualSense USB report (id 1) with the given bytes at their offsets.
    fn usb_dualsense(set: &[(usize, u8)]) -> Vec<u8> {
        let mut r = vec![0u8; 64];
        r[0] = 0x01;
        // Sticks centred, d-pad released.
        r[1..5].fill(128);
        r[8] = 0x08;
        for &(i, v) in set {
            r[i] = v;
        }
        r
    }

    #[test]
    fn dualsense_usb_idle_and_pushed() {
        let idle = parse(Model::DualSense, &usb_dualsense(&[])).unwrap();
        assert_eq!(idle, PadState::default());
        // Left stick all the way left and up (up is low), right stick down-right, L2 half, R2 full.
        let pad = parse(Model::DualSense, &usb_dualsense(&[(1, 0), (2, 0), (3, 255), (4, 255), (5, 128), (6, 255)])).unwrap();
        assert_eq!((pad.left_x, pad.left_y), (-1.0, 1.0));
        assert_eq!((pad.right_x, pad.right_y), (1.0, -1.0));
        assert!((pad.left_trigger - 128.0 / 255.0).abs() < 1e-6);
        assert_eq!(pad.right_trigger, 1.0);
    }

    #[test]
    fn dualsense_buttons() {
        // Face byte: hat in the low nibble (up-right = 1), square, cross, circle, triangle in bits 4 to 7.
        let face = |bits: u8| parse(Model::DualSense, &usb_dualsense(&[(8, bits)])).unwrap().buttons;
        assert_eq!(face(0x08), 0);
        assert_eq!(face(0x01), UP | RIGHT);
        assert_eq!(face(0x04), DOWN);
        assert_eq!(face(0x06), LEFT);
        assert_eq!(face(0x18), X);
        assert_eq!(face(0x28), A);
        assert_eq!(face(0x48), B);
        assert_eq!(face(0x88), Y);
        assert_eq!(face(0xF8), X | A | B | Y);
        // Second byte: L1, R1, (L2, R2 digital), Create, Options, L3, R3.
        let shoulders = |bits: u8| parse(Model::DualSense, &usb_dualsense(&[(9, bits)])).unwrap().buttons;
        assert_eq!(shoulders(0x01), LEFT_SHOULDER);
        assert_eq!(shoulders(0x02), RIGHT_SHOULDER);
        assert_eq!(shoulders(0x0C), 0, "the digital triggers are read as the analog ones");
        assert_eq!(shoulders(0x10), VIEW);
        assert_eq!(shoulders(0x20), MENU);
        assert_eq!(shoulders(0x40), LEFT_STICK);
        assert_eq!(shoulders(0x80), RIGHT_STICK);
        // Third byte: PS (nothing), touchpad click (the map), mute (nothing).
        let system = |bits: u8| parse(Model::DualSense, &usb_dualsense(&[(10, bits)])).unwrap().buttons;
        assert_eq!(system(0x01), 0);
        assert_eq!(system(0x02), VIEW);
        assert_eq!(system(0x04), 0);
    }

    #[test]
    fn dualsense_bluetooth_has_one_more_header_byte() {
        let mut r = vec![0u8; 78];
        r[0] = 0x31;
        r[1] = 0x01; // sequence tag
        r[2..6].fill(128);
        r[2] = 255; // left stick right
        r[6] = 200; // L2
        r[9] = 0x28; // cross, d-pad released (hat 8)
        r[10] = 0x02; // R1
        let pad = parse(Model::DualSense, &r).unwrap();
        assert_eq!(pad.left_x, 1.0);
        assert!((pad.left_trigger - 200.0 / 255.0).abs() < 1e-6);
        assert_eq!(pad.buttons, A | RIGHT_SHOULDER);
        assert_eq!(parse(Model::DualSense, &r[..11]), None, "a cut-off report");
    }

    /// The short layout: id 1, sticks at 1..4, buttons at 5..7, triggers at 8 and 9.
    fn simple_report(len: usize) -> Vec<u8> {
        let mut r = vec![0u8; len];
        r[0] = 0x01;
        r[1..5].fill(128);
        r[5] = 0x08;
        r
    }

    #[test]
    fn dualshock4_usb_and_the_short_bluetooth_report() {
        let mut r = simple_report(64);
        r[3] = 0; // right stick left
        r[5] = 0x41; // circle, up-right
        r[6] = 0x10 | 0x80; // share, R3
        r[7] = 0x02; // touchpad
        r[8] = 255;
        let pad = parse(Model::DualShock4, &r).unwrap();
        assert_eq!(pad.right_x, -1.0);
        assert_eq!(pad.left_trigger, 1.0);
        assert_eq!(pad.buttons, B | UP | RIGHT | VIEW | RIGHT_STICK);
        // The same layout when a Bluetooth DualSense has not been switched to its full report yet.
        let short = simple_report(10);
        assert_eq!(parse(Model::DualSense, &short), Some(PadState::default()));
        assert_eq!(parse(Model::DualSense, &short[..9]), None);
    }

    #[test]
    fn dualshock4_bluetooth_report_11() {
        let mut r = vec![0u8; 78];
        r[0] = 0x11;
        r[1] = 0xC0;
        r[3..7].fill(128);
        r[4] = 0; // left stick up
        r[7] = 0x88; // triangle, hat released
        r[8] = 0x20; // options
        r[11] = 128; // R2
        let pad = parse(Model::DualShock4, &r).unwrap();
        assert_eq!((pad.left_x, pad.left_y), (0.0, 1.0));
        assert_eq!(pad.buttons, Y | MENU);
        assert!((pad.right_trigger - 128.0 / 255.0).abs() < 1e-6);
        assert_eq!(parse(Model::DualSense, &r), None, "a DualSense does not send report 0x11");
        assert_eq!(parse(Model::DualShock4, &[]), None);
        assert_eq!(parse(Model::DualShock4, &[0x07; 64]), None, "an unknown report");
    }

    #[test]
    fn crc_matches_the_standard_check_value() {
        // The textbook bitwise CRC-32 (IEEE) of "123456789" is 0xCBF43926; the seeded one is that over the seed
        // byte followed by the data.
        let direct = |bytes: &[u8]| {
            let mut crc = !0u32;
            for &b in bytes {
                crc ^= u32::from(b);
                for _ in 0..8 {
                    crc = if crc & 1 != 0 { (crc >> 1) ^ 0xEDB8_8320 } else { crc >> 1 };
                }
            }
            !crc
        };
        assert_eq!(direct(b"123456789"), 0xCBF4_3926);
        let data = b"123456789";
        let mut seeded = vec![0xA2];
        seeded.extend_from_slice(data);
        assert_eq!(crc32(0xA2, data), direct(&seeded));
    }

    #[test]
    fn rumble_reports() {
        // DualSense over USB: id 2; motors at 3 (small, right) and 4 (big, left); flags 3 and compat. vibration 2.
        let r = rumble_report(Model::DualSense, Link::Usb, 63, 0xFF00, 0x8000, 0);
        assert_eq!((r.len(), r[0], r[1], r[3], r[4], r[39]), (63, 0x02, 0x03, 0x80, 0xFF, 0x04));
        // Over Bluetooth: id 0x31, sequence in the high nibble, tag 0x10, data from 3, CRC last.
        let r = rumble_report(Model::DualSense, Link::Bluetooth, 78, 0x4000, 0xFFFF, 5);
        assert_eq!((r.len(), r[0], r[1], r[2], r[3], r[5], r[6]), (78, 0x31, 0x50, 0x10, 0x03, 0xFF, 0x40));
        let crc = crc32(0xA2, &r[..74]);
        assert_eq!(&r[74..], &crc.to_le_bytes());
        // DualShock 4 over USB: id 5, flag 1, motors at 4 (small) and 5 (big).
        let r = rumble_report(Model::DualShock4, Link::Usb, 32, 0xFF00, 0x2000, 0);
        assert_eq!((r.len(), r[0], r[1], r[4], r[5]), (32, 0x05, 0x01, 0x20, 0xFF));
        // Over Bluetooth: id 0x11, 0xC0, data from 3, motors at 6 and 7, CRC last.
        let r = rumble_report(Model::DualShock4, Link::Bluetooth, 78, 0xFF00, 0x2000, 0);
        assert_eq!((r.len(), r[0], r[1], r[3], r[6], r[7]), (78, 0x11, 0xC0, 0x01, 0x20, 0xFF));
        assert_eq!(&r[74..], &crc32(0xA2, &r[..74]).to_le_bytes());
        // Stopped.
        let r = rumble_report(Model::DualSense, Link::Usb, 63, 0, 0, 0);
        assert_eq!((r[3], r[4]), (0, 0));
        // A device that reports a longer output report gets it zero-padded.
        assert_eq!(rumble_report(Model::DualShock4, Link::Usb, 40, 0, 0, 0).len(), 40);
    }

    #[test]
    fn products() {
        assert_eq!(model(SONY, 0x0CE6), Some(Model::DualSense));
        assert_eq!(model(SONY, 0x0DF2), Some(Model::DualSense));
        assert_eq!(model(SONY, 0x05C4), Some(Model::DualShock4));
        assert_eq!(model(SONY, 0x09CC), Some(Model::DualShock4));
        assert_eq!(model(0x045E, 0x0CE6), None);
        assert_eq!(model(SONY, 0x0001), None);
    }
}
