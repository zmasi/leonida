//! The menu itself: pages of items, the cursor and its back stack, and what
//! a key does to them. No drawing and no platform: a [`Backend`] holds the
//! values the items show and does what they ask, and [`Menu::view`] says
//! what is to be drawn.

/// An item's name, by which the backend knows it.
pub type Id = &'static str;

/// What an item is.
#[derive(Clone, Debug, PartialEq)]
pub enum Kind {
    /// Opens the page of that index.
    Submenu(usize),
    /// On or off; Enter flips it.
    Toggle,
    /// A number between `min` and `max`, moved by `step` with Left and Right.
    Number {
        min: f64,
        max: f64,
        step: f64,
        decimals: u8,
        suffix: &'static str,
    },
    /// One of these, cycled with Left and Right (round).
    Choice(&'static [&'static str]),
    /// Does something once, on Enter.
    Action,
}

/// One line of a page.
#[derive(Clone, Debug, PartialEq)]
pub struct Item {
    pub id: Id,
    pub label: &'static str,
    /// The line under the menu while the item is selected.
    pub help: &'static str,
    pub kind: Kind,
}

impl Item {
    pub const fn new(id: Id, label: &'static str, help: &'static str, kind: Kind) -> Item {
        Item {
            id,
            label,
            help,
            kind,
        }
    }
}

/// A page: its caption (in the bar under the banner) and its items.
#[derive(Clone, Debug, PartialEq)]
pub struct Page {
    pub title: &'static str,
    pub items: Vec<Item>,
}

/// An item's value.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Value {
    Bool(bool),
    Number(f64),
    Index(usize),
    /// An action's (or a submenu's): nothing.
    None,
}

/// Holds the values and does what the items ask.
pub trait Backend {
    /// The value item `id` shows now.
    fn value(&self, id: Id) -> Value;
    /// Item `id` was given `value` (a flipped toggle, a moved number, a
    /// chosen option, `Value::None` for an action). `Ok` with an optional
    /// line to show, or `Err` with why it did not happen.
    fn set(&mut self, id: Id, value: Value) -> Result<Option<String>, String>;
    /// `Err` with the reason when item `id` cannot work in this build or run
    /// (it is shown dimmed, and selecting it shows the reason).
    fn available(&self, id: Id) -> Result<(), String>;
}

/// What the player asked for.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Input {
    /// Open or close the menu.
    Toggle,
    Up,
    Down,
    Left,
    Right,
    Select,
    Back,
}

/// Where the cursor is on one page of the stack.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct Cursor {
    page: usize,
    selected: usize,
    /// The first item shown.
    scroll: usize,
}

/// What a row shows on its right.
#[derive(Clone, Debug, PartialEq)]
pub enum Right {
    Nothing,
    /// `>`: the item opens a page.
    Submenu,
    /// A box, filled when on.
    Toggle(bool),
    /// A number or an option, between `<` and `>` while selected.
    Value(String),
}

/// One row as drawn.
#[derive(Clone, Debug, PartialEq)]
pub struct Row {
    pub label: String,
    pub right: Right,
    /// Dimmed when not.
    pub enabled: bool,
}

/// Everything there is to draw.
#[derive(Clone, Debug, PartialEq)]
pub struct View {
    /// The caption on the left of the bar under the banner.
    pub caption: String,
    /// `selected / count` on its right.
    pub counter: String,
    /// The rows shown (at most [`Menu::visible`]).
    pub rows: Vec<Row>,
    /// Which of them is selected.
    pub highlight: usize,
    pub more_above: bool,
    pub more_below: bool,
    /// The line under the rows: the item's help, the backend's answer, or
    /// why the item is not available.
    pub footer: String,
    /// The footer is a refusal.
    pub footer_warns: bool,
    /// How deep the cursor is (0: the first page).
    pub depth: usize,
    /// The page shown and its first row: when either changes the
    /// highlight jumps instead of sliding.
    pub page: usize,
    pub scroll: usize,
}

/// The menu: its pages and where the cursor is.
pub struct Menu {
    pages: Vec<Page>,
    stack: Vec<Cursor>,
    open: bool,
    /// Rows shown at once.
    visible: usize,
    /// The backend's last answer, until the cursor moves.
    message: Option<(String, bool)>,
}

