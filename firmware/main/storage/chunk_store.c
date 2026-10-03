#include "chunk_store.h"
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "esp_heap_caps.h"
#include "bsp_board.h"
#include "app_config.h"
#include "device_config.h"
#include "timekeep.h"
#include "event_log.h"

static const char *TAG = "chunks";
static bool s_ok;
static int s_fail_count;
static int64_t s_last_remount_us;
static char *s_vbuf;                 // stdio buffer for the open chunk (internal DMA RAM, see init)

#define VBUF_SIZE 8192
#define MAX_LIST  512

void chunk_path(uint32_t seq, const char *ext, char *out, size_t len)
{
    snprintf(out, len, REC_DIR "/%08lu.%s", (unsigned long)seq, ext);
}

static bool exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

cJSON *chunk_meta_load(uint32_t seq)
{
    char path[48];
    chunk_path(seq, "JSN", path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    char *buf = malloc(2048);
    cJSON *m = NULL;
    if (buf) {
        size_t n = fread(buf, 1, 2047, f);
        buf[n] = 0;
        m = cJSON_Parse(buf);
        free(buf);
    }
    fclose(f);
    return m;
}

// Write .JST (fsynced), then replace .JSN. FAT can't rename over a file, so a
// power cut can leave only the complete .JST: recover() promotes it.
bool chunk_meta_save(uint32_t seq, cJSON *meta)
{
    char tmp[48], path[48];
    chunk_path(seq, "JST", tmp, sizeof(tmp));
    chunk_path(seq, "JSN", path, sizeof(path));
    char *s = cJSON_PrintUnformatted(meta);
    if (!s) return false;
    FILE *f = fopen(tmp, "w");
    bool ok = f && fputs(s, f) >= 0 && fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (f && fclose(f) != 0) ok = false;
    free(s);
    if (!ok) { unlink(tmp); return false; }
    unlink(path);
    return rename(tmp, path) == 0;
}

static cJSON *new_meta(uint32_t seq, bool known_start)
{
    char store[33], sales[33], iso[24] = "";
    config_get_names(store, sizeof(store), sales, sizeof(sales));
    if (known_start) timekeep_iso(iso, sizeof(iso));
    cJSON *m = cJSON_CreateObject();
    cJSON_AddNumberToObject(m, "seq", seq);
    cJSON_AddStringToObject(m, "device_id", config_device_id());
    cJSON_AddStringToObject(m, "store", store);
    cJSON_AddStringToObject(m, "salesperson", sales);
    cJSON_AddNumberToObject(m, "boot_id", config_boot_id());
    cJSON_AddNumberToObject(m, "start_uptime_ms", known_start ? (double)(esp_timer_get_time() / 1000) : -1);
    cJSON_AddStringToObject(m, "start_utc", iso);
    cJSON_AddStringToObject(m, "time_source", known_start ? timekeep_source_name() : "unknown");
    cJSON_AddStringToObject(m, "codec", "opus");
    cJSON_AddNumberToObject(m, "sample_rate", SAMPLE_RATE_HZ);
    cJSON_AddNumberToObject(m, "bitrate", OPUS_BITRATE_BPS);
    cJSON_AddStringToObject(m, "state", "OPEN");
    cJSON_AddNumberToObject(m, "attempts", 0);
    return m;
}

static void set_state(cJSON *m, const char *state)
{
    cJSON_DeleteItemFromObject(m, "state");
    cJSON_AddStringToObject(m, "state", state);
}

// Lists `<dir>/*.<ext>` (or every NNNNNNNN.XXX entry when ext is NULL) as sequence numbers.
static int list_seqs(const char *dir, const char *ext, uint32_t *out, int max)
{
    DIR *d = opendir(dir);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        if (strlen(e->d_name) != 12 || e->d_name[8] != '.') continue;
        if (ext && strcmp(e->d_name + 9, ext)) continue;
        out[n++] = strtoul(e->d_name, NULL, 10);
    }
    closedir(d);
    return n;
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

// Fixes what a power cut can leave behind. Collects names first, then changes the
// directory (renaming inside a readdir loop is unreliable on FAT).
static void recover(void)
{
    uint32_t *seqs = heap_caps_malloc(MAX_LIST * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    if (!seqs) return;
    char a[48], b[48];

    int n = list_seqs(REC_DIR, "JST", seqs, MAX_LIST);   // interrupted sidecar writes
    for (int i = 0; i < n; i++) {
        chunk_path(seqs[i], "JST", a, sizeof(a));
        chunk_path(seqs[i], "JSN", b, sizeof(b));
        if (exists(b)) unlink(a);                         // old .JSN intact: drop the partial new one
        else rename(a, b);                                // .JST was complete (fsynced before unlink)
    }

    n = list_seqs(REC_DIR, "PRT", seqs, MAX_LIST);       // chunks cut off by a power loss
    for (int i = 0; i < n; i++) {
        chunk_path(seqs[i], "PRT", a, sizeof(a));
        chunk_path(seqs[i], "OGG", b, sizeof(b));
        if (exists(b) || rename(a, b) != 0) {
            ESP_LOGE(TAG, "cannot recover %s", a);
            continue;
        }
        cJSON *m = chunk_meta_load(seqs[i]);
        if (!m) m = new_meta(seqs[i], false);
        set_state(m, "READY");
        cJSON_DeleteItemFromObject(m, "recovered");
        cJSON_AddBoolToObject(m, "recovered", true);
        chunk_meta_save(seqs[i], m);
        cJSON_Delete(m);
        event_log("power_cut_recovered", "seq=%lu", (unsigned long)seqs[i]);
    }

    n = list_seqs(REC_DIR, "OGG", seqs, MAX_LIST);       // finished chunks whose sidecar was lost
    for (int i = 0; i < n; i++) {
        chunk_path(seqs[i], "JSN", b, sizeof(b));
        if (exists(b)) continue;
        cJSON *m = new_meta(seqs[i], false);
        set_state(m, "READY");
        chunk_meta_save(seqs[i], m);
        cJSON_Delete(m);
    }

    // Never reuse a sequence number still on the card (e.g. after an NVS erase).
    uint32_t highest = 0;
    n = list_seqs(REC_DIR, NULL, seqs, MAX_LIST);
    for (int i = 0; i < n; i++) if (seqs[i] > highest) highest = seqs[i];
    n = list_seqs(BAD_DIR, NULL, seqs, MAX_LIST);
    for (int i = 0; i < n; i++) if (seqs[i] > highest) highest = seqs[i];
    config_seed_seq(highest);
    free(seqs);
}

static bool mount(void)
{
    esp_err_t err = esp_sdcard_init(SD_MOUNT, 8);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(err));
        return false;
    }
    mkdir(REC_DIR, 0777);
    mkdir(BAD_DIR, 0777);
    mkdir(LOG_DIR, 0777);
    return true;
}

