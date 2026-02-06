#include "wifi_controller.h"
#include "serial_protocol.h"

#include <string.h>
#include <stdio.h>

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "wifi_ctrl";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static EventGroupHandle_t s_wifi_event_group;
static esp_netif_t *s_ap_netif = NULL;
static esp_netif_t *s_sta_netif = NULL;
static bool s_ap_active = false;
static bool s_sta_connected = false;

static ap_status_t s_ap_status;

static void mac_to_str(const uint8_t *mac, char *str, size_t len)
{
    snprintf(str, len, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void ip_to_str(esp_ip4_addr_t ip, char *str, size_t len)
{
    snprintf(str, len, IPSTR, IP2STR(&ip));
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
            char mac_str[18];
            mac_to_str(event->mac, mac_str, sizeof(mac_str));
            ESP_LOGI(TAG, "Station connected: %s", mac_str);
            // IP will be reported via IP_EVENT_AP_STAIPASSIGNED
            break;
        }
        case WIFI_EVENT_AP_STADISCONNECTED: {
            wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
            char mac_str[18];
            mac_to_str(event->mac, mac_str, sizeof(mac_str));
            ESP_LOGI(TAG, "Station disconnected: %s", mac_str);

            // Remove from tracking
            for (int i = 0; i < s_ap_status.station_count; i++) {
                if (memcmp(s_ap_status.stations[i].mac, event->mac, 6) == 0) {
                    // Shift remaining entries
                    for (int j = i; j < s_ap_status.station_count - 1; j++) {
                        s_ap_status.stations[j] = s_ap_status.stations[j + 1];
                    }
                    s_ap_status.station_count--;
                    break;
                }
            }

            // Send event
            cJSON *payload = cJSON_CreateObject();
            cJSON_AddStringToObject(payload, "mac", mac_str);
            serial_send_event("STA_DISCONNECT", payload);
            cJSON_Delete(payload);
            break;
        }
        case WIFI_EVENT_STA_DISCONNECTED: {
            s_sta_connected = false;
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            break;
        }
        default:
            break;
        }
    } else if (event_base == IP_EVENT) {
        switch (event_id) {
        case IP_EVENT_AP_STAIPASSIGNED: {
            ip_event_ap_staipassigned_t *event = (ip_event_ap_staipassigned_t *)event_data;
            char ip_str[16];
            ip_to_str(event->ip, ip_str, sizeof(ip_str));
            char mac_str[18];
            mac_to_str(event->mac, mac_str, sizeof(mac_str));

            // Track in status
            if (s_ap_status.station_count < WIFI_MAX_STATIONS) {
                station_info_t *si = &s_ap_status.stations[s_ap_status.station_count];
                memcpy(si->mac, event->mac, 6);
                strncpy(si->ip, ip_str, sizeof(si->ip));
                s_ap_status.station_count++;
            }

            ESP_LOGI(TAG, "Station IP assigned: %s (MAC: %s)", ip_str, mac_str);

            cJSON *payload = cJSON_CreateObject();
            cJSON_AddStringToObject(payload, "mac", mac_str);
            cJSON_AddStringToObject(payload, "ip", ip_str);
            serial_send_event("STA_CONNECT", payload);
            cJSON_Delete(payload);
            break;
        }
        case IP_EVENT_STA_GOT_IP: {
            ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
            ESP_LOGI(TAG, "STA got IP: " IPSTR, IP2STR(&event->ip_info.ip));
            s_sta_connected = true;
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
            break;
        }
        default:
            break;
        }
    }
}

void wifi_controller_init(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_ap_netif = esp_netif_create_default_wifi_ap();
    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                         &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                                         &wifi_event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    memset(&s_ap_status, 0, sizeof(s_ap_status));
    ESP_LOGI(TAG, "WiFi controller initialized");
}