impl Menu {
    /// A closed menu on `pages[0]`, showing `visible` rows at once.
    pub fn new(pages: Vec<Page>, visible: usize) -> Menu {
        assert!(!pages.is_empty(), "a menu has a first page");
        Menu {
            pages,
            stack: vec![Cursor {
                page: 0,
                selected: 0,
                scroll: 0,
            }],
            open: false,
            visible: visible.max(1),
            message: None,
        }
    }

    pub fn is_open(&self) -> bool {
        self.open
    }

    /// Replaces the pages (the Lua mods' pages come and go at a reload).
    /// Cursors on pages from `volatile` on are left (the stack pops back to
    /// the first page that is not one of those); the others stay where
    /// they were, kept inside their page.
    pub fn replace_pages(&mut self, pages: Vec<Page>, volatile: usize) {
        assert!(!pages.is_empty(), "a menu has a first page");
        self.pages = pages;
        while self.stack.len() > 1
            && self
                .stack
                .last()
                .is_some_and(|c| c.page >= volatile || c.page >= self.pages.len())
        {
            self.stack.pop();
        }
        for c in &mut self.stack {
            let count = self.pages.get(c.page).map_or(0, |p| p.items.len());
            c.selected = c.selected.min(count.saturating_sub(1));
            c.scroll = c.scroll.min(c.selected);
        }
        self.scroll_to_selection();
        self.message = None;
    }

    pub fn visible(&self) -> usize {
        self.visible
    }

    /// The page shown.
    pub fn page(&self) -> &Page {
        &self.pages[self.cursor().page]
    }

    /// The selected item, if the page has any.
    pub fn selected(&self) -> Option<&Item> {
        self.page().items.get(self.cursor().selected)
    }

    /// The pages open, the first page first.
    pub fn depth(&self) -> usize {
        self.stack.len() - 1
    }

    fn cursor(&self) -> &Cursor {
        self.stack.last().expect("the stack keeps its first page")
    }

    fn cursor_mut(&mut self) -> &mut Cursor {
        self.stack
            .last_mut()
            .expect("the stack keeps its first page")
    }

    /// Applies `input`; whether anything changed (a redraw is due).
    pub fn handle(&mut self, input: Input, backend: &mut dyn Backend) -> bool {
        if input == Input::Toggle {
            self.open = !self.open;
            self.message = None;
            return true;
        }
        if !self.open {
            return false;
        }
        let count = self.page().items.len();
        match input {
            Input::Toggle => unreachable!(),
            Input::Up | Input::Down => {
                if count == 0 {
                    return false;
                }
                let c = self.cursor_mut();
                c.selected = match input {
                    Input::Up => (c.selected + count - 1) % count,
                    _ => (c.selected + 1) % count,
                };
                self.scroll_to_selection();
                self.message = None;
                true
            }
            Input::Left | Input::Right => {
                let Some(item) = self.selected().cloned() else {
                    return false;
                };
                let forward = input == Input::Right;
                let value = match (&item.kind, backend.value(item.id)) {
                    (Kind::Number { min, max, step, .. }, Value::Number(v)) => {
                        let v = if forward { v + step } else { v - step };
                        // Steps land on the grid, not on the sum's rounding error.
                        let v = (v / step).round() * step;
                        Value::Number(v.clamp(*min, *max))
                    }
                    (Kind::Choice(options), Value::Index(i)) if !options.is_empty() => {
                        let n = options.len();
                        Value::Index(if forward {
                            (i + 1) % n
                        } else {
                            (i + n - 1) % n
                        })
                    }
                    _ => return false,
                };
                if backend.value(item.id) == value {
                    return false;
                }
                self.apply(&item, value, backend);
                true
            }
            Input::Select => {
                let Some(item) = self.selected().cloned() else {
                    return false;
                };
                match item.kind {
                    Kind::Submenu(page) => {
                        if page < self.pages.len() {
                            self.stack.push(Cursor {
                                page,
                                selected: 0,
                                scroll: 0,
                            });
                            self.message = None;
                        }
                    }
                    Kind::Toggle => {
                        let on = matches!(backend.value(item.id), Value::Bool(true));
                        self.apply(&item, Value::Bool(!on), backend);
                    }
                    Kind::Action => self.apply(&item, Value::None, backend),
                    // Moved with Left and Right; Enter says how.
                    Kind::Number { .. } | Kind::Choice(_) => {
                        self.message = Some(("Use Left and Right to change it.".into(), false));
                    }
                }
                true
            }
            Input::Back => {
                if self.stack.len() > 1 {
                    self.stack.pop();
                } else {
                    self.open = false;
                }
                self.message = None;
                true
            }
        }
    }

