#pragma once
// Wi-Fi setup without an app: a hotspot "NoteTaker-XXXX" (password DEV_SETUP_CODE)
// serving a page at http://192.168.4.1 to enter store Wi-Fi, store, salesperson
// and (developer builds) the API token. Settings are saved only after the device
// has connected with them. Closes after SETUP_WINDOW_S.
#include <stdbool.h>

void setup_portal_init(void);   // once at boot
void setup_portal_start(void);  // any task; returns immediately
void setup_portal_stop(void);
bool setup_portal_active(void);
