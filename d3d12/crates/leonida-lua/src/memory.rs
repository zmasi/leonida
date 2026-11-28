//! Checked access to the title's memory for `leonida.memory`, and the byte
//! pattern scan.
//!
//! The runtime says how memory is read and written ([`Host`]): the renderer
//! lends the copies it already uses for the title's memory, which report an
//! unmapped or protected page instead of faulting. Nothing here dereferences
//! an address itself.

use std::sync::OnceLock;

/// How the title's memory is read and written; both say false where a page
/// of the range is not mapped (or, for a write, not writable).
#[derive(Clone, Copy)]
pub struct Host {
    pub read: fn(address: usize, out: &mut [u8]) -> bool,
    pub write: fn(address: usize, data: &[u8]) -> bool,
}

static HOST: OnceLock<Host> = OnceLock::new();
static IMAGE: OnceLock<(usize, usize)> = OnceLock::new();

/// Tells the mods how the title's memory is read and written (once).
pub fn set_host(host: Host) {
    let _ = HOST.set(host);
}

/// Tells the mods where the title's executable image is mapped (once).
pub fn set_image(base: usize, size: usize) {
    let _ = IMAGE.set((base, size));
}

pub(crate) fn host() -> Option<Host> {
    HOST.get().copied()
}

pub(crate) fn image() -> Option<(usize, usize)> {
    IMAGE.get().copied()
}

/// The lowest address a mod may touch (the first 64 KiB are never mapped).
pub const LOWEST: usize = 0x1_0000;
/// One past the highest user-mode address of a 64-bit process.
pub const HIGHEST: usize = 0x8000_0000_0000;
/// The most bytes one read or write moves.
pub const MAX_BYTES: usize = 1 << 20;
/// Matches a scan returns at most.
pub const MAX_MATCHES: usize = 10_000;

/// Whether `[address, address + len)` is a range a mod may ask about: inside
/// user space, not wrapped, not empty, no larger than [`MAX_BYTES`].
pub fn valid_range(address: i64, len: usize) -> Option<usize> {
    let address = usize::try_from(address).ok()?;
    let end = address.checked_add(len)?;
    (len > 0 && len <= MAX_BYTES && address >= LOWEST && end <= HIGHEST).then_some(address)
}

/// A byte pattern with wildcards: `48 8B ?? 05`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Pattern {
    bytes: Vec<u8>,
    /// Whether the byte there counts.
    mask: Vec<bool>,
    /// The first byte that counts.
    anchor: usize,
}

impl Pattern {
    /// Parses hex byte pairs, each a pair of digits or `?`/`??`, spaces
    /// between them optional: `"48 8B ?? 05"`, `"488B??05"`. It must hold at
    /// least one byte that counts.
    pub fn parse(text: &str) -> Result<Pattern, String> {
        let mut bytes = Vec::new();
        let mut mask = Vec::new();
        let flat: Vec<char> = text.chars().filter(|c| !c.is_whitespace()).collect();
        // `?` alone stands for a byte when spaces separate it; without
        // spaces a byte is two characters.
        let spaced = text.split_whitespace().count() > 1
            || text.split_whitespace().next().is_some_and(|t| t.len() <= 2);
        if spaced {
            for token in text.split_whitespace() {
                match token {
                    "?" | "??" => {
                        bytes.push(0);
                        mask.push(false);
                    }
                    t if t.len() == 2 && t.bytes().all(|b| b.is_ascii_hexdigit()) => {
                        bytes.push(u8::from_str_radix(t, 16).map_err(|e| e.to_string())?);
                        mask.push(true);
                    }
                    t => return Err(format!("`{t}` is not a byte (two hex digits, or ??)")),
                }
            }
        } else {
            if flat.len() % 2 != 0 {
                return Err("an odd number of digits".into());
            }
            for pair in flat.chunks(2) {
                match (pair[0], pair[1]) {
                    ('?', '?') => {
                        bytes.push(0);
                        mask.push(false);
                    }
                    (a, b) if a.is_ascii_hexdigit() && b.is_ascii_hexdigit() => {
                        bytes.push(
                            u8::from_str_radix(&format!("{a}{b}"), 16)
                                .map_err(|e| e.to_string())?,
                        );
                        mask.push(true);
                    }
                    (a, b) => {
                        return Err(format!("`{a}{b}` is not a byte (two hex digits, or ??)"));
                    }
                }
            }
        }
        let anchor = mask
            .iter()
            .position(|&m| m)
            .ok_or("a pattern needs at least one byte that is not ??")?;
        Ok(Pattern {
            bytes,
            mask,
            anchor,
        })
    }

