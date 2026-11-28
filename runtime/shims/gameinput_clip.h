// When the keyboard pad may clip the cursor to the game window's client area (gameinput.cpp, clip_cursor). Pure, so it
// is tested without a window: gameinput_clip_test.
#pragma once

namespace rt::gameinput::detail {

// The clip keeps a click in the client area from landing on another window. It must not hold the cursor while the
// player drags or sizes the window: the clip is to the client area, so a press on the title bar or a border would be
// pulled into it, and each re-clip during the modal move loop pulls the cursor (and with it the window) down again.
struct CursorClipGate {
    bool was_down = false;         // a mouse button was down at the last update
    bool nonclient_press = false;  // the press still held began outside the client area

    // want: the clip is wanted at all (a focused window of ours, raw input driving the pad). modal_loop: the window's
    // thread is in a move/size or menu loop (GUI_INMOVESIZE, GUI_INMENUMODE, GUI_SYSTEMMENUMODE). button_down: a
    // mouse button is held. in_client: the cursor is inside the client area. True: clip now; false: release any clip.
    bool update(bool want, bool modal_loop, bool button_down, bool in_client) {
        if (!want) {
            // Forget the button too: a press already held when the clip is wanted again (a click on the title bar
            // that activates the window) counts as starting where the cursor is then.
            *this = {};
            return false;
        }
        if (!button_down)
            nonclient_press = false;
        else if (!was_down)
            nonclient_press = !in_client;
        was_down = button_down;
        return !modal_loop && !nonclient_press;
    }
};

}  // namespace rt::gameinput::detail
