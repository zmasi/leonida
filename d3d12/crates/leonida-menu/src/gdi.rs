//! Atlases from the fonts installed with Windows, rasterised by GDI at run
//! time (`GetGlyphOutlineW`, 65 levels of grey). Nothing of a font is in
//! the repository or the build: the faces are looked up by name on the
//! player's system, and GDI's font mapper substitutes one it has if a name
//! is missing.

use windows_sys::Win32::Graphics::Gdi::{
    ANTIALIASED_QUALITY, CLIP_DEFAULT_PRECIS, CreateCompatibleDC, CreateFontW, DEFAULT_CHARSET,
    DEFAULT_PITCH, DeleteDC, DeleteObject, FIXED, FW_BOLD, FW_NORMAL, FW_SEMIBOLD,
    GGO_GRAY8_BITMAP, GLYPHMETRICS, GetGlyphOutlineW, GetTextMetricsW, MAT2, OUT_TT_PRECIS,
    SelectObject, TEXTMETRICW,
};

use crate::font::{Atlas, Face, FontSource, Glyph};

/// GDI's error return.
const GDI_ERROR: u32 = u32::MAX;

/// The faces by name: Segoe Script for the banner, Segoe UI for the rows
/// (both installed with Windows 10 and 11).
pub struct Gdi;

fn wide(s: &str) -> Vec<u16> {
    s.encode_utf16().chain(Some(0)).collect()
}

impl FontSource for Gdi {
    fn atlas(&self, face: Face, px: u32) -> Atlas {
        let (name, weight) = match face {
            Face::Script => ("Segoe Script", FW_BOLD),
            Face::Body => ("Segoe UI", FW_NORMAL),
            Face::Bold => ("Segoe UI Semibold", FW_SEMIBOLD),
        };
        made(name, weight as i32, px).unwrap_or_else(|| crate::font::Dots.atlas(face, px))
    }
}

/// `name` at `px` pixels of em height, or `None` if GDI made nothing.
fn made(name: &str, weight: i32, px: u32) -> Option<Atlas> {
    let name = wide(name);
    let identity = MAT2 {
        eM11: FIXED { fract: 0, value: 1 },
        eM12: FIXED { fract: 0, value: 0 },
        eM21: FIXED { fract: 0, value: 0 },
        eM22: FIXED { fract: 0, value: 1 },
    };
    // SAFETY: plain GDI object creation and queries on a memory DC of our
    // own; every buffer is as large as GDI said; all objects are freed.
    unsafe {
        let dc = CreateCompatibleDC(std::ptr::null_mut());
        if dc.is_null() {
            return None;
        }
        // A negative height asks for the em (character) height.
        let font = CreateFontW(
            -(px as i32),
            0,
            0,
            0,
            weight,
            0,
            0,
            0,
            DEFAULT_CHARSET as u32,
            OUT_TT_PRECIS as u32,
            CLIP_DEFAULT_PRECIS as u32,
            ANTIALIASED_QUALITY as u32,
            DEFAULT_PITCH as u32,
            name.as_ptr(),
        );
        if font.is_null() {
            DeleteDC(dc);
            return None;
        }
        let old = SelectObject(dc, font);
        let mut metrics: TEXTMETRICW = std::mem::zeroed();
        GetTextMetricsW(dc, &mut metrics);
        let mut glyphs = Vec::with_capacity(95);
        for c in 32u32..=126 {
            let mut gm: GLYPHMETRICS = std::mem::zeroed();
            let size = GetGlyphOutlineW(
                dc,
                c,
                GGO_GRAY8_BITMAP,
                &mut gm,
                0,
                std::ptr::null_mut(),
                &identity,
            );
            if size == GDI_ERROR {
                glyphs.push(Glyph::default());
                continue;
            }
            let (w, h) = (gm.gmBlackBoxX, gm.gmBlackBoxY);
            let mut alpha = Vec::new();
            if size > 0 && w > 0 && h > 0 {
                let mut raw = vec![0u8; size as usize];
                GetGlyphOutlineW(
                    dc,
                    c,
                    GGO_GRAY8_BITMAP,
                    &mut gm,
                    size,
                    raw.as_mut_ptr().cast(),
                    &identity,
                );
                // Rows of one byte a pixel, 0 to 64, each padded to four bytes.
                let pitch = (w as usize).next_multiple_of(4);
                alpha = (0..h as usize)
                    .flat_map(|y| {
                        raw[y * pitch..y * pitch + w as usize]
                            .iter()
                            .map(|&v| (u32::from(v.min(64)) * 255 / 64) as u8)
                    })
                    .collect();
            }
            let blank = alpha.is_empty();
            glyphs.push(Glyph {
                width: if blank { 0 } else { w },
                height: if blank { 0 } else { h },
                left: gm.gmptGlyphOrigin.x,
                top: gm.gmptGlyphOrigin.y,
                advance: i32::from(gm.gmCellIncX),
                alpha,
            });
        }
        SelectObject(dc, old);
        DeleteObject(font);
        DeleteDC(dc);
        if glyphs.iter().all(|g| g.alpha.is_empty()) {
            return None;
        }
        Some(Atlas {
            glyphs,
            ascent: metrics.tmAscent,
            descent: metrics.tmDescent,
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn system_fonts_give_glyphs() {
        let a = Gdi.atlas(Face::Body, 22);
        assert_eq!(a.glyphs.len(), 95);
        let g = a.glyph('W').unwrap();
        assert!(g.width > 4 && g.height > 4 && g.alpha.contains(&255));
        assert!(a.measure("Leonida") > 40);
        assert!(a.glyph(' ').unwrap().advance > 0);
    }
}
