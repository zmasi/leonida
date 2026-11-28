//! The menu as pixels, painted on the CPU when it changes. The look is the
//! classic single-player trainer's: a blue banner with the title in a
//! script face, a black bar with the page's caption and `n / N`, the rows
//! (dark with light text; the selected one light with dark text), a bar
//! with the scroll arrows and a box with the selected item's help.
//!
//! Two layers are painted side by side: every row as unselected, and every
//! row as selected. The overlay shows the second inside the highlight band
//! and the first elsewhere, so the band can slide from row to row on the
//! card without painting anything again.

use crate::font::{Atlas, Face, FontSource};
use crate::model::{Right, View};

/// Sizes at a 1080-line picture, in pixels.
pub const LEFT: f32 = 28.0;
pub const TOP: f32 = 28.0;
const WIDTH: f32 = 432.0;
const HEADER: f32 = 108.0;
const SUBTITLE: f32 = 34.0;
const ROW: f32 = 36.0;
const ARROWS: f32 = 28.0;
const GAP: f32 = 4.0;
const LINE: f32 = 3.0;
const PAD: f32 = 10.0;
const TITLE_PX: f32 = 56.0;
const BODY_PX: f32 = 20.0;
const CAPTION_PX: f32 = 18.0;
/// Lines of help at most.
const HELP_LINES: usize = 3;

/// Straight RGBA.
pub(crate) type Colour = [u8; 4];

const BANNER_TOP: Colour = [44, 112, 222, 255];
const BANNER_BOTTOM: Colour = [10, 38, 118, 255];
const BAR: Colour = [0, 0, 0, 240];
const CAPTION: Colour = [112, 192, 255, 255];
const WHITE: Colour = [255, 255, 255, 255];
const ROW_DARK: Colour = [14, 17, 24, 205];
const ROW_LIGHT: Colour = [238, 238, 238, 255];
const TEXT_DARK: Colour = [12, 12, 14, 255];
const DIM_ON_DARK: Colour = [128, 132, 140, 255];
const DIM_ON_LIGHT: Colour = [120, 120, 124, 255];
const TOGGLE_ON: Colour = [84, 200, 120, 255];
const HELP_BOX: Colour = [0, 0, 0, 205];
const WARN: Colour = [255, 192, 84, 255];

/// The faces at one scale.
pub struct Fonts {
    pub script: Atlas,
    pub body: Atlas,
    pub bold: Atlas,
    pub scale: f32,
}

impl Fonts {
    pub fn new(source: &dyn FontSource, scale: f32) -> Fonts {
        let px = |base: f32| (base * scale).round().max(6.0) as u32;
        Fonts {
            script: source.atlas(Face::Script, px(TITLE_PX)),
            body: source.atlas(Face::Body, px(BODY_PX)),
            bold: source.atlas(Face::Bold, px(CAPTION_PX)),
            scale,
        }
    }
}

/// The painted menu.
#[derive(Clone, Debug, Default)]
pub struct Layers {
    /// One layer's size.
    pub width: u32,
    pub height: u32,
    /// `height` rows of `2 * width` premultiplied RGBA pixels: the
    /// unselected layer, then the selected one.
    pub pixels: Vec<u8>,
    /// The rows' place: the first one's top, a row's height, how many.
    pub rows_top: u32,
    pub row_height: u32,
    pub rows: usize,
    /// The selected row among them.
    pub highlight: usize,
    /// Page and scroll of the view: when either changes the band jumps.
    pub page: usize,
    pub scroll: usize,
    pub depth: usize,
    /// The scale it was painted at.
    pub scale: f32,
}

impl Layers {
    /// Where the highlight band's top is.
    pub fn band_top(&self) -> u32 {
        self.rows_top + self.highlight as u32 * self.row_height
    }

    /// The final picture as straight RGBA over `under` (for previews and
    /// tests): the band on row `highlight`.
    pub fn over(&self, under: Colour) -> Vec<u8> {
        let (w, h) = (self.width as usize, self.height as usize);
        let band = self.band_top() as usize..(self.band_top() + self.row_height) as usize;
        let mut out = Vec::with_capacity(w * h * 4);
        for y in 0..h {
            let layer = usize::from(band.contains(&y) && self.rows > 0);
            for x in 0..w {
                let p = &self.pixels[(y * 2 * w + layer * w + x) * 4..][..4];
                let a = u32::from(p[3]);
                for c in 0..3 {
                    out.push(
                        (u32::from(p[c]) + u32::from(under[c]) * (255 - a) / 255).min(255) as u8,
                    );
                }
                out.push(255);
            }
        }
        out
    }
}

/// A premultiplied RGBA picture.
pub(crate) struct Canvas {
    pub(crate) w: i32,
    pub(crate) h: i32,
    pub(crate) px: Vec<u8>,
}

