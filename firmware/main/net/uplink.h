#pragma once
// Upload windows: every UPLOAD_INTERVAL_S turn Wi-Fi on, sync time, upload
// finished chunks oldest-first, write a status snapshot, turn Wi-Fi off.
#include <stdbool.h>

void uplink_start(void);
void uplink_trigger(void);        // run a window now (console, setup success)
bool uplink_healthy(void);        // uploads current, no auth problem