bool chunk_store_init(void)
{
    // SDMMC only DMAs from internal RAM; a PSRAM stdio buffer would make every write
    // allocate a bounce buffer. One fixed internal buffer avoids that churn.
    s_vbuf = heap_caps_malloc(VBUF_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_ok = mount();
    event_log_set_sd(s_ok);
    if (s_ok) recover();
    else config_seed_seq(0);
    return s_ok;
}

bool chunk_store_ok(void) { return s_ok; }

void chunk_store_report_error(void)
{
    if (++s_fail_count < 3) return;
    // Repeated I/O errors: card removed or failing. Unmount and retry at most once a minute.
    int64_t now = esp_timer_get_time();
    if (now - s_last_remount_us < 60 * 1000000LL) return;
    s_last_remount_us = now;
    event_log("sd_fault", "%d errors, remounting", s_fail_count);
    if (s_ok) esp_sdcard_deinit(SD_MOUNT);
    s_ok = mount();
    event_log_set_sd(s_ok);
    if (s_ok) { s_fail_count = 0; recover(); }
}

void chunk_store_space(uint32_t *total_mb, uint32_t *free_mb)
{
    uint64_t total = 0, free_b = 0;
    if (!s_ok || esp_vfs_fat_info(SD_MOUNT, &total, &free_b) != ESP_OK) total = free_b = 0;
    *total_mb = total >> 20;
    *free_mb = free_b >> 20;
}

bool chunk_store_has_space(void)
{
    uint32_t total, free_mb;
    chunk_store_space(&total, &free_mb);
    return s_ok && total > 0 && free_mb > SD_MIN_FREE_MB;
}

bool chunk_open(chunk_t *c)
{
    uint32_t seq = config_peek_next_seq();
    char path[48];
    chunk_path(seq, "PRT", path, sizeof(path));
    c->f = exists(path) ? NULL : fopen(path, "wb");
    if (!c->f) {
        event_log("sd_fault", "open %s failed", path);
        chunk_store_report_error();
        return false;
    }
    config_commit_seq(seq);              // only consume the number once the file exists
    c->seq = seq;
    c->start_uptime_ms = esp_timer_get_time() / 1000;
    if (s_vbuf) setvbuf(c->f, s_vbuf, _IOFBF, VBUF_SIZE);
    cJSON *m = new_meta(seq, true);
    chunk_meta_save(seq, m);
    cJSON_Delete(m);
    return true;
}

bool chunk_finish(chunk_t *c, uint32_t duration_ms, uint64_t bytes)
{
    bool ok = fflush(c->f) == 0;
    ok &= fsync(fileno(c->f)) == 0;
    ok &= fclose(c->f) == 0;
    c->f = NULL;
    char prt[48], ogg[48];
    chunk_path(c->seq, "PRT", prt, sizeof(prt));
    chunk_path(c->seq, "OGG", ogg, sizeof(ogg));
    if (rename(prt, ogg) != 0) {
        event_log("sd_fault", "finish seq=%lu failed", (unsigned long)c->seq);
        chunk_store_report_error();
        return false;                    // left as .PRT: recover() finishes it after the next mount
    }
    cJSON *m = chunk_meta_load(c->seq);
    if (!m) m = new_meta(c->seq, false);
    set_state(m, "READY");
    cJSON_AddNumberToObject(m, "duration_ms", duration_ms);
    cJSON_AddNumberToObject(m, "bytes", (double)bytes);
    ok &= chunk_meta_save(c->seq, m);
    cJSON_Delete(m);
    if (ok) s_fail_count = 0;
    ESP_LOGI(TAG, "chunk %lu done: %lu ms, %llu bytes", (unsigned long)c->seq,
             (unsigned long)duration_ms, (unsigned long long)bytes);
    return ok;
}

int chunk_list_ready(uint32_t *seqs, int max)
{
    // Read all, sort, return the oldest `max`: readdir order is not age order.
    uint32_t *all = heap_caps_malloc(MAX_LIST * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    if (!all) return 0;
    int n = list_seqs(REC_DIR, "OGG", all, MAX_LIST);
    qsort(all, n, sizeof(*all), cmp_u32);
    int k = n < max ? n : max;
    memcpy(seqs, all, k * sizeof(*seqs));
    free(all);
    return k;
}

int chunk_count_ready(void)
{
    uint32_t *all = heap_caps_malloc(MAX_LIST * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    int n = all ? list_seqs(REC_DIR, "OGG", all, MAX_LIST) : 0;
    free(all);
    return n;
}

void chunk_delete(uint32_t seq)
{
    char p[48];
    chunk_path(seq, "OGG", p, sizeof(p));
    unlink(p);
    chunk_path(seq, "JSN", p, sizeof(p));
    unlink(p);
}

void chunk_quarantine(uint32_t seq)
{
    const char *exts[] = {"OGG", "JSN"};
    for (int i = 0; i < 2; i++) {
        char from[48], to[48];
        chunk_path(seq, exts[i], from, sizeof(from));
        snprintf(to, sizeof(to), BAD_DIR "/%08lu.%s", (unsigned long)seq, exts[i]);
        rename(from, to);
    }
    event_log("upload_rejected", "seq=%lu moved to BAD", (unsigned long)seq);
}

int chunk_clear_all(void)
{
    uint32_t *all = heap_caps_malloc(MAX_LIST * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    if (!all) return 0;
    int n = list_seqs(REC_DIR, NULL, all, MAX_LIST), removed = 0;
    const char *exts[] = {"OGG", "JSN", "JST"};
    for (int i = 0; i < n; i++) {
        char prt[48];
        chunk_path(all[i], "PRT", prt, sizeof(prt));
        if (exists(prt)) continue;                         // the chunk being recorded
        for (int k = 0; k < 3; k++) {
            char p[48];
            chunk_path(all[i], exts[k], p, sizeof(p));
            if (unlink(p) == 0) removed++;
        }
    }
    free(all);
    return removed;
}
