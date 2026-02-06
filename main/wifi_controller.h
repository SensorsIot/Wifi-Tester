#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_netif.h"

#define WIFI_MAX_STATIONS 4

typedef struct {
    uint8_t mac[6];
    char ip[16];
} station_info_t;

typedef struct {
    bool active;
    char ssid[33];
    int channel;
    int station_count;
    station_info_t stations[WIFI_MAX_STATIONS];
} ap_status_t;

typedef struct {
    char ip[16];
    char gateway[16];
} sta_connection_t;

typedef struct {
    char ssid[33];
    int rssi;
    char auth[20];
} scan_result_t;

/**
 * Initialize WiFi subsystem (NVS, netif, event loop, WiFi driver).
 */
void wifi_controller_init(void);

/**
 * Start softAP with given SSID, password, and channel.
 * Returns ESP_OK on success.
 */
esp_err_t wifi_ap_start(const char *ssid, const char *password, int channel);

/**
 * Stop softAP. Idempotent.
 */
esp_err_t wifi_ap_stop(void);

/**
 * Get current AP status.
 */
void wifi_ap_get_status(ap_status_t *status);

/**
 * Join a WiFi network as station. Blocks up to timeout_sec.
 * On success, fills conn with IP info.
 */
esp_err_t wifi_sta_join(const char *ssid, const char *password, int timeout_sec, sta_connection_t *conn);

/**
 * Leave the current STA network.
 */
esp_err_t wifi_sta_leave(void);

/**
 * Scan for visible WiFi networks.
 * Returns count of results. Caller provides array and max_results.
 */
int wifi_scan(scan_result_t *results, int max_results);

/**
 * Check if AP mode is currently active.
 */
bool wifi_ap_is_active(void);

/**
 * Check if STA mode is currently connected.
 */
bool wifi_sta_is_connected(void);