    fn apply(&mut self, item: &Item, value: Value, backend: &mut dyn Backend) {
        if let Err(why) = backend.available(item.id) {
            self.message = Some((why, true));
            return;
        }
        self.message = match backend.set(item.id, value) {
            Ok(Some(line)) => Some((line, false)),
            Ok(None) => None,
            Err(why) => Some((why, true)),
        };
    }

    /// Keeps the selected item among the rows shown.
    fn scroll_to_selection(&mut self) {
        let visible = self.visible;
        let c = self.cursor_mut();
        if c.selected < c.scroll {
            c.scroll = c.selected;
        } else if c.selected >= c.scroll + visible {
            c.scroll = c.selected + 1 - visible;
        }
    }

    /// What to draw.
    pub fn view(&self, backend: &dyn Backend) -> View {
        let c = *self.cursor();
        let page = self.page();
        let count = page.items.len();
        let end = (c.scroll + self.visible).min(count);
        let rows = page.items[c.scroll.min(end)..end]
            .iter()
            .map(|item| Row {
                label: item.label.to_owned(),
                right: right(item, backend.value(item.id)),
                enabled: backend.available(item.id).is_ok(),
            })
            .collect();
        let (footer, footer_warns) = match (&self.message, self.selected()) {
            (Some((line, warns)), _) => (line.clone(), *warns),
            (None, Some(item)) => match backend.available(item.id) {
                Ok(()) => (item.help.to_owned(), false),
                Err(why) => (why, true),
            },
            (None, None) => (String::new(), false),
        };
        View {
            caption: page.title.to_uppercase(),
            counter: if count == 0 {
                "0 / 0".into()
            } else {
                format!("{} / {}", c.selected + 1, count)
            },
            rows,
            highlight: c.selected - c.scroll.min(c.selected),
            more_above: c.scroll > 0,
            more_below: end < count,
            footer,
            footer_warns,
            depth: self.depth(),
            page: c.page,
            scroll: c.scroll,
        }
    }
}

