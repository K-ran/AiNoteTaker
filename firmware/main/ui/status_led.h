#pragma once
// Status light. Each module owns its own flag(s); the LED task shows the most
// important active state: setup > problem (any cause) > paused > recording.
#include <stdbool.h>

typedef enum {
    LED_SETUP,
    LED_PAUSED,
    // problem causes: any of these shows the red "needs attention" blink
    LED_PROBLEM_SD,
    LED_PROBLEM_ENCODER,
    LED_PROBLEM_UPLOAD,
    LED_FLAG_COUNT
} led_flag_t;

void status_led_start(void);
void status_led_set(led_flag_t flag, bool on);
bool status_led_any_problem(void);
void status_led_show_status(bool healthy);   // KEY 2: 3 s green (healthy) or red
