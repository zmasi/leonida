//! The "Mods" page of the trainer menu: pages and items the Lua mods
//! (`crates/leonida-lua`) add, and what the menu tells them back.
//!
//! The mods' thread publishes the pages ([`set_pages`]); the menu's thread
//! shows them under Main menu, Mods, keeps the values the player sets, and
//! queues what changed ([`take_events`]) for the mods' thread to call the
//! mods' functions with: the menu never runs a mod's code.

use std::collections::HashSet;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Mutex, OnceLock};

use crate::model::{Id, Item, Kind, Page, Value};

/// Pages in all, items on a page and the longest label kept.
pub const MAX_PAGES: usize = 64;
pub const MAX_ITEMS: usize = 64;
pub const MAX_LABEL: usize = 48;
/// Events kept for the mods' thread; the oldest go first.
const MAX_EVENTS: usize = 256;

/// What an item is and where its value starts.
#[derive(Clone, Debug, PartialEq)]
pub enum ModKind {
    Toggle(bool),
    Number {
        value: f64,
        min: f64,
        max: f64,
        step: f64,
        decimals: u8,
        suffix: String,
    },
    Action,
}

#[derive(Clone, Debug, PartialEq)]
pub struct ModItem {
    pub label: String,
    pub help: String,
    pub kind: ModKind,
}

#[derive(Clone, Debug, PartialEq)]
pub struct ModPage {
    pub title: String,
    pub items: Vec<ModItem>,
}

/// What the menu did to an item of a mod's page.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Event {
    /// Indexes into the pages last given to [`set_pages`].
    pub page: usize,
    pub item: usize,
    /// A toggle's new state, a number's new value, or `Value::None` (an action).
    pub value: Value,
}

#[derive(Default)]
struct State {
    pages: Vec<ModPage>,
    values: Vec<Vec<Value>>,
    events: std::collections::VecDeque<Event>,
}

static STATE: Mutex<State> = Mutex::new(State {
    pages: Vec::new(),
    values: Vec::new(),
    events: std::collections::VecDeque::new(),
});
static GENERATION: AtomicU64 = AtomicU64::new(0);
static ENABLED: AtomicBool = AtomicBool::new(false);
static RELOAD: AtomicBool = AtomicBool::new(false);

/// The Lua mods are running: the trainer menu has a Mods page.
pub fn enable() {
    ENABLED.store(true, Ordering::Release);
    GENERATION.fetch_add(1, Ordering::AcqRel);
}

pub fn enabled() -> bool {
    ENABLED.load(Ordering::Acquire)
}

/// Changes whenever the pages do (and when the Mods page appears).
pub fn generation() -> u64 {
    GENERATION.load(Ordering::Acquire)
}

/// Replaces the mods' pages (limits applied: too many pages or items are
/// dropped, labels cut). The values are the items' own; events pending for
/// the old pages are dropped.
pub fn set_pages(mut pages: Vec<ModPage>) {
    pages.truncate(MAX_PAGES);
    for page in &mut pages {
        page.items.truncate(MAX_ITEMS);
        page.title = cut(&page.title);
        for item in &mut page.items {
            item.label = cut(&item.label);
        }
    }
    let values = pages
        .iter()
        .map(|p| {
            p.items
                .iter()
                .map(|i| match i.kind {
                    ModKind::Toggle(on) => Value::Bool(on),
                    ModKind::Number { value, .. } => Value::Number(value),
                    ModKind::Action => Value::None,
                })
                .collect()
        })
        .collect();
    let mut state = STATE.lock().unwrap_or_else(|e| e.into_inner());
    *state = State {
        pages,
        values,
        events: Default::default(),
    };
    GENERATION.fetch_add(1, Ordering::AcqRel);
}

fn cut(text: &str) -> String {
    text.chars()
        .filter(|c| !c.is_control())
        .take(MAX_LABEL)
        .collect()
}

/// Sets an item's value from the mods' side (no event is queued).
pub fn set_value(page: usize, item: usize, value: Value) -> bool {
    let mut state = STATE.lock().unwrap_or_else(|e| e.into_inner());
    match state.values.get_mut(page).and_then(|p| p.get_mut(item)) {
        Some(slot) if !matches!(slot, Value::None) && !matches!(value, Value::None) => {
            *slot = value;
            true
        }
        _ => false,
    }
}

/// An item's value now.
pub fn value_of(page: usize, item: usize) -> Option<Value> {
    let state = STATE.lock().unwrap_or_else(|e| e.into_inner());
    state.values.get(page)?.get(item).copied()
}

