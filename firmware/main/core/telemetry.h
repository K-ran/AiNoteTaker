#pragma once
// Device telemetry (Smaarthi reportDeviceTelemetry): heartbeats every HEARTBEAT_S and
// diagnostic events, queued durably on the SD card (in RAM without a card) and sent in
// batches during upload windows. Every record gets a stable eventId when captured, so
// retries after a timeout never create duplicates on the backend.
#include <stdbool.h>
#include <stdint.h>
#include "smaarthi_api.h"

typedef enum { TEL_TRACE, TEL_DEBUG, TEL_INFO, TEL_WARN, TEL_ERROR, TEL_FATAL } tel_severity_t;

void telemetry_init(void);          // after the SD mount attempt (recovers an interrupted queue trim)
void telemetry_set_sd(bool ok);
void telemetry_start(void);         // heartbeat task

// Capture an event (any task). attrs, if given, is consumed.
void telemetry_event(const char *name, tel_severity_t sev, const char *message, cJSON *attrs);

// Inside an upload window: send queued records. Returns the last API result
// (API_OK when the queue is empty or fully delivered).
api_result_t telemetry_flush(const char *token, const char *device_id);

int telemetry_pending(void);        // records waiting (approximate when queued on SD)
void telemetry_dump(int max_records);  // developer console: print the oldest queued records

// Counters other modules report into; heartbeats read and reset them.
void telemetry_note_chunk_recorded(void);
void telemetry_note_upload(uint32_t bytes);
void telemetry_note_window(int rssi_dbm, int connect_ms, int wifi_fail_reason, const char *last_error);