esp_err_t wifi_ap_start(const char *ssid, const char *password, int channel)
{
    // Stop AP if already running
    if (s_ap_active) {
        wifi_ap_stop();
    }

    // Stop STA if connected
    if (s_sta_connected) {
        wifi_sta_leave();
    }

    wifi_config_t wifi_config = {
        .ap = {
            .max_connection = WIFI_MAX_STATIONS,
            .channel = channel,
            .authmode = (strlen(password) > 0) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN,
            .pmf_cfg = {
                .required = false,
            },
        },
    };

    strncpy((char *)wifi_config.ap.ssid, ssid, sizeof(wifi_config.ap.ssid));
    wifi_config.ap.ssid_len = strlen(ssid);

    if (strlen(password) > 0) {
        strncpy((char *)wifi_config.ap.password, password, sizeof(wifi_config.ap.password));
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    esp_err_t ret = esp_wifi_start();
    if (ret != ESP_OK) {
        return ret;
    }

    s_ap_active = true;
    memset(&s_ap_status, 0, sizeof(s_ap_status));
    s_ap_status.active = true;
    strncpy(s_ap_status.ssid, ssid, sizeof(s_ap_status.ssid));
    s_ap_status.channel = channel;

    ESP_LOGI(TAG, "AP started: SSID=%s channel=%d", ssid, channel);
    return ESP_OK;
}

esp_err_t wifi_ap_stop(void)
{
    if (s_ap_active) {
        esp_wifi_stop();
        s_ap_active = false;
        memset(&s_ap_status, 0, sizeof(s_ap_status));
        ESP_LOGI(TAG, "AP stopped");
    }
    return ESP_OK;
}

void wifi_ap_get_status(ap_status_t *status)
{
    memcpy(status, &s_ap_status, sizeof(ap_status_t));
}

esp_err_t wifi_sta_join(const char *ssid, const char *password, int timeout_sec, sta_connection_t *conn)
{
    // Stop AP if running (ESP32-C3 single radio constraint)
    if (s_ap_active) {
        wifi_ap_stop();
    }

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = (strlen(password) > 0) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN,
        },
    };

    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    if (strlen(password) > 0) {
        strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password));
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());

    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                            pdFALSE, pdFALSE,
                                            pdMS_TO_TICKS(timeout_sec * 1000));

    if (bits & WIFI_CONNECTED_BIT) {
        esp_netif_ip_info_t ip_info;
        esp_netif_get_ip_info(s_sta_netif, &ip_info);
        snprintf(conn->ip, sizeof(conn->ip), IPSTR, IP2STR(&ip_info.ip));
        snprintf(conn->gateway, sizeof(conn->gateway), IPSTR, IP2STR(&ip_info.gw));
        ESP_LOGI(TAG, "STA connected to %s, IP=%s", ssid, conn->ip);
        return ESP_OK;
    }

    // Timeout or failure
    esp_wifi_disconnect();
    esp_wifi_stop();
    s_sta_connected = false;
    return ESP_ERR_TIMEOUT;
}

esp_err_t wifi_sta_leave(void)
{
    if (s_sta_connected) {
        esp_wifi_disconnect();
        s_sta_connected = false;
    }
    esp_wifi_stop();
    ESP_LOGI(TAG, "STA disconnected");
    return ESP_OK;
}

int wifi_scan(scan_result_t *results, int max_results)
{
    // If not started, start in STA mode for scanning
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    bool was_stopped = (mode == WIFI_MODE_NULL);

    if (was_stopped) {
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_start();
    }

    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 100,
        .scan_time.active.max = 300,
    };

    esp_err_t ret = esp_wifi_scan_start(&scan_config, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Scan start failed: %s", esp_err_to_name(ret));
        if (was_stopped) {
            esp_wifi_stop();
        }
        return 0;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);

    if (ap_count > (uint16_t)max_results) {
        ap_count = max_results;
    }

    wifi_ap_record_t *ap_records = malloc(ap_count * sizeof(wifi_ap_record_t));
    if (!ap_records) {
        if (was_stopped) {
            esp_wifi_stop();
        }
        return 0;
    }

    esp_wifi_scan_get_ap_records(&ap_count, ap_records);

    for (int i = 0; i < ap_count; i++) {
        strncpy(results[i].ssid, (char *)ap_records[i].ssid, sizeof(results[i].ssid) - 1);
        results[i].ssid[sizeof(results[i].ssid) - 1] = '\0';
        results[i].rssi = ap_records[i].rssi;

        switch (ap_records[i].authmode) {
        case WIFI_AUTH_OPEN:           strncpy(results[i].auth, "OPEN", sizeof(results[i].auth)); break;
        case WIFI_AUTH_WEP:            strncpy(results[i].auth, "WEP", sizeof(results[i].auth)); break;
        case WIFI_AUTH_WPA_PSK:        strncpy(results[i].auth, "WPA", sizeof(results[i].auth)); break;
        case WIFI_AUTH_WPA2_PSK:       strncpy(results[i].auth, "WPA2", sizeof(results[i].auth)); break;
        case WIFI_AUTH_WPA3_PSK:       strncpy(results[i].auth, "WPA3", sizeof(results[i].auth)); break;
        case WIFI_AUTH_WPA2_ENTERPRISE:strncpy(results[i].auth, "WPA2_ENTERPRISE", sizeof(results[i].auth)); break;
        default:                       strncpy(results[i].auth, "UNKNOWN", sizeof(results[i].auth)); break;
        }
    }

    free(ap_records);

    if (was_stopped) {
        esp_wifi_stop();
    }

    ESP_LOGI(TAG, "Scan found %d networks", ap_count);
    return ap_count;
}

bool wifi_ap_is_active(void)
{
    return s_ap_active;
}

bool wifi_sta_is_connected(void)
{
    return s_sta_connected;
}
