#pragma once
// Events and status snapshots as JSON lines on the SD card. P0 only stores them;
// P1 sends this backlog to the official telemetry endpoint.
#include <stdbool.h>

void event_log_init(void);              // call first: before anything can log
void event_log_set_sd(bool sd_ok);       // after the SD card mount attempt
void event_log(const char *event, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void event_log_status(const char *json_line);    // one heartbeat snapshot
