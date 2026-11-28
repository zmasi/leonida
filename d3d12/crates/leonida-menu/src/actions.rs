//! What the title's own scripts would have to do: god mode, wanted level,
//! vehicles, weapons, teleports, time and weather. A trainer for a RAGE
//! title does these by calling the script engine's natives; the runtime
//! cannot do that for this title yet (`docs/mod-menu.md` says what was
//! looked for in the executable and why it was not found). The menu's
//! items for them go through [`GameActions`], and the backend there is
//! today is [`NotYet`], which answers every one with the reason, except the
//! teleports: [`Player`] does those by writing the player's position into the
//! title's memory (`knobs::PlayerHook`), when the runtime found where it is.
//! With `LEONIDA_GAME_ACTIONS=1` the runtime also lends a `knobs::GameHook`:
//! wanted level, time of day, weather and the title's own cheats, run by the
//! title's own code on its own thread; [`Player`] asks it first.

use crate::knobs::{self, NO_PLAYER};
use crate::model::{Id, Value};

/// The features that need the title's scripts. Single player only: nothing
/// here touches other players, money or anything online.
pub trait GameActions: Send {
    /// `Err` with why the feature cannot work now.
    fn available(&self, id: Id) -> Result<(), String>;
    /// Its value now (a toggle's state, a number).
    fn value(&self, id: Id) -> Value;
    /// Does it: `Ok` with an optional line for the menu.
    fn apply(&mut self, id: Id, value: Value) -> Result<Option<String>, String>;
}

/// The reason given for every script feature today.
pub const NOT_YET: &str = "Not available yet: needs the title's script natives, which the runtime cannot call yet (docs/mod-menu.md).";

/// No script features.
pub struct NotYet;

impl GameActions for NotYet {
    fn available(&self, _: Id) -> Result<(), String> {
        Err(NOT_YET.into())
    }
    fn value(&self, _: Id) -> Value {
        Value::None
    }
    fn apply(&mut self, _: Id, _: Value) -> Result<Option<String>, String> {
        Err(NOT_YET.into())
    }
}

/// How long a teleport from the menu holds the player at its new place.
const HOLD_MS: u32 = 3000;
/// "Forward 10 m".
const HOP_M: f32 = 10.0;

/// The script features through [`NotYet`], and the teleports through the
/// runtime's player hook.
#[derive(Default)]
pub struct Player {
    saved: Option<[f32; 3]>,
}

fn hook() -> Result<&'static knobs::PlayerHook, String> {
    knobs::player().ok_or_else(|| NO_PLAYER.to_string())
}

fn at(p: [f32; 3]) -> String {
    format!("({:.1}, {:.1}, {:.1})", p[0], p[1], p[2])
}

impl GameActions for Player {
    fn available(&self, id: Id) -> Result<(), String> {
        if let Some(answer) = knobs::game().and_then(|game| (game.available)(id)) {
            return answer;
        }
        match id {
            "teleport.save" | "teleport.forward" => (hook()?.ready)(),
            "teleport.load" => {
                (hook()?.ready)()?;
                self.saved
                    .map(|_| ())
                    .ok_or_else(|| "Nothing saved yet: use Save position first.".into())
            }
            "teleport.vice" => {
                let hook = hook()?;
                (hook.ready)()?;
                (hook.place)("vice").map(|_| ()).ok_or_else(|| {
                    "No Vice City position: set LEONIDA_VICE_CITY=x,y,z (docs/mod-menu.md).".into()
                })
            }
            _ => NotYet.available(id),
        }
    }

    fn value(&self, id: Id) -> Value {
        knobs::game()
            .and_then(|game| (game.value)(id))
            .unwrap_or(Value::None)
    }

    fn apply(&mut self, id: Id, value: Value) -> Result<Option<String>, String> {
        self.available(id)?;
        if let Some(answer) = knobs::game().and_then(|game| (game.apply)(id, value)) {
            return answer;
        }
        let hook = hook()?;
        match id {
            "teleport.save" => {
                let here = (hook.position)()?;
                self.saved = Some(here);
                Ok(Some(format!("Saved {}.", at(here))))
            }
            "teleport.load" => {
                let to = self.saved.ok_or("Nothing saved yet.")?;
                (hook.teleport)(to, HOLD_MS).map(Some)
            }
            "teleport.forward" => {
                let here = (hook.position)()?;
                let [hx, hy] = (hook.heading)().ok_or("The player's heading is not known.")?;
                let to = [here[0] + hx * HOP_M, here[1] + hy * HOP_M, here[2]];
                (hook.teleport)(to, HOLD_MS / 3).map(Some)
            }
            "teleport.vice" => {
                let to = (hook.place)("vice").ok_or("No Vice City position.")?;
                (hook.teleport)(to, HOLD_MS * 2).map(Some)
            }
            _ => NotYet.apply(id, value),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A game hook that owns the wanted level and nothing else.
    fn available(id: &str) -> Option<Result<(), String>> {
        (id == "player.wanted").then_some(Ok(()))
    }
    fn value(id: &str) -> Option<Value> {
        (id == "player.wanted").then_some(Value::Number(2.0))
    }
    fn apply(id: &str, value: Value) -> Option<Result<Option<String>, String>> {
        (id == "player.wanted").then(|| Ok(Some(format!("asked for {value:?}"))))
    }

    #[test]
    fn the_game_hook_answers_its_items_and_the_rest_say_why_not() {
        let _one = knobs::TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        // Without a hook every script feature says why it is not there.
        let mut player = Player::default();
        if knobs::game().is_none() {
            assert_eq!(player.available("player.wanted"), Err(NOT_YET.into()));
            assert_eq!(player.value("player.wanted"), Value::None);
        }
        knobs::provide_game(knobs::GameHook {
            available,
            value,
            apply,
        });
        assert_eq!(player.available("player.wanted"), Ok(()));
        assert_eq!(player.value("player.wanted"), Value::Number(2.0));
        assert_eq!(
            player.apply("player.wanted", Value::Number(3.0)),
            Ok(Some("asked for Number(3.0)".into()))
        );
        // What the hook does not own goes on as before.
        assert_eq!(player.available("player.god"), Err(NOT_YET.into()));
        assert_eq!(player.value("player.god"), Value::None);
        assert!(player.apply("player.god", Value::Bool(true)).is_err());
        assert!(player.available("teleport.save").is_err() || knobs::player().is_some());
    }
}