/// The right side of an item's row.
fn right(item: &Item, value: Value) -> Right {
    match (&item.kind, value) {
        (Kind::Submenu(_), _) => Right::Submenu,
        (Kind::Toggle, Value::Bool(on)) => Right::Toggle(on),
        (Kind::Toggle, _) => Right::Toggle(false),
        (
            Kind::Number {
                decimals, suffix, ..
            },
            Value::Number(v),
        ) => Right::Value(format!("{v:.prec$}{suffix}", prec = usize::from(*decimals))),
        (Kind::Choice(options), Value::Index(i)) => {
            Right::Value(options.get(i).copied().unwrap_or("?").to_owned())
        }
        // A value the backend does not have (a feature not there).
        (Kind::Number { .. }, _) => Right::Value("--".into()),
        (Kind::Choice(options), _) => {
            Right::Value(options.first().copied().unwrap_or("--").to_owned())
        }
        _ => Right::Nothing,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::HashMap;

    /// Values in a map; ids starting with `x` are not available.
    #[derive(Default)]
    struct Fake {
        values: HashMap<Id, Value>,
        done: Vec<(Id, Value)>,
    }

    impl Backend for Fake {
        fn value(&self, id: Id) -> Value {
            self.values.get(id).copied().unwrap_or(match id {
                "n" => Value::Number(1.0),
                "c" => Value::Index(0),
                "t" => Value::Bool(false),
                _ => Value::None,
            })
        }
        fn set(&mut self, id: Id, value: Value) -> Result<Option<String>, String> {
            self.done.push((id, value));
            self.values.insert(id, value);
            Ok(if id == "a" { Some("done".into()) } else { None })
        }
        fn available(&self, id: Id) -> Result<(), String> {
            if id.starts_with('x') {
                Err("not yet".into())
            } else {
                Ok(())
            }
        }
    }

    const NUMBER: Kind = Kind::Number {
        min: 0.0,
        max: 2.0,
        step: 0.5,
        decimals: 1,
        suffix: "x",
    };

    fn menu() -> Menu {
        let main = Page {
            title: "Main",
            items: vec![
                Item::new("sub", "Sub", "opens", Kind::Submenu(1)),
                Item::new("t", "Toggle", "flips", Kind::Toggle),
                Item::new("n", "Number", "moves", NUMBER),
                Item::new(
                    "c",
                    "Choice",
                    "cycles",
                    Kind::Choice(&["one", "two", "three"]),
                ),
                Item::new("a", "Action", "acts", Kind::Action),
                Item::new("xa", "Missing", "absent", Kind::Action),
            ],
        };
        let sub = Page {
            title: "Sub",
            items: (0..12)
                .map(|_| Item::new("s", "Row", "", Kind::Action))
                .collect(),
        };
        let mut m = Menu::new(vec![main, sub], 5);
        m.handle(Input::Toggle, &mut Fake::default());
        m
    }

    #[test]
    fn toggle_opens_and_closes_and_keys_do_nothing_while_closed() {
        let mut b = Fake::default();
        let mut m = Menu::new(
            vec![Page {
                title: "P",
                items: vec![Item::new("t", "T", "", Kind::Toggle)],
            }],
            3,
        );
        assert!(!m.is_open());
        assert!(!m.handle(Input::Select, &mut b));
        assert!(b.done.is_empty());
        assert!(m.handle(Input::Toggle, &mut b));
        assert!(m.is_open());
        assert!(m.handle(Input::Toggle, &mut b));
        assert!(!m.is_open());
    }

    #[test]
    fn up_and_down_wrap_around() {
        let (mut m, mut b) = (menu(), Fake::default());
        m.handle(Input::Up, &mut b);
        assert_eq!(m.selected().unwrap().id, "xa");
        m.handle(Input::Down, &mut b);
        assert_eq!(m.selected().unwrap().id, "sub");
        m.handle(Input::Down, &mut b);
        assert_eq!(m.selected().unwrap().id, "t");
        assert_eq!(m.view(&b).counter, "2 / 6");
    }

    #[test]
    fn submenus_push_and_back_pops_then_closes() {
        let (mut m, mut b) = (menu(), Fake::default());
        m.handle(Input::Down, &mut b);
        m.handle(Input::Up, &mut b);
        m.handle(Input::Select, &mut b);
        assert_eq!(m.page().title, "Sub");
        assert_eq!(m.depth(), 1);
        m.handle(Input::Down, &mut b);
        m.handle(Input::Back, &mut b);
        // The first page's cursor is where it was.
        assert_eq!((m.page().title, m.selected().unwrap().id), ("Main", "sub"));
        assert!(m.is_open());
        m.handle(Input::Back, &mut b);
        assert!(!m.is_open());
        // Opened again, where it was left.
        m.handle(Input::Toggle, &mut b);
        assert_eq!(m.page().title, "Main");
    }

    #[test]
    fn scrolling_keeps_the_selection_in_view() {
        let (mut m, mut b) = (menu(), Fake::default());
        m.handle(Input::Select, &mut b);
        let v = m.view(&b);
        assert_eq!(
            (v.rows.len(), v.highlight, v.more_above, v.more_below),
            (5, 0, false, true)
        );
        for _ in 0..6 {
            m.handle(Input::Down, &mut b);
        }
        let v = m.view(&b);
        assert_eq!(
            (v.scroll, v.highlight, v.more_above, v.more_below),
            (2, 4, true, true)
        );
        assert_eq!(v.counter, "7 / 12");
        // Up from the top wraps to the last row, scrolled to the end.
        for _ in 0..6 {
            m.handle(Input::Up, &mut b);
        }
        m.handle(Input::Up, &mut b);
        let v = m.view(&b);
        assert_eq!(
            (v.scroll, v.highlight, v.more_below, v.counter.as_str()),
            (7, 4, false, "12 / 12")
        );
    }

    #[test]
    fn toggles_flip_and_show_their_state() {
        let (mut m, mut b) = (menu(), Fake::default());
        m.handle(Input::Down, &mut b);
        assert_eq!(m.view(&b).rows[1].right, Right::Toggle(false));
        m.handle(Input::Select, &mut b);
        assert_eq!(b.done, [("t", Value::Bool(true))]);
        assert_eq!(m.view(&b).rows[1].right, Right::Toggle(true));
        m.handle(Input::Select, &mut b);
        assert_eq!(b.value("t"), Value::Bool(false));
    }

    #[test]
    fn numbers_step_and_stop_at_their_ends() {
        let (mut m, mut b) = (menu(), Fake::default());
        m.handle(Input::Down, &mut b);
        m.handle(Input::Down, &mut b);
        assert_eq!(m.view(&b).rows[2].right, Right::Value("1.0x".into()));
        m.handle(Input::Right, &mut b);
        m.handle(Input::Right, &mut b);
        assert_eq!(b.value("n"), Value::Number(2.0));
        // At the top nothing changes and nothing is sent.
        let sent = b.done.len();
        assert!(!m.handle(Input::Right, &mut b));
        assert_eq!(b.done.len(), sent);
        for _ in 0..10 {
            m.handle(Input::Left, &mut b);
        }
        assert_eq!(b.value("n"), Value::Number(0.0));
        // Enter on a number only says how to change it.
        m.handle(Input::Select, &mut b);
        assert!(m.view(&b).footer.contains("Left and Right"));
    }

    #[test]
    fn choices_cycle_round() {
        let (mut m, mut b) = (menu(), Fake::default());
        for _ in 0..3 {
            m.handle(Input::Down, &mut b);
        }
        m.handle(Input::Left, &mut b);
        assert_eq!(b.value("c"), Value::Index(2));
        assert_eq!(m.view(&b).rows[3].right, Right::Value("three".into()));
        m.handle(Input::Right, &mut b);
        m.handle(Input::Right, &mut b);
        assert_eq!(b.value("c"), Value::Index(1));
    }

    #[test]
    fn actions_answer_in_the_footer_until_the_cursor_moves() {
        let (mut m, mut b) = (menu(), Fake::default());
        for _ in 0..4 {
            m.handle(Input::Down, &mut b);
        }
        assert_eq!(m.view(&b).footer, "acts");
        m.handle(Input::Select, &mut b);
        assert_eq!(b.done, [("a", Value::None)]);
        assert_eq!(m.view(&b).footer, "done");
        m.handle(Input::Down, &mut b);
        let v = m.view(&b);
        // Not available: dimmed, and the footer says why.
        assert!(!v.rows[v.highlight].enabled);
        assert_eq!((v.footer.as_str(), v.footer_warns), ("not yet", true));
        m.handle(Input::Select, &mut b);
        assert_eq!(b.done.len(), 1, "an unavailable item is not run");
    }

    #[test]
    fn replaced_pages_pop_the_volatile_ones_and_keep_cursors_inside() {
        let (mut m, mut b) = (menu(), Fake::default());
        m.handle(Input::Select, &mut b);
        assert_eq!((m.depth(), m.page().title), (1, "Sub"));
        for _ in 0..9 {
            m.handle(Input::Down, &mut b);
        }
        // Page 1 is volatile and now shorter: back to the first page.
        let main = Page {
            title: "Main",
            items: vec![Item::new("sub", "Sub", "", Kind::Submenu(1))],
        };
        let sub = Page {
            title: "Sub",
            items: vec![Item::new("s", "Row", "", Kind::Action)],
        };
        m.replace_pages(vec![main.clone(), sub.clone()], 1);
        assert_eq!((m.depth(), m.page().title), (0, "Main"));
        assert_eq!(m.selected().unwrap().id, "sub");
        // A page that is not volatile stays, and the cursor is clamped.
        m.handle(Input::Select, &mut b);
        m.replace_pages(vec![main, sub], 5);
        assert_eq!((m.depth(), m.page().title), (1, "Sub"));
        // A page that is gone is popped whatever.
        let only = Page {
            title: "Only",
            items: vec![],
        };
        m.replace_pages(vec![only], 5);
        assert_eq!((m.depth(), m.page().title), (0, "Only"));
        assert!(m.selected().is_none());
        assert!(!m.handle(Input::Down, &mut b));
        assert_eq!(m.view(&b).counter, "0 / 0");
    }

    #[test]
    fn a_submenu_shows_a_chevron() {
        let (m, b) = (menu(), Fake::default());
        let v = m.view(&b);
        assert_eq!(v.rows[0].right, Right::Submenu);
        assert_eq!(v.caption, "MAIN");
        assert_eq!(v.rows[4].right, Right::Nothing);
    }
}
