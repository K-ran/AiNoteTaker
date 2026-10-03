#pragma once
// Wi-Fi radio. Off by default (battery). A "session" owns the station interface for
// one connect -> use -> disconnect cycle (uplink window, setup test; OTA/telemetry
// later). The setup hotspot (AP) is independent and can run alongside a session.
#include <stdbool.h>
#include <stdint.h>
#include "esp_wifi.h"

void wifi_link_init(void);

bool wifi_link_session_begin(int timeout_ms);   // false if another session holds the radio
void wifi_link_session_end(void);               // disconnects; radio off unless the hotspot is up

// Within a session:
bool wifi_link_connect(const char *ssid, const char *pass, int timeout_ms);
int wifi_link_scan(wifi_ap_record_t *out, int max);
bool wifi_link_connected(void);
int wifi_link_rssi(void);                       // dBm, 0 if not connected
uint8_t wifi_link_last_reason(void);            // last disconnect reason code (diagnostics)

esp_err_t wifi_link_ap_start(const char *ssid, const char *pass);
void wifi_link_ap_stop(void);
