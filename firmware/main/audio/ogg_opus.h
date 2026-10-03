#pragma once
// Minimal Ogg Opus muxer (RFC 7845). Pure C + stdio, no ESP-IDF deps, so it is
// unit-tested on the host (test/test_ogg_opus.c).

#include <stdint.h>
#include <stdio.h>
#include <stddef.h>

#define OGG_MAX_BODY 16384
#define OGG_PACKETS_PER_PAGE 50     // 50 x 20 ms = 1 s of audio per page

typedef struct {
    FILE *f;
    uint32_t serial;
    uint32_t page_seq;
    uint64_t granule;               // 48 kHz samples after the last complete packet
    uint8_t seg[255];
    int nseg;
    uint8_t body[OGG_MAX_BODY];
    size_t body_len;
    int packets;                    // packets buffered in the pending page
    uint64_t bytes_written;
} ogg_opus_t;

// Writes the OpusHead and OpusTags pages. tags are "KEY=value" strings.
int ogg_opus_begin(ogg_opus_t *o, FILE *f, uint32_t serial, uint32_t input_rate,
                   uint16_t pre_skip, const char *vendor, const char *const *tags, int ntags);

// Buffers one Opus packet; writes a page when the page is full.
// samples48 = packet duration in 48 kHz samples (960 for 20 ms).
int ogg_opus_write_packet(ogg_opus_t *o, const uint8_t *pkt, size_t len, uint32_t samples48);

// Writes any buffered packets as a page (call before fflush/fsync).
int ogg_opus_flush(ogg_opus_t *o);

// Writes the final page with the end-of-stream flag.
int ogg_opus_end(ogg_opus_t *o);