    pub fn len(&self) -> usize {
        self.bytes.len()
    }

    pub fn is_empty(&self) -> bool {
        self.bytes.is_empty()
    }

    /// Offsets in `hay` where the pattern matches.
    pub fn find_in<'a>(&'a self, hay: &'a [u8]) -> impl Iterator<Item = usize> + 'a {
        let n = self.bytes.len();
        let first = self.bytes[self.anchor];
        let last = hay.len().saturating_sub(n.saturating_sub(1));
        let mut at = self.anchor;
        std::iter::from_fn(move || {
            loop {
                // The next place the first byte that counts is, as long as
                // the whole pattern still fits from there.
                let found = hay.get(at..)?.iter().position(|&b| b == first)? + at;
                at = found + 1;
                let start = found.checked_sub(self.anchor)?;
                if start >= last {
                    return None;
                }
                if self
                    .bytes
                    .iter()
                    .zip(&self.mask)
                    .zip(&hay[start..start + n])
                    .all(|((&p, &m), &h)| !m || p == h)
                {
                    return Some(start);
                }
            }
        })
    }
}

/// How much is read in one go during a scan.
const CHUNK: usize = 1 << 20;
const PAGE: usize = 4096;

/// The stretches of `[at, at + out.len())` that `read` can read, as
/// (offset, length), read into `out`: the whole range if it can, else by
/// pages.
fn readable(
    read: &dyn Fn(usize, &mut [u8]) -> bool,
    at: usize,
    out: &mut [u8],
) -> Vec<(usize, usize)> {
    if read(at, out) {
        return vec![(0, out.len())];
    }
    let mut runs: Vec<(usize, usize)> = Vec::new();
    let mut off = 0;
    while off < out.len() {
        let piece = (PAGE - (at + off) % PAGE).min(out.len() - off);
        if read(at + off, &mut out[off..off + piece]) {
            match runs.last_mut() {
                Some((from, len)) if *from + *len == off => *len += piece,
                _ => runs.push((off, piece)),
            }
        }
        off += piece;
    }
    runs
}

