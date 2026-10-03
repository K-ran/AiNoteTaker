#include "ogg_opus.h"
#include <string.h>

// Ogg CRC32: polynomial 0x04c11db7, no reflection, init 0.
static uint32_t crc_table[256];

static void crc_init(void)
{
    if (crc_table[1]) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t r = i << 24;
        for (int k = 0; k < 8; k++) r = (r & 0x80000000u) ? (r << 1) ^ 0x04c11db7u : r << 1;
        crc_table[i] = r;
    }
}

static uint32_t crc_update(uint32_t crc, const uint8_t *p, size_t n)
{
    while (n--) crc = (crc << 8) ^ crc_table[((crc >> 24) ^ *p++) & 0xff];
    return crc;
}

static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }

#define FLAG_BOS 0x02
#define FLAG_EOS 0x04

// Writes the pending segments/body as one page and resets the page buffer.
static int write_page(ogg_opus_t *o, uint8_t flags)
{
    uint8_t hdr[27 + 255];
    memcpy(hdr, "OggS", 4);
    hdr[4] = 0;
    hdr[5] = flags;
    put64(hdr + 6, o->granule);
    put32(hdr + 14, o->serial);
    put32(hdr + 18, o->page_seq++);
    put32(hdr + 22, 0);
    hdr[26] = (uint8_t)o->nseg;
    memcpy(hdr + 27, o->seg, o->nseg);
    size_t hlen = 27 + o->nseg;

    uint32_t crc = crc_update(0, hdr, hlen);
    crc = crc_update(crc, o->body, o->body_len);
    put32(hdr + 22, crc);

    if (fwrite(hdr, 1, hlen, o->f) != hlen) return -1;
    if (o->body_len && fwrite(o->body, 1, o->body_len, o->f) != o->body_len) return -1;
    o->bytes_written += hlen + o->body_len;
    o->nseg = 0;
    o->body_len = 0;
    o->packets = 0;
    return 0;
}

// Appends a packet's lacing values and bytes to the pending page. Caller checks room.
static void add_packet(ogg_opus_t *o, const uint8_t *pkt, size_t len)
{
    size_t left = len;
    while (left >= 255) { o->seg[o->nseg++] = 255; left -= 255; }
    o->seg[o->nseg++] = (uint8_t)left;
    memcpy(o->body + o->body_len, pkt, len);
    o->body_len += len;
    o->packets++;
}

int ogg_opus_begin(ogg_opus_t *o, FILE *f, uint32_t serial, uint32_t input_rate,
                   uint16_t pre_skip, const char *vendor, const char *const *tags, int ntags)
{
    crc_init();
    memset(o, 0, sizeof(*o));
    o->f = f;
    o->serial = serial;

    uint8_t head[19];
    memcpy(head, "OpusHead", 8);
    head[8] = 1;                         // version
    head[9] = 1;                         // mono
    head[10] = pre_skip; head[11] = pre_skip >> 8;
    put32(head + 12, input_rate);
    head[16] = 0; head[17] = 0;          // output gain
    head[18] = 0;                        // mapping family 0
    add_packet(o, head, sizeof(head));
    if (write_page(o, FLAG_BOS)) return -1;

    // OpusTags: "OpusTags", vendor, comment list. Must fit one page here (it does: < 16 KB).
    size_t vlen = strlen(vendor), pos = 0;
    uint8_t *b = o->body;
    memcpy(b, "OpusTags", 8); pos = 8;
    put32(b + pos, vlen); pos += 4;
    memcpy(b + pos, vendor, vlen); pos += vlen;
    put32(b + pos, ntags); pos += 4;
    for (int i = 0; i < ntags; i++) {
        size_t tl = strlen(tags[i]);
        if (pos + 4 + tl > OGG_MAX_BODY) return -1;
        put32(b + pos, tl); pos += 4;
        memcpy(b + pos, tags[i], tl); pos += tl;
    }
    size_t left = pos;
    while (left >= 255) { o->seg[o->nseg++] = 255; left -= 255; }
    o->seg[o->nseg++] = (uint8_t)left;
    o->body_len = pos;
    return write_page(o, 0);
}

int ogg_opus_write_packet(ogg_opus_t *o, const uint8_t *pkt, size_t len, uint32_t samples48)
{
    int lacing = (int)(len / 255) + 1;
    if (lacing > 255 || len > OGG_MAX_BODY) return -1;   // never happens at our bitrates
    if (o->nseg + lacing > 255 || o->body_len + len > OGG_MAX_BODY) {
        if (write_page(o, 0)) return -1;
    }
    add_packet(o, pkt, len);
    o->granule += samples48;
    if (o->packets >= OGG_PACKETS_PER_PAGE) return write_page(o, 0);
    return 0;
}

int ogg_opus_flush(ogg_opus_t *o)
{
    return o->packets ? write_page(o, 0) : 0;
}

int ogg_opus_end(ogg_opus_t *o)
{
    return write_page(o, FLAG_EOS);      // may be an empty page; still a valid EOS marker
}