/// What the menu did since the last call.
pub fn take_events() -> Vec<Event> {
    let mut state = STATE.lock().unwrap_or_else(|e| e.into_inner());
    state.events.drain(..).collect()
}

/// The "Reload Lua mods" item was chosen since the last call.
pub fn take_reload() -> bool {
    RELOAD.swap(false, Ordering::AcqRel)
}

/// Asks for a reload as the menu's item does.
pub fn request_reload() {
    RELOAD.store(true, Ordering::Release);
}

/// A label or id kept for the menu's life: items name themselves with
/// `&'static str`, and the same few strings come back at every reload.
fn intern(text: &str) -> &'static str {
    static SEEN: OnceLock<Mutex<HashSet<&'static str>>> = OnceLock::new();
    let mut seen = SEEN
        .get_or_init(Default::default)
        .lock()
        .unwrap_or_else(|e| e.into_inner());
    if let Some(&known) = seen.get(text) {
        return known;
    }
    let leaked: &'static str = Box::leak(text.to_owned().into_boxed_str());
    seen.insert(leaked);
    leaked
}

/// The id of item `item` of page `page`.
fn id_of(page: usize, item: usize) -> Id {
    intern(&format!("mod.{page}.{item}"))
}

/// `(page, item)` of an id made by [`id_of`].
fn parse(id: &str) -> Option<(usize, usize)> {
    let rest = id.strip_prefix("mod.")?;
    let (page, item) = rest.split_once('.')?;
    Some((page.parse().ok()?, item.parse().ok()?))
}

/// The id of the "Reload Lua mods" item.
pub const RELOAD_ID: Id = "mod.reload";

/// The Mods page, then the mods' pages, to follow `first` pages already in
/// the menu: the Mods page is page `first`, the mods' pages `first + 1`...
pub fn pages(first: usize) -> Vec<Page> {
    let state = STATE.lock().unwrap_or_else(|e| e.into_inner());
    let mut list = vec![Item::new(
        RELOAD_ID,
        "Reload Lua mods",
        "Stops the Lua mods and loads them again from the mods folder.",
        Kind::Action,
    )];
    for (n, page) in state.pages.iter().enumerate() {
        list.push(Item::new(
            intern(&format!("go.mod.{n}")),
            intern(&page.title),
            "A page of a Lua mod.",
            Kind::Submenu(first + 1 + n),
        ));
    }
    let mut out = vec![Page {
        title: "Mods",
        items: list,
    }];
    for (p, page) in state.pages.iter().enumerate() {
        let items = page
            .items
            .iter()
            .enumerate()
            .map(|(i, item)| {
                let kind = match &item.kind {
                    ModKind::Toggle(_) => Kind::Toggle,
                    ModKind::Action => Kind::Action,
                    ModKind::Number {
                        min,
                        max,
                        step,
                        decimals,
                        suffix,
                        ..
                    } => Kind::Number {
                        min: *min,
                        max: *max,
                        step: *step,
                        decimals: *decimals,
                        suffix: intern(suffix),
                    },
                };
                Item::new(id_of(p, i), intern(&item.label), intern(&item.help), kind)
            })
            .collect();
        out.push(Page {
            title: intern(&page.title),
            items,
        });
    }
    out
}

/// The value item `id` shows.
pub fn value(id: &str) -> Value {
    match parse(id) {
        Some((p, i)) => value_of(p, i).unwrap_or(Value::None),
        None => Value::None,
    }
}

/// The menu set item `id` (a toggle flipped, a number moved, an action run).
pub fn apply(id: &str, value: Value) -> Result<Option<String>, String> {
    if id == RELOAD_ID {
        request_reload();
        return Ok(Some("Reloading the Lua mods.".into()));
    }
    let (p, i) = parse(id).ok_or_else(|| "Not a mod's item.".to_string())?;
    let mut state = STATE.lock().unwrap_or_else(|e| e.into_inner());
    let slot = state
        .values
        .get_mut(p)
        .and_then(|page| page.get_mut(i))
        .ok_or_else(|| "That mod item is gone (the mods were reloaded).".to_string())?;
    if !matches!(slot, Value::None) {
        *slot = value;
    }
    if state.events.len() >= MAX_EVENTS {
        state.events.pop_front();
    }
    state.events.push_back(Event {
        page: p,
        item: i,
        value,
    });
    Ok(None)
}

