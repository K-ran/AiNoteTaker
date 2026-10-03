#pragma once
// Wall clock: RTC at boot, NTP when online (written back to the RTC).
#include <stdbool.h>
#include <stddef.h>
#include "driver/i2c_master.h"

typedef enum { TIME_NONE, TIME_RTC, TIME_HTTP, TIME_NTP } time_source_t;

void timekeep_init(i2c_master_bus_handle_t bus);
void timekeep_start_sntp(void);              // call once the network is up; safe to call again
bool timekeep_wait_sync(int timeout_ms);
const char *timekeep_source_name(void);
bool timekeep_valid(void);
// Fallback when NTP is blocked: an RFC 1123 HTTP Date header from a TLS-verified server.
void timekeep_from_http_date(const char *date);
void timekeep_iso(char *buf, size_t len);    // 2026-10-03T10:15:00Z, or "" when unknown