impl Canvas {
    pub(crate) fn new(w: u32, h: u32) -> Canvas {
        Canvas {
            w: w as i32,
            h: h as i32,
            px: vec![0; (w * h * 4) as usize],
        }
    }

    /// Lays `colour` with coverage `cover` (0 to 255) over pixel (x, y).
    fn blend(&mut self, x: i32, y: i32, colour: Colour, cover: u8) {
        if x < 0 || y < 0 || x >= self.w || y >= self.h || cover == 0 {
            return;
        }
        let a = u32::from(colour[3]) * u32::from(cover) / 255;
        let at = ((y * self.w + x) * 4) as usize;
        let p = &mut self.px[at..at + 4];
        for c in 0..3 {
            let src = u32::from(colour[c]) * a / 255;
            p[c] = (src + u32::from(p[c]) * (255 - a) / 255).min(255) as u8;
        }
        p[3] = (a + u32::from(p[3]) * (255 - a) / 255).min(255) as u8;
    }

    pub(crate) fn fill(&mut self, x0: i32, y0: i32, x1: i32, y1: i32, colour: Colour) {
        for y in y0.max(0)..y1.min(self.h) {
            for x in x0.max(0)..x1.min(self.w) {
                self.blend(x, y, colour, 255);
            }
        }
    }

    /// A box's edge, `t` pixels thick.
    fn outline(&mut self, x0: i32, y0: i32, x1: i32, y1: i32, t: i32, colour: Colour) {
        self.fill(x0, y0, x1, y0 + t, colour);
        self.fill(x0, y1 - t, x1, y1, colour);
        self.fill(x0, y0 + t, x0 + t, y1 - t, colour);
        self.fill(x1 - t, y0 + t, x1, y1 - t, colour);
    }

    /// The banner: a vertical gradient, lit from the upper left, with a
    /// faint diagonal grain.
    fn banner(&mut self, x0: i32, y0: i32, x1: i32, y1: i32) {
        let (w, h) = ((x1 - x0).max(1) as f32, (y1 - y0).max(1) as f32);
        for y in y0..y1 {
            let t = (y - y0) as f32 / h;
            for x in x0..x1 {
                let u = (x - x0) as f32 / w;
                let glow = (1.0 - ((u * 0.8).powi(2) + (t * 1.3).powi(2)).sqrt()).max(0.0) * 0.35;
                let grain = if (x - x0 + y - y0).rem_euclid(6) < 3 {
                    0.02
                } else {
                    0.0
                };
                let mut c = [0u8; 4];
                for i in 0..3 {
                    let base =
                        f32::from(BANNER_TOP[i]) * (1.0 - t) + f32::from(BANNER_BOTTOM[i]) * t;
                    c[i] = (base * (1.0 + glow + grain) + 255.0 * glow * 0.15).min(255.0) as u8;
                }
                c[3] = 255;
                self.blend(x, y, c, 255);
            }
        }
    }

    /// `text` with its pen at `x` on baseline `y`.
    pub(crate) fn text(&mut self, atlas: &Atlas, x: i32, y: i32, text: &str, colour: Colour) {
        let mut pen = x;
        for c in text.chars() {
            let Some(g) = atlas.glyph(c) else { continue };
            for gy in 0..g.height as i32 {
                for gx in 0..g.width as i32 {
                    let cover = g.alpha[(gy * g.width as i32 + gx) as usize];
                    self.blend(pen + g.left + gx, y - g.top + gy, colour, cover);
                }
            }
            pen += g.advance;
        }
    }

    /// A filled triangle pointing up or down, `size` wide, centred on (cx, cy).
    fn arrow(&mut self, cx: f32, cy: f32, size: f32, up: bool, colour: Colour) {
        let half = size / 2.0;
        let height = size * 0.55;
        let (top, bottom) = (cy - height / 2.0, cy + height / 2.0);
        for y in top.floor() as i32..bottom.ceil() as i32 {
            // How far down the triangle, 0 at its point.
            let from_point = if up {
                (y as f32 + 0.5 - top) / height
            } else {
                (bottom - y as f32 - 0.5) / height
            };
            let reach = half * from_point.clamp(0.0, 1.0);
            for x in (cx - half).floor() as i32..(cx + half).ceil() as i32 {
                let d = reach - (x as f32 + 0.5 - cx).abs();
                let cover = (d + 0.5).clamp(0.0, 1.0);
                self.blend(x, y, colour, (cover * 255.0) as u8);
            }
        }
    }
}

/// The baseline that centres `atlas`'s letters in a band from `top`, `height` high.
fn baseline(atlas: &Atlas, top: i32, height: i32) -> i32 {
    // Capitals are about seven tenths of the ascent (the rest is the
    // accents' room above them): centred on those.
    let cap = (atlas.ascent as f32 * 0.72) as i32;
    top + (height + cap) / 2
}

