//! The Lua mods' overlay: a list of texts and rectangles a mod draws each
//! frame (`crates/leonida-lua`), painted on the CPU as one picture the size of
//! the drawing's bounding box and laid over the presented frame by the
//! renderer in one blended quad (`leonida-gpu`, `host/trainer.rs`), where the
//! trainer menu is.
//!
//! Coordinates are pixels of a 1080-line picture and scale with the
//! picture's height, so a mod's layout is the same at every resolution.
//! A present pays one atomic load (`knobs::work() & LUA_DRAW`) while no mod
//! draws; the list is painted by the mod's own thread when it changes.

use std::collections::HashMap;
use std::sync::{Arc, Mutex};

use crate::app::{Frame, height, system_fonts};
use crate::font::{Atlas, Face, FontSource};
use crate::knobs;
use crate::paint::{Canvas, Colour, Layers};

/// Draw commands kept per frame at most; the rest are dropped.
pub const MAX_COMMANDS: usize = 1024;
/// The picture's largest size, in pixels (an 8-bit 4K picture and a bit more).
const MAX_W: i32 = 4096;
const MAX_H: i32 = 2304;
/// Text size when a mod gives none, in pixels of a 1080-line picture.
pub const DEFAULT_SIZE: f32 = 20.0;
const MIN_SIZE: f32 = 6.0;
const MAX_SIZE: f32 = 200.0;
/// Sizes kept as glyph atlases at once.
const ATLASES: usize = 12;

/// One thing to draw. Colours are straight RGBA.
#[derive(Clone, Debug, PartialEq)]
pub enum Cmd {
    /// `text` with its top-left corner at (`x`, `y`), `size` pixels tall (the em).
    Text {
        x: f32,
        y: f32,
        text: String,
        size: f32,
        colour: Colour,
    },
    Rect {
        x: f32,
        y: f32,
        w: f32,
        h: f32,
        colour: Colour,
    },
}

/// Paints draw lists; keeps the glyph atlases of the sizes it has met.
pub struct Painter {
    source: Box<dyn FontSource + Send>,
    atlases: HashMap<u32, Atlas>,
}

/// A painted list: its top-left corner on the picture and its pixels.
pub struct Painted {
    pub origin: (i32, i32),
    pub layers: Layers,
}

impl Painter {
    pub fn new(source: Box<dyn FontSource + Send>) -> Painter {
        Painter {
            source,
            atlases: HashMap::new(),
        }
    }

    fn atlas(&mut self, px: u32) -> &Atlas {
        if self.atlases.len() >= ATLASES && !self.atlases.contains_key(&px) {
            self.atlases.clear();
        }
        self.atlases
            .entry(px)
            .or_insert_with(|| self.source.atlas(Face::Body, px))
    }

