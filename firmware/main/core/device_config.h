#pragma once
// Persistent device settings in NVS. P0 fills them from the setup page or the
// developer console; P2 adds values pushed by the backend.
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

esp_err_t config_init(void);
const char *config_device_id(void);
void config_get_names(char *store, size_t store_len, char *sales, size_t sales_len);
uint32_t config_boot_id(void);
esp_err_t config_set_wifi(const char *ssid, const char *pass);
esp_err_t config_set_names(const char *store, const char *salesperson);
esp_err_t config_set_token(const char *token);    // trims whitespace; rejects non-token characters
// Copies the token into buf (caller scrubs with explicit_bzero). Returns false if unset.
bool config_copy_token(char *buf, size_t len);
bool config_has_token(void);
void config_get_wifi(char *ssid, size_t ssid_len, char *pass, size_t pass_len);

// Chunk sequence numbers. seed() raises the counter above any number already
// on the card (protects against an NVS erase); commit() persists one after use.
void config_seed_seq(uint32_t highest_on_card);
uint32_t config_peek_next_seq(void);
void config_commit_seq(uint32_t seq);

// "Some Store" -> "Some-Store": safe for file names, max 24 chars.
void config_slug(const char *in, char *out, size_t out_len);
