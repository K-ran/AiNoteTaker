#pragma once

#include <stdint.h>

typedef enum { LED_IDLE, LED_RECORDING, LED_UPLOADING } led_mode_t;

// Sets up the 7 on-board WS2812 LEDs and starts the animation task
// (the only code that drives the LEDs). Call once at boot.
void led_anim_start(void);

// Safe to call from any task.
void led_set_mode(led_mode_t mode);
void led_set_level(float level);                 // 0..1 mic level, used while recording
void led_flash(uint8_t r, uint8_t g, uint8_t b); // short fading flash over the current animation

// HTTP server for the idle colour. Call once the network is up.
//   GET /                   control page (colour wheel + brightness)
//   GET /led?state=on|off   idle glow on/off
//   GET /led?rgb=RRGGBB     idle colour
void led_web_start(void);
