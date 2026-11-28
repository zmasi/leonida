//! Glyphs as coverage bitmaps, and where they come from: a system font
//! rasterised at run time (`crate::gdi` on Windows; nothing of a font is
//! shipped), or the built-in 5x7 dot font, which needs nothing and is what
//! tests and other platforms get.

/// The faces the menu uses.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Face {
    /// The banner's title: a large script face.
    Script,
    /// Row text.
    Body,
    /// The caption bar: a heavier body face.
    Bold,
}

/// One character's picture: `alpha` is `width * height` coverage bytes
/// (0 to 255); `left` and `top` place its top-left corner from the pen
/// position on the baseline (`top` upwards).
#[derive(Clone, Debug, Default)]
pub struct Glyph {
    pub width: u32,
    pub height: u32,
    pub left: i32,
    pub top: i32,
    pub advance: i32,
    pub alpha: Vec<u8>,
}

/// A face at one size: the printable ASCII characters.
#[derive(Clone, Debug, Default)]
pub struct Atlas {
    /// Characters 32 to 126.
    pub glyphs: Vec<Glyph>,
    /// Above and below the baseline, in pixels.
    pub ascent: i32,
    pub descent: i32,
}

impl Atlas {
    /// The glyph of `c`; `?` for one it does not have.
    pub fn glyph(&self, c: char) -> Option<&Glyph> {
        let at = |c: u32| c.checked_sub(32).and_then(|i| self.glyphs.get(i as usize));
        at(c as u32).or_else(|| at(u32::from(b'?')))
    }

    /// How wide `text` is.
    pub fn measure(&self, text: &str) -> i32 {
        text.chars()
            .filter_map(|c| self.glyph(c))
            .map(|g| g.advance)
            .sum()
    }

    /// The height of a line: ascent and descent.
    pub fn height(&self) -> i32 {
        self.ascent + self.descent
    }
}

/// Makes atlases.
pub trait FontSource {
    /// `face` with `px` pixels from the top of the tallest letters to the
    /// bottom of the lowest (the em height).
    fn atlas(&self, face: Face, px: u32) -> Atlas;
}

/// The built-in dot font, scaled to whole dots; the script face is the bold
/// one, larger.
pub struct Dots;

impl FontSource for Dots {
    fn atlas(&self, face: Face, px: u32) -> Atlas {
        // Seven dots of letter and two below, in an em of ten.
        let dot = (px / 10).max(1);
        let bold = face != Face::Body;
        let glyphs = (32u8..=126)
            .map(|c| {
                let rows = dots(c);
                let extra = u32::from(bold);
                let (w, h) = ((5 + extra) * dot, 7 * dot);
                let mut alpha = vec![0u8; (w * h) as usize];
                for (y, row) in rows.iter().enumerate() {
                    for x in 0..5u32 {
                        if row >> (4 - x) & 1 == 0 {
                            continue;
                        }
                        for bx in 0..=extra {
                            for dy in 0..dot {
                                for dx in 0..dot {
                                    let px = (x + bx) * dot + dx;
                                    let py = y as u32 * dot + dy;
                                    alpha[(py * w + px) as usize] = 255;
                                }
                            }
                        }
                    }
                }
                Glyph {
                    width: w,
                    height: h,
                    left: 0,
                    top: 7 * dot as i32,
                    advance: ((6 + extra) * dot) as i32,
                    alpha,
                }
            })
            .collect();
        Atlas {
            glyphs,
            ascent: 8 * dot as i32,
            descent: 2 * dot as i32,
        }
    }
}

