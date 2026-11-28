//! Paints the menu on the CPU over a made-up backdrop and saves a PNG:
//! `cargo run -p leonida-menu --example preview -- <out.png> [height] [page]`
//! (`page`: `main`, `player`, `display`).

use leonida_menu::model::Input;

fn main() {
    let mut args = std::env::args().skip(1);
    let out = args.next().unwrap_or_else(|| "menu-preview.png".into());
    let height: u32 = args.next().and_then(|h| h.parse().ok()).unwrap_or(1080);
    let page = args.next().unwrap_or_else(|| "main".into());
    let inputs: &[Input] = match page.as_str() {
        "player" => &[Input::Select, Input::Down, Input::Down],
        "display" => &[Input::Up, Input::Select, Input::Select, Input::Right],
        _ => &[Input::Down],
    };
    let layers = leonida_menu::preview(inputs, leonida_menu::scale_for(height));
    let width = height * 16 / 9;
    // A sky-to-ground backdrop for the menu's translucent rows.
    let mut rgb = Vec::with_capacity((width * height * 3) as usize);
    for y in 0..height {
        for x in 0..width {
            let t = y as f32 / height as f32;
            let u = x as f32 / width as f32;
            let c = if t < 0.55 {
                [
                    (90.0 + 80.0 * t) as u8,
                    (150.0 + 60.0 * t) as u8,
                    (220.0 - 30.0 * u) as u8,
                ]
            } else {
                [(70.0 + 40.0 * u) as u8, (90.0 + 20.0 * t) as u8, 60]
            };
            rgb.extend(c);
        }
    }
    let menu = layers.over([0, 0, 0, 255]);
    let (left, top) = (
        (leonida_menu::paint::LEFT * layers.scale) as u32,
        (leonida_menu::paint::TOP * layers.scale) as u32,
    );
    // Lay the premultiplied band picture over the backdrop.
    let band = layers.band_top()..layers.band_top() + layers.row_height;
    for y in 0..layers.height {
        let layer = u32::from(band.contains(&y));
        for x in 0..layers.width {
            let p = &layers.pixels
                [((y * 2 * layers.width + layer * layers.width + x) * 4) as usize..][..4];
            let at = (((top + y) * width + left + x) * 3) as usize;
            for c in 0..3 {
                let under = u32::from(rgb[at + c]);
                rgb[at + c] =
                    (u32::from(p[c]) + under * (255 - u32::from(p[3])) / 255).min(255) as u8;
            }
        }
    }
    let _ = menu;
    std::fs::write(
        &out,
        leonida_menu::png::encode(width, height, 3, width as usize * 3, &rgb),
    )
    .expect("written");
    println!("{out}");
}