/// The ids' page and item counts, for a test.
#[cfg(test)]
pub(crate) fn counts() -> std::collections::HashMap<usize, usize> {
    let state = STATE.lock().unwrap_or_else(|e| e.into_inner());
    state
        .pages
        .iter()
        .enumerate()
        .map(|(i, p)| (i, p.items.len()))
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::knobs::TEST_LOCK;

    fn sample() -> Vec<ModPage> {
        vec![
            ModPage {
                title: "Example".into(),
                items: vec![
                    ModItem {
                        label: "Show it".into(),
                        help: "".into(),
                        kind: ModKind::Toggle(true),
                    },
                    ModItem {
                        label: "Amount".into(),
                        help: "How much.".into(),
                        kind: ModKind::Number {
                            value: 2.0,
                            min: 0.0,
                            max: 10.0,
                            step: 0.5,
                            decimals: 1,
                            suffix: "x".into(),
                        },
                    },
                    ModItem {
                        label: "Do it".into(),
                        help: "".into(),
                        kind: ModKind::Action,
                    },
                ],
            },
            ModPage {
                title: "Second".into(),
                items: vec![],
            },
        ]
    }

    #[test]
    fn pages_follow_the_menus_own_and_carry_their_initial_values() {
        let _one = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let before = generation();
        set_pages(sample());
        assert!(generation() > before);
        let pages = pages(13);
        // The Mods page, then one page for each of the mods'.
        assert_eq!(pages.len(), 3);
        assert_eq!(pages[0].title, "Mods");
        let labels: Vec<_> = pages[0].items.iter().map(|i| i.label).collect();
        assert_eq!(labels, ["Reload Lua mods", "Example", "Second"]);
        assert_eq!(pages[0].items[1].kind, Kind::Submenu(14));
        assert_eq!(pages[0].items[2].kind, Kind::Submenu(15));
        let ids: Vec<_> = pages[1].items.iter().map(|i| i.id).collect();
        assert_eq!(ids, ["mod.0.0", "mod.0.1", "mod.0.2"]);
        assert_eq!(value("mod.0.0"), Value::Bool(true));
        assert_eq!(value("mod.0.1"), Value::Number(2.0));
        assert_eq!(value("mod.0.2"), Value::None);
        assert_eq!(value("mod.9.9"), Value::None);
        assert_eq!(counts()[&0], 3);
    }

    #[test]
    fn what_the_menu_does_is_queued_for_the_mods_and_the_value_kept() {
        let _one = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        set_pages(sample());
        assert_eq!(apply("mod.0.0", Value::Bool(false)), Ok(None));
        assert_eq!(apply("mod.0.1", Value::Number(3.5)), Ok(None));
        assert_eq!(apply("mod.0.2", Value::None), Ok(None));
        assert_eq!(value("mod.0.0"), Value::Bool(false));
        assert_eq!(value("mod.0.1"), Value::Number(3.5));
        let events = take_events();
        assert_eq!(events.len(), 3);
        assert_eq!(
            events[1],
            Event {
                page: 0,
                item: 1,
                value: Value::Number(3.5)
            }
        );
        assert!(take_events().is_empty());
        // A mod sets a value itself: no event.
        assert!(set_value(0, 0, Value::Bool(true)));
        assert!(!set_value(0, 2, Value::Bool(true)), "an action has no value");
        assert!(!set_value(5, 0, Value::Bool(true)));
        assert!(take_events().is_empty());
        // After a reload the old ids are gone.
        set_pages(vec![]);
        assert!(apply("mod.0.0", Value::Bool(true)).is_err());
        assert!(apply("nonsense", Value::None).is_err());
    }

    #[test]
    fn reload_is_asked_once() {
        let _one = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        take_reload();
        assert!(apply(RELOAD_ID, Value::None).unwrap().is_some());
        assert!(take_reload());
        assert!(!take_reload());
    }

    #[test]
    fn limits_and_labels_are_kept() {
        let _one = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let long = "x".repeat(200);
        let item = |label: &str| ModItem {
            label: label.into(),
            help: String::new(),
            kind: ModKind::Action,
        };
        let pages: Vec<_> = (0..100)
            .map(|_| ModPage {
                title: long.clone(),
                items: (0..100).map(|_| item("a\nb")).collect(),
            })
            .collect();
        set_pages(pages);
        let state = STATE.lock().unwrap();
        assert_eq!(state.pages.len(), MAX_PAGES);
        assert_eq!(state.pages[0].items.len(), MAX_ITEMS);
        assert_eq!(state.pages[0].title.len(), MAX_LABEL);
        assert_eq!(state.pages[0].items[0].label, "ab");
        drop(state);
        set_pages(vec![]);
    }

    #[test]
    fn the_same_strings_are_interned_once() {
        assert!(std::ptr::eq(intern("Same"), intern("Same")));
        assert_eq!(parse("mod.3.12"), Some((3, 12)));
        assert_eq!(parse("mod.x.1"), None);
        assert_eq!(parse("go.mods"), None);
    }
}
