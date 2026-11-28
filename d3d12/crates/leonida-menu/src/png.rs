//! A PNG encoder for screenshots and previews: 8-bit RGB or RGBA rows,
//! deflated by `zlib-rs`.

/// A PNG of `width * height` pixels, `channels` bytes each (3: RGB, 4:
/// RGBA), rows `pitch` bytes apart in `pixels`.
pub fn encode(width: u32, height: u32, channels: u8, pitch: usize, pixels: &[u8]) -> Vec<u8> {
    assert!(channels == 3 || channels == 4);
    let row = width as usize * usize::from(channels);
    let mut raw = Vec::with_capacity((row + 1) * height as usize);
    for y in 0..height as usize {
        raw.push(0);
        raw.extend_from_slice(&pixels[y * pitch..y * pitch + row]);
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
    png.extend((data.len() as u32).to_be_bytes());
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
}
