//! A PNG encoder for screenshots and previews: 8-bit RGB or RGBA rows,
//! deflated by `zlib-rs`.

/// A PNG of `width * height` pixels, `channels` bytes each (3: RGB, 4:
/// RGBA), rows `pitch` bytes apart in `pixels`.
pub fn encode(width: u32, height: u32, channels: u8, pitch: usize, pixels: &[u8]) -> Vec<u8> {
    assert!(channels == 3 || channels == 4, "PNG channels must be 3 or 4");
    assert!(width > 0 && height > 0, "PNG dimensions must be non-zero");
    let w = width as usize;
    let h = height as usize;
    let row = w
        .checked_mul(usize::from(channels))
        .expect("PNG row byte length overflow");
    assert!(
        pitch >= row,
        "PNG row pitch ({pitch}) smaller than row width ({row})"
    );
    let raw_len = row
        .checked_add(1)
        .and_then(|stride| stride.checked_mul(h))
        .expect("PNG raw scanline buffer size overflow");
    let needed = (h - 1)
        .checked_mul(pitch)
        .and_then(|last| last.checked_add(row))
        .expect("PNG pixel buffer span overflow");
    assert!(
        pixels.len() >= needed,
        "PNG pixel buffer ({}) shorter than required span ({needed})",
        pixels.len()
    );
    let mut raw = Vec::with_capacity(raw_len);
    for y in 0..h {
        let at = y * pitch;
        raw.push(0);
        raw.extend_from_slice(&pixels[at..at + row]);
    }
    let mut png = b"\x89PNG\r\n\x1a\n".to_vec();
    let mut header = Vec::new();
    header.extend(width.to_be_bytes());
    header.extend(height.to_be_bytes());
    header.extend([8, if channels == 4 { 6 } else { 2 }, 0, 0, 0]);
    chunk(&mut png, b"IHDR", &header);
    let mut compressed = vec![0u8; zlib_rs::compress_bound(raw.len())];
    let (idat, _) = zlib_rs::compress_slice(&mut compressed, &raw, zlib_rs::DeflateConfig::new(6));
    chunk(&mut png, b"IDAT", idat);
    chunk(&mut png, b"IEND", &[]);
    png
}

fn chunk(png: &mut Vec<u8>, kind: &[u8; 4], data: &[u8]) {
    let len = u32::try_from(data.len()).expect("PNG chunk exceeds u32::MAX");
    png.extend(len.to_be_bytes());
    let start = png.len();
    png.extend(kind);
    png.extend(data);
    let crc = crc32(&png[start..]);
    png.extend(crc.to_be_bytes());
}

fn crc32(bytes: &[u8]) -> u32 {
    let mut crc = !0u32;
    for &b in bytes {
        crc ^= u32::from(b);
        for _ in 0..8 {
            crc = (crc >> 1) ^ (0xedb8_8320 & (crc & 1).wrapping_neg());
        }
    }
    !crc
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn crc_and_header() {
        assert_eq!(crc32(b"123456789"), 0xCBF4_3926);
        let png = encode(2, 1, 4, 8, &[1, 2, 3, 4, 5, 6, 7, 8]);
        assert_eq!(&png[..8], b"\x89PNG\r\n\x1a\n");
        assert_eq!(&png[12..16], b"IHDR");
        assert_eq!(png[25], 6);
        assert_eq!(&png[png.len() - 8..png.len() - 4], b"IEND");
    }

    #[test]
    #[should_panic(expected = "pitch")]
    fn rejects_pitch_smaller_than_row() {
        let _ = encode(2, 2, 4, 0, &[0u8; 16]);
    }

    #[test]
    #[should_panic(expected = "non-zero")]
    fn rejects_zero_dimensions() {
        let _ = encode(0, 1, 4, 0, &[]);
    }

    #[test]
    #[should_panic(expected = "overflow")]
    fn rejects_overflowing_span() {
        let _ = encode(1, 3, 4, (usize::MAX / 2) + 4, &[0u8; 16]);
    }
}