/// `text` broken into lines no wider than `width`.
fn wrap(atlas: &Atlas, text: &str, width: i32) -> Vec<String> {
    let mut lines = Vec::new();
    let mut line = String::new();
    for word in text.split_whitespace() {
        let candidate = if line.is_empty() {
            word.to_owned()
        } else {
            format!("{line} {word}")
        };
        if atlas.measure(&candidate) > width && !line.is_empty() {
            lines.push(std::mem::replace(&mut line, word.to_owned()));
        } else {
            line = candidate;
        }
    }
    if !line.is_empty() {
        lines.push(line);
    }
    lines
}

/// Paints `view` at `fonts.scale` (1: a 1080-line picture).
pub fn paint(view: &View, fonts: &Fonts) -> Layers {
    let s = fonts.scale;
    let px = |v: f32| (v * s).round() as i32;
    let (width, header, subtitle, row, arrows, gap, line, pad) = (
        px(WIDTH),
        px(HEADER),
        px(SUBTITLE),
        px(ROW),
        px(ARROWS),
        px(GAP),
        px(LINE).max(1),
        px(PAD),
    );
    let help_lines = {
        let lines = wrap(&fonts.body, &view.footer, width - 2 * pad);
        lines.into_iter().take(HELP_LINES).collect::<Vec<_>>()
    };
    let help_line = fonts.body.height().max(1);
    let help_height = if help_lines.is_empty() {
        0
    } else {
        line + pad + help_lines.len() as i32 * help_line + pad / 2
    };
    let rows_top = header + subtitle;
    let rows_bottom = rows_top + row * view.rows.len() as i32;
    let height = rows_bottom
        + arrows
        + if help_height > 0 {
            gap + help_height
        } else {
            0
        };

    let mut layers: [Canvas; 2] = [
        Canvas::new(width as u32, height as u32),
        Canvas::new(width as u32, height as u32),
    ];
    for (selected, c) in layers.iter_mut().enumerate() {
        let selected = selected == 1;
        c.banner(0, 0, width, header);
        // A darker line under the banner.
        c.fill(0, header - px(2.0).max(1), width, header, [0, 0, 0, 90]);
        let title = "Leonida";
        let tw = fonts.script.measure(title);
        let ty = baseline(&fonts.script, 0, header) + px(2.0);
        let shadow = px(2.0).max(1);
        c.text(
            &fonts.script,
            (width - tw) / 2 + shadow,
            ty + shadow,
            title,
            [0, 0, 20, 140],
        );
        c.text(&fonts.script, (width - tw) / 2, ty, title, WHITE);

        c.fill(0, header, width, rows_top, BAR);
        let by = baseline(&fonts.bold, header, subtitle);
        c.text(&fonts.bold, pad, by, &view.caption, CAPTION);
        let cw = fonts.bold.measure(&view.counter);
        c.text(&fonts.bold, width - pad - cw, by, &view.counter, WHITE);

        for (i, r) in view.rows.iter().enumerate() {
            let top = rows_top + i as i32 * row;
            let (bg, fg, dim) = if selected {
                (ROW_LIGHT, TEXT_DARK, DIM_ON_LIGHT)
            } else {
                (ROW_DARK, WHITE, DIM_ON_DARK)
            };
            let fg = if r.enabled { fg } else { dim };
            c.fill(0, top, width, top + row, bg);
            let y = baseline(&fonts.body, top, row);
            c.text(&fonts.body, pad, y, &r.label, fg);
            let right = width - pad;
            match &r.right {
                Right::Nothing => {}
                Right::Submenu => {
                    let w = fonts.body.measure(">");
                    c.text(&fonts.body, right - w, y, ">", fg);
                }
                Right::Toggle(on) => {
                    let size = px(16.0);
                    let (x1, y0) = (right, top + (row - size) / 2);
                    c.outline(x1 - size, y0, x1, y0 + size, px(2.0).max(1), fg);
                    if *on {
                        let inset = px(4.0).max(2);
                        let fill = if r.enabled { TOGGLE_ON } else { dim };
                        c.fill(
                            x1 - size + inset,
                            y0 + inset,
                            x1 - inset,
                            y0 + size - inset,
                            fill,
                        );
                    }
                }
                Right::Value(v) => {
                    // `< value >` while selected, the value alone otherwise.
                    let text = if selected {
                        format!("< {v} >")
                    } else {
                        v.clone()
                    };
                    let w = fonts.body.measure(&text);
                    c.text(&fonts.body, right - w, y, &text, fg);
                }
            }
        }

        // The bar with the scroll arrows, brighter where there is more.
        c.fill(0, rows_bottom, width, rows_bottom + arrows, BAR);
        let size = px(12.0) as f32;
        let mid = (width / 2) as f32;
        let ay = rows_bottom as f32 + arrows as f32 / 2.0;
        let lit = |more: bool| if more { WHITE } else { [150, 150, 150, 255] };
        c.arrow(mid, ay - size * 0.36, size, true, lit(view.more_above));
        c.arrow(mid, ay + size * 0.36, size, false, lit(view.more_below));

        if help_height > 0 {
            let top = rows_bottom + arrows + gap;
            c.fill(0, top, width, top + line, BANNER_TOP);
            c.fill(0, top + line, width, top + help_height, HELP_BOX);
            let colour = if view.footer_warns { WARN } else { WHITE };
            for (i, text) in help_lines.iter().enumerate() {
                let y = top + line + pad / 2 + i as i32 * help_line;
                c.text(
                    &fonts.body,
                    pad,
                    baseline(&fonts.body, y, help_line),
                    text,
                    colour,
                );
            }
        }
    }

    let [normal, chosen] = layers;
    let row_bytes = width as usize * 4;
    let mut pixels = Vec::with_capacity(row_bytes * 2 * height as usize);
    for y in 0..height as usize {
        pixels.extend_from_slice(&normal.px[y * row_bytes..][..row_bytes]);
        pixels.extend_from_slice(&chosen.px[y * row_bytes..][..row_bytes]);
    }
    Layers {
        width: width as u32,
        height: height as u32,
        pixels,
        rows_top: rows_top as u32,
        row_height: row as u32,
        rows: view.rows.len(),
        highlight: view.highlight,
        page: view.page,
        scroll: view.scroll,
        depth: view.depth,
        scale: s,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::font::Dots;
    use crate::model::Row;

    fn view() -> View {
        View {
            caption: "MAIN MENU".into(),
            counter: "2 / 3".into(),
            rows: vec![
                Row {
                    label: "Player".into(),
                    right: Right::Submenu,
                    enabled: true,
                },
                Row {
                    label: "God mode".into(),
                    right: Right::Toggle(true),
                    enabled: false,
                },
                Row {
                    label: "Speed".into(),
                    right: Right::Value("1.0x".into()),
                    enabled: true,
                },
            ],
            highlight: 1,
            more_above: false,
            more_below: false,
            footer: "Some help that is long enough to be wrapped onto a second line of the box"
                .into(),
            footer_warns: false,
            depth: 0,
            page: 0,
            scroll: 0,
        }
    }

    fn at(l: &Layers, layer: usize, x: u32, y: u32) -> [u8; 4] {
        let i = ((y * 2 * l.width + layer as u32 * l.width + x) * 4) as usize;
        l.pixels[i..i + 4].try_into().unwrap()
    }

    #[test]
    fn layout_scales_with_the_picture() {
        let v = view();
        let one = paint(&v, &Fonts::new(&Dots, 1.0));
        let two = paint(&v, &Fonts::new(&Dots, 2.0));
        assert_eq!(
            (one.width, one.rows_top, one.row_height, one.rows),
            (432, 142, 36, 3)
        );
        assert_eq!((two.width, two.rows_top, two.row_height), (864, 284, 72));
        assert_eq!(one.pixels.len(), (one.width * 2 * one.height * 4) as usize);
        assert_eq!(one.band_top(), 142 + 36);
    }

    #[test]
    fn rows_are_dark_unselected_and_light_selected() {
        let l = paint(&view(), &Fonts::new(&Dots, 1.0));
        // Near the right edge of the first row, where there is no text.
        let (x, y) = (200, l.rows_top + 4);
        let dark = at(&l, 0, x, y);
        let light = at(&l, 1, x, y);
        assert!(dark[0] < 40 && dark[3] > 150 && dark[3] < 255, "{dark:?}");
        assert!(light[0] > 200 && light[3] == 255, "{light:?}");
        // The banner and the bar are the same in both layers.
        assert_eq!(at(&l, 0, 5, 5), at(&l, 1, 5, 5));
        assert_eq!(at(&l, 0, 5, l.rows_top - 3), [0, 0, 0, 240]);
        // The banner is blue.
        let b = at(&l, 0, 400, 100);
        assert!(b[2] > b[0] + 40, "{b:?}");
    }

    #[test]
    fn the_preview_shows_the_band_on_the_selected_row() {
        let l = paint(&view(), &Fonts::new(&Dots, 1.0));
        let rgb = l.over([0, 0, 0, 255]);
        let pixel = |x: u32, y: u32| rgb[((y * l.width + x) * 4) as usize];
        assert!(pixel(200, l.band_top() + 2) > 200);
        assert!(pixel(200, l.rows_top + 2) < 40);
    }

    #[test]
    fn help_wraps_into_lines() {
        let f = Fonts::new(&Dots, 1.0);
        let lines = wrap(&f.body, &view().footer, 400);
        assert!(lines.len() >= 2);
        assert!(lines.iter().all(|l| f.body.measure(l) <= 400));
    }
}