    /// `cmds` painted for a picture whose scale against 1080 lines is
    /// `scale`; `None` when nothing is to be seen.
    pub fn paint(&mut self, cmds: &[Cmd], scale: f32) -> Option<Painted> {
        let scale = if scale.is_finite() {
            scale.clamp(0.25, 8.0)
        } else {
            1.0
        };
        let px_of = |size: f32| {
            let size = if size.is_finite() { size } else { DEFAULT_SIZE };
            (size.clamp(MIN_SIZE, MAX_SIZE) * scale).round().max(6.0) as u32
        };
        let int = |v: f32| (v * scale).round().clamp(-1e6, 1e6) as i32;
        // Boxes in picture pixels: (x0, y0, x1, y1) and the command's baseline.
        let mut boxes = Vec::with_capacity(cmds.len().min(MAX_COMMANDS));
        for cmd in cmds.iter().take(MAX_COMMANDS) {
            boxes.push(match cmd {
                Cmd::Text {
                    x,
                    y,
                    text,
                    size,
                    colour,
                } => {
                    if colour[3] == 0 || text.is_empty() {
                        continue;
                    }
                    let atlas = self.atlas(px_of(*size));
                    let (w, h, ascent) = (atlas.measure(text), atlas.height(), atlas.ascent);
                    let (x0, y0) = (int(*x), int(*y));
                    (x0, y0, x0 + w, y0 + h, ascent)
                }
                Cmd::Rect { x, y, w, h, colour } => {
                    if colour[3] == 0 || !(*w > 0.0 && *h > 0.0) {
                        continue;
                    }
                    let (x0, y0) = (int(*x), int(*y));
                    (x0, y0, x0 + int(*w).max(1), y0 + int(*h).max(1), 0)
                }
            });
        }
        // The union of what is drawn, kept on the picture (no negative
        // corner) and within a side's limit.
        let (mut x0, mut y0, mut x1, mut y1) = (i32::MAX, i32::MAX, i32::MIN, i32::MIN);
        for &(a, b, c, d, _) in &boxes {
            if c <= 0 || d <= 0 || a >= MAX_W || b >= MAX_H || c <= a || d <= b {
                continue;
            }
            x0 = x0.min(a.max(0));
            y0 = y0.min(b.max(0));
            x1 = x1.max(c.min(MAX_W));
            y1 = y1.max(d.min(MAX_H));
        }
        if x1 <= x0 || y1 <= y0 {
            return None;
        }
        let (w, h) = ((x1 - x0) as u32, (y1 - y0) as u32);
        let mut canvas = Canvas::new(w, h);
        let mut at = boxes.iter();
        for cmd in cmds.iter().take(MAX_COMMANDS) {
            // The same commands were skipped above, in the same order.
            match cmd {
                Cmd::Text {
                    text, size, colour, ..
                } => {
                    if colour[3] == 0 || text.is_empty() {
                        continue;
                    }
                    let Some(&(a, b, _, _, ascent)) = at.next() else {
                        break;
                    };
                    let atlas = self.atlas(px_of(*size));
                    canvas.text(atlas, a - x0, b - y0 + ascent, text, *colour);
                }
                Cmd::Rect { w, h, colour, .. } => {
                    if colour[3] == 0 || !(*w > 0.0 && *h > 0.0) {
                        continue;
                    }
                    let Some(&(a, b, c, d, _)) = at.next() else {
                        break;
                    };
                    canvas.fill(a - x0, b - y0, c - x0, d - y0, *colour);
                }
            }
        }
        // Two layers side by side (the renderer's quad reads the second
        // inside a highlight band; there is none here): both the same.
        let row = (w * 4) as usize;
        let mut pixels = Vec::with_capacity(2 * canvas.px.len());
        for line in canvas.px.chunks_exact(row) {
            pixels.extend_from_slice(line);
            pixels.extend_from_slice(line);
        }
        Some(Painted {
            origin: (x0, y0),
            layers: Layers {
                width: w,
                height: h,
                pixels,
                scale,
                ..Layers::default()
            },
        })
    }
}

/// The newest picture of the overlay and its number.
struct Current {
    version: u64,
    painted: Option<(Arc<Layers>, (i32, i32))>,
    /// What it was painted from: an unchanged list is not painted again.
    from: Option<(Vec<Cmd>, u32)>,
}

static CURRENT: Mutex<Current> = Mutex::new(Current {
    version: 0,
    painted: None,
    from: None,
});
static PAINTER: Mutex<Option<Painter>> = Mutex::new(None);

/// Sets what the overlay shows from the next present on: `cmds` (an empty
/// list: nothing). Called by the mods' thread once a frame, and only paints
/// when the list or the picture's height changed.
pub fn publish(cmds: &[Cmd]) {
    publish_with(cmds, height(), || system_fonts());
}

fn publish_with(cmds: &[Cmd], height: u32, source: impl FnOnce() -> Box<dyn FontSource + Send>) {
    let mut now = CURRENT.lock().unwrap_or_else(|e| e.into_inner());
    if cmds.is_empty() {
        if now.painted.take().is_some() || now.from.take().is_some() {
            now.version += 1;
        }
        knobs::set_work(knobs::LUA_DRAW, false);
        return;
    }
    let cmds = &cmds[..cmds.len().min(MAX_COMMANDS)];
    if now
        .from
        .as_ref()
        .is_some_and(|(c, h)| c == cmds && *h == height)
    {
        return;
    }
    let mut painter = PAINTER.lock().unwrap_or_else(|e| e.into_inner());
    let painter = painter.get_or_insert_with(|| Painter::new(source()));
    let painted = painter
        .paint(cmds, height.max(1) as f32 / 1080.0)
        .map(|p| (Arc::new(p.layers), p.origin));
    now.from = Some((cmds.to_vec(), height));
    now.version += 1;
    let seen = painted.is_some();
    now.painted = painted;
    knobs::set_work(knobs::LUA_DRAW, seen);
}

