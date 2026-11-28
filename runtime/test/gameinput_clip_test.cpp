// CursorClipGate (shims/gameinput_clip.h): when the keyboard pad clips the cursor to the game window's client area.
// The clip must stay for clicks in the client area, and must let go while the window is dragged or sized, or a press
// on its title bar or border is held. No window, no SDL.
#include "gameinput_clip.h"

#include <cstdio>

namespace {
bool check(bool condition, const char* description) {
    if (!condition) std::fprintf(stderr, "FAIL: %s\n", description);
    return condition;
}
}

int main() {
    using rt::gameinput::detail::CursorClipGate;
    bool ok = true;
    constexpr bool want = true, modal = true, down = true, client = true;

    {
        CursorClipGate g;
        ok &= check(g.update(want, !modal, !down, client), "focused, no button: clip");
        ok &= check(!g.update(!want, !modal, !down, client), "not wanted: no clip");
        ok &= check(!g.update(!want, !modal, down, client), "not wanted, button held: no clip");
    }
    {
        // A click in the client area keeps the clip for as long as it is held, so it cannot land on another window.
        CursorClipGate g;
        ok &= check(g.update(want, !modal, !down, client), "before a client click: clip");
        ok &= check(g.update(want, !modal, down, client), "client press: clip");
        ok &= check(g.update(want, !modal, down, !client), "client press dragged to the edge: still clipped");
        ok &= check(g.update(want, !modal, !down, client), "after release: clip");
    }
    {
        // The user's report: grabbing the title bar of the focused window. The press starts outside the client
        // area, then Windows enters the move loop; the clip is let go for both and comes back after the release.
        CursorClipGate g;
        ok &= check(g.update(want, !modal, !down, !client), "cursor over the title bar, no button: clip");
        ok &= check(!g.update(want, !modal, down, !client), "title-bar press: no clip");
        ok &= check(!g.update(want, modal, down, !client), "move loop: no clip");
        ok &= check(!g.update(want, modal, down, client), "move loop over the client area: still no clip");
        ok &= check(!g.update(want, !modal, down, client), "move loop over, press still held: no clip");
        ok &= check(g.update(want, !modal, !down, client), "released: clip again");
    }
    {
        // A click on the title bar of the unfocused window: the button is already down when the clip is wanted.
        CursorClipGate g;
        ok &= check(!g.update(!want, !modal, down, !client), "unfocused press: no clip");
        ok &= check(!g.update(want, !modal, down, !client), "focus gained by a title-bar press: no clip");
        ok &= check(!g.update(want, !modal, down, client), "that press dragged over the client area: no clip");
        ok &= check(g.update(want, !modal, !down, client), "released: clip");
        // The same with focus gained by a client-area press: clipped at once.
        ok &= check(!g.update(!want, !modal, down, client), "unfocused client press: no clip");
        ok &= check(g.update(want, !modal, down, client), "focus gained by a client press: clip");
    }
    {
        // A modal loop alone (keyboard move or size from the system menu, a menu): no clip, whatever the buttons.
        CursorClipGate g;
        ok &= check(!g.update(want, modal, !down, client), "keyboard move/size or menu loop: no clip");
        ok &= check(g.update(want, !modal, !down, client), "loop over: clip");
    }
    return ok ? 0 : 1;
}
