//! Key names a mod uses (`"W"`, `"F5"`, `"Shift"`, `"Num4"`) and the virtual-key
//! codes behind them.

/// Names and virtual-key codes. The first name of a code is the one events use.
const NAMES: &[(&str, u8)] = &[
    ("MouseLeft", 0x01),
    ("MouseRight", 0x02),
    ("MouseMiddle", 0x04),
    ("Back", 0x08),
    ("Tab", 0x09),
    ("Enter", 0x0D),
    ("Shift", 0x10),
    ("Ctrl", 0x11),
    ("Alt", 0x12),
    ("Pause", 0x13),
    ("CapsLock", 0x14),
    ("Esc", 0x1B),
    ("Space", 0x20),
    ("PageUp", 0x21),
    ("PageDown", 0x22),
    ("End", 0x23),
    ("Home", 0x24),
    ("Left", 0x25),
    ("Up", 0x26),
    ("Right", 0x27),
    ("Down", 0x28),
    ("Insert", 0x2D),
    ("Delete", 0x2E),
    ("Num0", 0x60),
    ("Num1", 0x61),
    ("Num2", 0x62),
    ("Num3", 0x63),
    ("Num4", 0x64),
    ("Num5", 0x65),
    ("Num6", 0x66),
    ("Num7", 0x67),
    ("Num8", 0x68),
    ("Num9", 0x69),
    ("NumMul", 0x6A),
    ("NumAdd", 0x6B),
    ("NumSub", 0x6D),
    ("NumDot", 0x6E),
    ("NumDiv", 0x6F),
    ("LShift", 0xA0),
    ("RShift", 0xA1),
    ("LCtrl", 0xA2),
    ("RCtrl", 0xA3),
    ("LAlt", 0xA4),
    ("RAlt", 0xA5),
    ("Semicolon", 0xBA),
    ("Equals", 0xBB),
    ("Comma", 0xBC),
    ("Minus", 0xBD),
    ("Period", 0xBE),
    ("Slash", 0xBF),
    ("Tilde", 0xC0),
    ("LBracket", 0xDB),
    ("Backslash", 0xDC),
    ("RBracket", 0xDD),
    ("Quote", 0xDE),
];

/// The virtual-key code of `name` (case-insensitive): a letter, a digit, `F1`
/// to `F24` or one of the names above.
pub fn code(name: &str) -> Option<u8> {
    let b = name.as_bytes();
    if b.len() == 1 && b[0].is_ascii_alphanumeric() {
        return Some(b[0].to_ascii_uppercase());
    }
    if let Some(n) = name
        .strip_prefix(['F', 'f'])
        .and_then(|n| n.parse::<u8>().ok())
        && (1..=24).contains(&n)
    {
        return Some(0x6F + n);
    }
    NAMES
        .iter()
        .find(|(n, _)| n.eq_ignore_ascii_case(name))
        .map(|&(_, vk)| vk)
}

/// The name events give virtual key `vk`.
pub fn name(vk: u8) -> String {
    match vk {
        b'0'..=b'9' | b'A'..=b'Z' => (vk as char).to_string(),
        0x70..=0x87 => format!("F{}", vk - 0x6F),
        _ => NAMES
            .iter()
            .find(|&&(_, code)| code == vk)
            .map_or_else(|| format!("Key{vk:#04X}"), |&(n, _)| n.to_owned()),
    }
}

/// The codes worth looking at for key events: every key a keyboard has.
pub fn watched() -> impl Iterator<Item = u8> {
    (0x01..=0xFEu8).filter(|vk| !matches!(vk, 0x03 | 0x05..=0x07 | 0x0A..=0x0B | 0x0E..=0x0F))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn names_and_codes_agree() {
        assert_eq!(code("w"), Some(b'W'));
        assert_eq!(code("7"), Some(b'7'));
        assert_eq!(code("F4"), Some(0x73));
        assert_eq!(code("f12"), Some(0x7B));
        assert_eq!(code("F25"), None);
        assert_eq!(code("space"), Some(0x20));
        assert_eq!(code("Num4"), Some(0x64));
        assert_eq!(code("nothing"), None);
        assert_eq!(code(""), None);
        assert_eq!(name(0x73), "F4");
        assert_eq!(name(b'Q'), "Q");
        assert_eq!(name(0x20), "Space");
        assert_eq!(name(0xE7), "Key0xE7");
        for &(n, vk) in NAMES {
            assert_eq!(code(n), Some(vk), "{n}");
            assert_eq!(name(vk), n);
        }
        for vk in watched() {
            assert_eq!(code(&name(vk)).unwrap_or(vk), vk, "{vk:#x}");
        }
    }
}
