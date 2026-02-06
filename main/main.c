#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "serial_protocol.h"
#include "wifi_controller.h"
#include "http_relay.h"
#include "version.h"

static const char *TAG = "main";

// --- Command Handlers ---

static void cmd_ping(const char *cmd, cJSON *args)
{
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "fw_version", FW_VERSION);
    cJSON_AddNumberToObject(payload, "uptime", (double)(xTaskGetTickCount() * portTICK_PERIOD_MS));
    serial_send_response_ok(cmd, payload);
    cJSON_Delete(payload);
}

static void cmd_ap_start(const char *cmd, cJSON *args)
{
    const cJSON *ssid = cJSON_GetObjectItem(args, "ssid");
    if (!ssid || !cJSON_IsString(ssid)) {
        serial_send_response_err(cmd, "Missing required field: ssid");
        return;
    }

    const cJSON *pass = cJSON_GetObjectItem(args, "pass");
    const char *password = (pass && cJSON_IsString(pass)) ? pass->valuestring : "";

    const cJSON *chan = cJSON_GetObjectItem(args, "channel");
    int channel = (chan && cJSON_IsNumber(chan)) ? chan->valueint : 6;

    esp_err_t ret = wifi_ap_start(ssid->valuestring, password, channel);
    if (ret == ESP_OK) {
        cJSON *payload = cJSON_CreateObject();
        cJSON_AddStringToObject(payload, "ip", "192.168.4.1");
        serial_send_response_ok(cmd, payload);
        cJSON_Delete(payload);
    } else {
        serial_send_response_err(cmd, "Failed to start AP");
    }
}

static void cmd_ap_stop(const char *cmd, cJSON *args)
{
    wifi_ap_stop();
    serial_send_response_ok(cmd, NULL);
}

static void cmd_ap_status(const char *cmd, cJSON *args)
{
    ap_status_t status;
    wifi_ap_get_status(&status);

    cJSON *payload = cJSON_CreateObject();
    cJSON_AddBoolToObject(payload, "active", status.active);

    if (status.active) {
        cJSON_AddStringToObject(payload, "ssid", status.ssid);
        cJSON_AddNumberToObject(payload, "channel", status.channel);

        cJSON *stations = cJSON_CreateArray();
        for (int i = 0; i < status.station_count; i++) {
            cJSON *sta = cJSON_CreateObject();
            char mac_str[18];
            snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                     status.stations[i].mac[0], status.stations[i].mac[1],
                     status.stations[i].mac[2], status.stations[i].mac[3],
                     status.stations[i].mac[4], status.stations[i].mac[5]);
            cJSON_AddStringToObject(sta, "mac", mac_str);
            cJSON_AddStringToObject(sta, "ip", status.stations[i].ip);
            cJSON_AddItemToArray(stations, sta);
        }
        cJSON_AddItemToObject(payload, "stations", stations);
    }

    serial_send_response_ok(cmd, payload);
    cJSON_Delete(payload);
}

static void cmd_sta_join(const char *cmd, cJSON *args)
{
    const cJSON *ssid = cJSON_GetObjectItem(args, "ssid");
    if (!ssid || !cJSON_IsString(ssid)) {
        serial_send_response_err(cmd, "Missing required field: ssid");
        return;
    }

    const cJSON *pass = cJSON_GetObjectItem(args, "pass");
    const char *password = (pass && cJSON_IsString(pass)) ? pass->valuestring : "";

    const cJSON *timeout = cJSON_GetObjectItem(args, "timeout");
    int timeout_sec = (timeout && cJSON_IsNumber(timeout)) ? timeout->valueint : 15;

    sta_connection_t conn = {0};
    esp_err_t ret = wifi_sta_join(ssid->valuestring, password, timeout_sec, &conn);

    if (ret == ESP_OK) {
        cJSON *payload = cJSON_CreateObject();
        cJSON_AddStringToObject(payload, "ip", conn.ip);
        cJSON_AddStringToObject(payload, "gateway", conn.gateway);
        serial_send_response_ok(cmd, payload);
        cJSON_Delete(payload);
    } else {
        serial_send_response_err(cmd, "Connection timeout");
    }
}

static void cmd_sta_leave(const char *cmd, cJSON *args)
{
    wifi_sta_leave();
    serial_send_response_ok(cmd, NULL);
}

static void cmd_http(const char *cmd, cJSON *args)
{
    http_result_t result = http_relay_request(args);

    if (result.error) {
        cJSON *payload = cJSON_CreateObject();
        cJSON_AddStringToObject(payload, "error", result.error);
        cJSON_AddNumberToObject(payload, "code", result.status_code);
        serial_send_response_err(cmd, result.error);
        cJSON_Delete(payload);
    } else {
        cJSON *payload = cJSON_CreateObject();
        cJSON_AddNumberToObject(payload, "status", result.status_code);
        cJSON_AddRawToObject(payload, "headers", result.headers_json);
        cJSON_AddStringToObject(payload, "body", result.body_base64 ? result.body_base64 : "");
        serial_send_response_ok(cmd, payload);
        cJSON_Delete(payload);
    }

    http_result_free(&result);
}

static void cmd_scan(const char *cmd, cJSON *args)
{
    scan_result_t results[20];
    int count = wifi_scan(results, 20);

    cJSON *payload = cJSON_CreateObject();
    cJSON *networks = cJSON_CreateArray();

    for (int i = 0; i < count; i++) {
        cJSON *net = cJSON_CreateObject();
        cJSON_AddStringToObject(net, "ssid", results[i].ssid);
        cJSON_AddNumberToObject(net, "rssi", results[i].rssi);
        cJSON_AddStringToObject(net, "auth", results[i].auth);
        cJSON_AddItemToArray(networks, net);
    }

    cJSON_AddItemToObject(payload, "networks", networks);
    serial_send_response_ok(cmd, payload);
    cJSON_Delete(payload);
}

static void cmd_reset(const char *cmd, cJSON *args)
{
    serial_send_response_ok(cmd, NULL);
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
}

// --- Command Table ---

static const cmd_entry_t commands[] = {
    {"PING",      cmd_ping},
    {"AP_START",  cmd_ap_start},
    {"AP_STOP",   cmd_ap_stop},
    {"AP_STATUS", cmd_ap_status},
    {"STA_JOIN",  cmd_sta_join},
    {"STA_LEAVE", cmd_sta_leave},
    {"HTTP",      cmd_http},
    {"SCAN",      cmd_scan},
    {"RESET",     cmd_reset},
    {NULL, NULL},
};

void app_main(void)
{
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "WiFi Tester v%s starting", FW_VERSION);

    // Initialize subsystems
    serial_init();
    wifi_controller_init();

    // Register commands and start serial loop
    serial_register_commands(commands);
    serial_run();
}
