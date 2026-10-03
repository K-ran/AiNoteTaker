#pragma once
// Audio chunks on the SD card (8.3 names; FAT long names are off):
//   REC/00000123.PRT  being recorded
//   REC/00000123.OGG  finished, waiting for upload
//   REC/00000123.JSN  sidecar: metadata + upload state (survives reboots)
//   REC/BAD/...       chunks the server explicitly rejected REJECT_LIMIT times
// Only the recorder task opens/finishes chunks; only the uplink task deletes them.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "cJSON.h"

typedef struct {
    uint32_t seq;
    FILE *f;
    int64_t start_uptime_ms;
} chunk_t;

bool chunk_store_init(void);                 // mounts SD, creates dirs, recovers after power cuts
bool chunk_store_ok(void);
void chunk_store_report_error(void);         // I/O failure: after repeats, unmount + remount
bool chunk_store_has_space(void);            // free > SD_MIN_FREE_MB
void chunk_store_space(uint32_t *total_mb, uint32_t *free_mb);

bool chunk_open(chunk_t *c);                 // new .PRT + sidecar (state OPEN)
bool chunk_finish(chunk_t *c, uint32_t duration_ms, uint64_t bytes);   // .PRT -> .OGG, state READY

int chunk_list_ready(uint32_t *seqs, int max);   // oldest `max` finished chunks
int chunk_count_ready(void);
void chunk_path(uint32_t seq, const char *ext, char *out, size_t len);

cJSON *chunk_meta_load(uint32_t seq);        // caller frees; NULL if missing
bool chunk_meta_save(uint32_t seq, cJSON *meta);
void chunk_delete(uint32_t seq);
void chunk_quarantine(uint32_t seq);
int chunk_clear_all(void);                   // developer console only: wipes REC/ (not BAD/)