/// Seven rows of five dots, the leftmost the highest bit; small letters
/// are drawn as capitals.
#[rustfmt::skip]
fn dots(c: u8) -> [u8; 7] {
    match c.to_ascii_uppercase() {
        b'0' => [0b01110, 0b10001, 0b10011, 0b10101, 0b11001, 0b10001, 0b01110],
        b'1' => [0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110],
        b'2' => [0b01110, 0b10001, 0b00001, 0b00110, 0b01000, 0b10000, 0b11111],
        b'3' => [0b11110, 0b00001, 0b00001, 0b01110, 0b00001, 0b00001, 0b11110],
        b'4' => [0b00010, 0b00110, 0b01010, 0b10010, 0b11111, 0b00010, 0b00010],
        b'5' => [0b11111, 0b10000, 0b11110, 0b00001, 0b00001, 0b10001, 0b01110],
        b'6' => [0b00110, 0b01000, 0b10000, 0b11110, 0b10001, 0b10001, 0b01110],
        b'7' => [0b11111, 0b00001, 0b00010, 0b00010, 0b00100, 0b00100, 0b00100],
        b'8' => [0b01110, 0b10001, 0b10001, 0b01110, 0b10001, 0b10001, 0b01110],
        b'9' => [0b01110, 0b10001, 0b10001, 0b01111, 0b00001, 0b00010, 0b01100],
        b'A' => [0b00100, 0b01010, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001],
        b'B' => [0b11110, 0b10001, 0b10001, 0b11110, 0b10001, 0b10001, 0b11110],
        b'C' => [0b01111, 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b01111],
        b'D' => [0b11110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b11110],
        b'E' => [0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b11111],
        b'F' => [0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b10000],
        b'G' => [0b01111, 0b10000, 0b10000, 0b10011, 0b10001, 0b10001, 0b01111],
        b'H' => [0b10001, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001],
        b'I' => [0b01110, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110],
        b'J' => [0b00111, 0b00010, 0b00010, 0b00010, 0b00010, 0b10010, 0b01100],
        b'K' => [0b10001, 0b10010, 0b10100, 0b11000, 0b10100, 0b10010, 0b10001],
        b'L' => [0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b11111],
        b'M' => [0b10001, 0b11011, 0b10101, 0b10101, 0b10001, 0b10001, 0b10001],
        b'N' => [0b10001, 0b11001, 0b11001, 0b10101, 0b10011, 0b10011, 0b10001],
        b'O' => [0b01110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110],
        b'P' => [0b11110, 0b10001, 0b10001, 0b11110, 0b10000, 0b10000, 0b10000],
        b'Q' => [0b01110, 0b10001, 0b10001, 0b10001, 0b10101, 0b10010, 0b01101],
        b'R' => [0b11110, 0b10001, 0b10001, 0b11110, 0b10100, 0b10010, 0b10001],
        b'S' => [0b01111, 0b10000, 0b10000, 0b01110, 0b00001, 0b00001, 0b11110],
        b'T' => [0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100],
        b'U' => [0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110],
        b'V' => [0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01010, 0b00100],
        b'W' => [0b10001, 0b10001, 0b10001, 0b10101, 0b10101, 0b11011, 0b10001],
        b'X' => [0b10001, 0b10001, 0b01010, 0b00100, 0b01010, 0b10001, 0b10001],
        b'Y' => [0b10001, 0b10001, 0b01010, 0b00100, 0b00100, 0b00100, 0b00100],
        b'Z' => [0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b10000, 0b11111],
        b'.' => [0, 0, 0, 0, 0, 0b01100, 0b01100],
        b',' => [0, 0, 0, 0, 0b01100, 0b00100, 0b01000],
        b':' => [0, 0b01100, 0b01100, 0, 0b01100, 0b01100, 0],
        b'/' => [0b00001, 0b00010, 0b00010, 0b00100, 0b01000, 0b01000, 0b10000],
        b'-' => [0, 0, 0, 0b11111, 0, 0, 0],
        b'+' => [0, 0b00100, 0b00100, 0b11111, 0b00100, 0b00100, 0],
        b'<' => [0b00010, 0b00100, 0b01000, 0b10000, 0b01000, 0b00100, 0b00010],
        b'>' => [0b01000, 0b00100, 0b00010, 0b00001, 0b00010, 0b00100, 0b01000],
        b'(' => [0b00010, 0b00100, 0b01000, 0b01000, 0b01000, 0b00100, 0b00010],
        b')' => [0b01000, 0b00100, 0b00010, 0b00010, 0b00010, 0b00100, 0b01000],
        b'%' => [0b11001, 0b11010, 0b00010, 0b00100, 0b01000, 0b01011, 0b10011],
        b'\'' => [0b00100, 0b00100, 0b01000, 0, 0, 0, 0],
        b'!' => [0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0, 0b00100],
        b'?' => [0b01110, 0b10001, 0b00001, 0b00110, 0b00100, 0, 0b00100],
        b'_' => [0, 0, 0, 0, 0, 0, 0b11111],
        b'=' => [0, 0, 0b11111, 0, 0b11111, 0, 0],
        b'"' => [0b01010, 0b01010, 0, 0, 0, 0, 0],
        b' ' => [0; 7],
        _ => [0b11111, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b11111],
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn dots_cover_ascii_and_measure() {
        let a = Dots.atlas(Face::Body, 20);
        assert_eq!(a.glyphs.len(), 95);
        assert_eq!(a.measure("AB"), 2 * 12);
        assert_eq!(a.height(), 20);
        // A character it has no glyph for is drawn as `?`.
        assert_eq!(
            a.glyph('\u{e9}').unwrap().alpha,
            a.glyph('?').unwrap().alpha
        );
        // Bold is wider.
        assert!(Dots.atlas(Face::Bold, 20).measure("A") > a.measure("A"));
    }
}
