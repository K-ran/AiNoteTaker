#pragma once
// Button state machine, pure C so it is host-tested (test/test_buttons.c).
// Call button_step() every poll period with the current key states.
#include <stdbool.h>

typedef enum { BTN_NONE = 0, BTN_TOGGLE_PAUSE = 1, BTN_OPEN_SETUP = 2, BTN_SHOW_STATUS = 4 } button_action_t;

typedef struct {
    int held1, held2, combo;
    bool combo_used;     // KEY 1 + KEY 3 seen since the keys were last all released
} button_state_t;

static inline int button_step(button_state_t *s, bool k1, bool k2, bool k3, int poll_ms, int setup_hold_ms)
{
    int act = BTN_NONE;
    if (k1 && k3) {
        s->combo_used = true;
        if (++s->combo * poll_ms == setup_hold_ms) act |= BTN_OPEN_SETUP;
    } else {
        s->combo = 0;
    }
    // KEY 1 short press = privacy pause, never when the press was part of the combo
    // (people rarely release two keys at the same instant).
    if (k1) {
        if (!s->combo_used) s->held1++;
    } else {
        if (s->held1 && !s->combo_used && s->held1 * poll_ms < 1500) act |= BTN_TOGGLE_PAUSE;
        s->held1 = 0;
    }
    if (!k1 && !k3) s->combo_used = false;   // only after the release above was handled

    if (k2) s->held2++;
    else {
        if (s->held2 && s->held2 * poll_ms < 1500) act |= BTN_SHOW_STATUS;
        s->held2 = 0;
    }
    return act;
}
