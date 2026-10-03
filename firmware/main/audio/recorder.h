#pragma once
// Encoder + chunk writer: PCM from capture -> Opus -> Ogg -> SD, rotated every
// CHUNK_SECONDS and flushed every FLUSH_SECONDS. Safe to call from any task.
#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef enum { REC_RECORDING, REC_PAUSED, REC_STOPPED } rec_state_t;

#define RECORDER_STACK (32 * 1024)

void recorder_start(void);
void recorder_set_paused(bool paused);   // privacy pause request: closes the current chunk
void recorder_toggle_pause(void);
bool recorder_paused(void);
void recorder_rotate(void);              // close the current chunk now (console/testing)
const char *recorder_state_name(void);
uint32_t recorder_last_seq(void);
TaskHandle_t recorder_task_handle(void); // for stack high-water-mark telemetry
