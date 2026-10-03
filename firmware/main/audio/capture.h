#pragma once
// Mic capture: reads the codec continuously and pushes 16 kHz mono PCM into a
// PSRAM ring buffer, so SD or network stalls never block the I2S DMA.
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

void capture_start(void);
// Blocks until exactly `samples` are available (or timeout). Returns true when filled.
bool capture_read(int16_t *out, size_t samples, int timeout_ms);
// Level over the last few seconds, dBFS (-99 for silence). For telemetry/mic-silent checks.
float capture_level_dbfs(void);
uint32_t capture_overruns(void);
