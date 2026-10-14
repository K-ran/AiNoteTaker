#pragma once
// Device events: a local JSON-lines log on the SD card (LOG/EVENTS.LOG) and, for the
// names in the mapping table in event_log.c, a telemetry event sent to the backend.
#include <stdbool.h>

void event_log_init(void);              // call first: before anything can log
void event_log_set_sd(bool sd_ok);       // after the SD card mount attempt
void event_log(const char *event, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