/// What a present draws for the mods, while `knobs::work()` says
/// [`knobs::LUA_DRAW`].
pub fn frame() -> Option<Frame> {
    let now = CURRENT.lock().unwrap_or_else(|e| e.into_inner());
    let (layers, origin) = now.painted.clone()?;
    Some(Frame {
        version: now.version,
        layers,
        origin: Some(origin),
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::font::Dots;

    fn rect(x: f32, y: f32, w: f32, h: f32) -> Cmd {
        Cmd::Rect {
            x,
            y,
            w,
            h,
            colour: [255, 0, 0, 255],
        }
    }

    fn pixel(p: &Painted, layer: usize, x: u32, y: u32) -> [u8; 4] {
        let w = p.layers.width as usize;
        let at = (y as usize * 2 * w + layer * w + x as usize) * 4;
        p.layers.pixels[at..at + 4].try_into().unwrap()
    }

    #[test]
    fn a_rectangle_is_painted_at_its_place_and_scaled_with_the_picture() {
        let mut painter = Painter::new(Box::new(Dots));
        let p = painter.paint(&[rect(10.0, 20.0, 30.0, 8.0)], 1.0).unwrap();
        assert_eq!((p.origin, p.layers.width, p.layers.height), ((10, 20), 30, 8));
        assert_eq!(pixel(&p, 0, 0, 0), [255, 0, 0, 255]);
        assert_eq!(pixel(&p, 1, 29, 7), [255, 0, 0, 255]);
        let p = painter.paint(&[rect(10.0, 20.0, 30.0, 8.0)], 2.0).unwrap();
        assert_eq!((p.origin, p.layers.width, p.layers.height), ((20, 40), 60, 16));
    }

    #[test]
    fn text_lands_inside_its_box_and_the_box_is_the_union() {
        let mut painter = Painter::new(Box::new(Dots));
        let text = Cmd::Text {
            x: 100.0,
            y: 50.0,
            text: "AB".into(),
            size: 20.0,
            colour: [255, 255, 255, 255],
        };
        let p = painter.paint(&[text.clone()], 1.0).unwrap();
        assert_eq!(p.origin, (100, 50));
        // Two 20-pixel dot letters, 12 wide each.
        assert_eq!((p.layers.width, p.layers.height), (24, 20));
        assert!(
            (0..p.layers.height).any(|y| (0..p.layers.width).any(|x| pixel(&p, 0, x, y)[3] != 0)),
            "some of the letters are drawn"
        );
        let both = painter
            .paint(&[text, rect(0.0, 0.0, 10.0, 10.0)], 1.0)
            .unwrap();
        assert_eq!(both.origin, (0, 0));
        assert_eq!((both.layers.width, both.layers.height), (124, 70));
    }

    #[test]
    fn nothing_visible_gives_nothing() {
        let mut painter = Painter::new(Box::new(Dots));
        assert!(painter.paint(&[], 1.0).is_none());
        assert!(painter.paint(&[rect(0.0, 0.0, 0.0, 5.0)], 1.0).is_none());
        let clear = Cmd::Rect {
            x: 0.0,
            y: 0.0,
            w: 5.0,
            h: 5.0,
            colour: [1, 2, 3, 0],
        };
        assert!(painter.paint(&[clear], 1.0).is_none());
        // Off the picture on the left or the top: clipped to nothing.
        assert!(painter.paint(&[rect(-20.0, 0.0, 10.0, 5.0)], 1.0).is_none());
        // Partly off: what is on it.
        let p = painter.paint(&[rect(-5.0, -5.0, 10.0, 10.0)], 1.0).unwrap();
        assert_eq!((p.origin, p.layers.width, p.layers.height), ((0, 0), 5, 5));
    }

    #[test]
    fn nonsense_numbers_do_not_panic() {
        let mut painter = Painter::new(Box::new(Dots));
        let odd = [
            rect(f32::NAN, 0.0, 5.0, 5.0),
            rect(0.0, f32::INFINITY, 5.0, 5.0),
            rect(0.0, 0.0, f32::INFINITY, 5.0),
            Cmd::Text {
                x: 1e30,
                y: 0.0,
                text: "x".into(),
                size: f32::NAN,
                colour: [255; 4],
            },
        ];
        let _ = painter.paint(&odd, f32::NAN);
        let _ = painter.paint(&odd, 1.0);
    }

    #[test]
    fn publishing_sets_the_work_bit_and_an_unchanged_list_is_not_painted_again() {
        let _one = knobs::TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        publish_with(&[], 1080, || Box::new(Dots));
        assert_eq!(knobs::work() & knobs::LUA_DRAW, 0);
        assert!(frame().is_none());
        let cmds = [rect(4.0, 4.0, 40.0, 10.0)];
        publish_with(&cmds, 1080, || Box::new(Dots));
        assert_eq!(knobs::work() & knobs::LUA_DRAW, knobs::LUA_DRAW);
        let first = frame().unwrap();
        assert_eq!(first.origin, Some((4, 4)));
        publish_with(&cmds, 1080, || Box::new(Dots));
        assert_eq!(frame().unwrap().version, first.version);
        publish_with(&cmds, 540, || Box::new(Dots));
        let second = frame().unwrap();
        assert!(second.version > first.version);
        assert_eq!(second.layers.width, 20);
        publish_with(&[], 1080, || Box::new(Dots));
        assert_eq!(knobs::work() & knobs::LUA_DRAW, 0);
        assert!(frame().is_none());
    }
}
