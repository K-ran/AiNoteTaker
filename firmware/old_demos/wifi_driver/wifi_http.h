#pragma once

// Starts a task that connects to Wi-Fi, starts the LED web server, and every 10 s
// uploads new /sdcard/*.WAV files to UPLOAD_URL (renaming them to .SNT when done).
void wifi_http_start(void);