/// Scans `[start, start + size)` for `pattern` through `read`, which says
/// false for a stretch it cannot read: such a stretch is read by pages and
/// the pages that are not there skipped (a match never spans a gap). At
/// most `max` addresses, in order.
pub fn scan(
    read: &dyn Fn(usize, &mut [u8]) -> bool,
    start: usize,
    size: usize,
    pattern: &Pattern,
    max: usize,
) -> Vec<usize> {
    let mut found = Vec::new();
    let n = pattern.len();
    let end = start.saturating_add(size);
    let mut buf = vec![0u8; CHUNK];
    // The last n-1 bytes read, which end at `tail_end`: a match can span two stretches.
    let mut tail: Vec<u8> = Vec::new();
    let mut tail_end = start;
    let mut at = start;
    while at < end && found.len() < max {
        let len = CHUNK.min(end - at);
        for (off, run) in readable(read, at, &mut buf[..len]) {
            let from = at + off;
            let joined = tail_end == from && !tail.is_empty();
            let lead = if joined { tail.len() } else { 0 };
            let mut data = Vec::with_capacity(lead + run);
            if joined {
                data.extend_from_slice(&tail);
            }
            data.extend_from_slice(&buf[off..off + run]);
            let base = from - lead;
            for hit in pattern.find_in(&data) {
                if found.len() < max {
                    found.push(base + hit);
                }
            }
            let keep = (n - 1).min(data.len());
            tail = data[data.len() - keep..].to_vec();
            tail_end = from + run;
        }
        at += len;
    }
    found
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn patterns_parse_with_wildcards() {
        let p = Pattern::parse("48 8B ?? 05").unwrap();
        assert_eq!((p.len(), p.anchor), (4, 0));
        assert_eq!(Pattern::parse("488b??05").unwrap(), p);
        assert_eq!(Pattern::parse("? ? 41 ?").unwrap().anchor, 2);
        assert_eq!(Pattern::parse("41").unwrap().len(), 1);
        for bad in ["", "?? ??", "4", "GG", "48 8", "48 8BB", "4?", "123"] {
            assert!(Pattern::parse(bad).is_err(), "{bad:?}");
        }
    }

    #[test]
    fn matching_honours_wildcards_and_the_end_of_the_data() {
        let p = Pattern::parse("48 ?? 05").unwrap();
        let hay = [0u8, 0x48, 0x11, 0x05, 0x48, 0x05, 0x05, 0x48, 0x22, 0x05];
        assert_eq!(p.find_in(&hay).collect::<Vec<_>>(), [1, 4, 7]);
        // A pattern longer than the data, or cut off at its end, matches nothing.
        let cut = [0x48u8, 0x11];
        assert_eq!(p.find_in(&cut).count(), 0);
        assert_eq!(p.find_in(&[]).count(), 0);
        // A leading wildcard.
        let q = Pattern::parse("?? 07").unwrap();
        assert_eq!(q.find_in(&[7u8, 7, 1]).collect::<Vec<_>>(), [0]);
        assert_eq!(q.find_in(&[1u8, 7]).collect::<Vec<_>>(), [0]);
        assert_eq!(q.find_in(&[7u8]).count(), 0);
    }

    #[test]
    fn ranges_are_checked() {
        assert_eq!(valid_range(0x1_4000_0000, 4), Some(0x1_4000_0000));
        assert_eq!(valid_range(0, 4), None);
        assert_eq!(valid_range(0xFFFF, 4), None);
        assert_eq!(valid_range(-1, 4), None);
        assert_eq!(valid_range(0x1_0000, 0), None);
        assert_eq!(valid_range(0x1_0000, MAX_BYTES + 1), None);
        assert_eq!(valid_range(HIGHEST as i64 - 2, 4), None);
        assert_eq!(valid_range(i64::MAX, 8), None);
    }

    /// Memory of ours with a hole: reads inside `[hole, hole + len)` fail.
    fn memory(
        data: Vec<u8>,
        base: usize,
        hole: Option<(usize, usize)>,
    ) -> impl Fn(usize, &mut [u8]) -> bool {
        move |at, out| {
            if at < base || at + out.len() > base + data.len() {
                return false;
            }
            if let Some((h, l)) = hole
                && at < h + l
                && at + out.len() > h
            {
                return false;
            }
            out.copy_from_slice(&data[at - base..at - base + out.len()]);
            true
        }
    }

    #[test]
    fn a_scan_finds_matches_across_chunks_and_in_order() {
        let mut data = vec![0u8; 3 * CHUNK + 100];
        let spots = [10, CHUNK - 2, CHUNK + 5, 2 * CHUNK - 1, 3 * CHUNK + 90];
        for &s in &spots {
            data[s..s + 4].copy_from_slice(&[0xDE, 0xAD, 0xBE, 0xEF]);
        }
        let base = 0x2_0000_0000;
        let read = memory(data.clone(), base, None);
        let p = Pattern::parse("DE AD ?? EF").unwrap();
        let got = scan(&read, base, data.len(), &p, 100);
        assert_eq!(got, spots.map(|s| base + s));
        // A limit.
        assert_eq!(scan(&read, base, data.len(), &p, 2).len(), 2);
        // A range that starts later.
        let got = scan(&read, base + CHUNK, 2 * CHUNK, &p, 100);
        assert_eq!(got, [base + CHUNK + 5, base + 2 * CHUNK - 1]);
    }

    #[test]
    fn a_scan_skips_pages_that_are_not_there() {
        let mut data = vec![0u8; 8 * PAGE];
        data[PAGE + 8..PAGE + 12].copy_from_slice(&[1, 2, 3, 4]);
        data[5 * PAGE + 8..5 * PAGE + 12].copy_from_slice(&[1, 2, 3, 4]);
        let base = 0x3_0000_0000;
        // The page of the first one is gone.
        let read = memory(data.clone(), base, Some((base + PAGE, PAGE)));
        let p = Pattern::parse("01 02 03 04").unwrap();
        assert_eq!(scan(&read, base, data.len(), &p, 10), [base + 5 * PAGE + 8]);
        // All of it gone: nothing, no panic.
        let read = memory(data.clone(), base, Some((base, 8 * PAGE)));
        assert!(scan(&read, base, data.len(), &p, 10).is_empty());
        // A range off the end is not read.
        let read = memory(data.clone(), base, None);
        assert!(scan(&read, base + 9 * PAGE, PAGE, &p, 10).is_empty());
    }
}
