#include "serial_protocol.h"

#include <string.h>
#include <stdio.h>

#include "driver/uart.h"
#include "esp_log.h"

static const char *TAG = "serial";

#define UART_NUM       UART_NUM_0
#define UART_BUF_SIZE  (SERIAL_MAX_LINE_LEN + 128)

static const cmd_entry_t *s_commands = NULL;
static char s_line_buf[SERIAL_MAX_LINE_LEN];
static char s_tx_buf[SERIAL_MAX_LINE_LEN];

void serial_init(void)
{
    uart_config_t uart_config = {
        .baud_rate  = SERIAL_BAUD_RATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    uart_driver_install(UART_NUM, UART_BUF_SIZE, UART_BUF_SIZE, 0, NULL, 0);
    uart_param_config(UART_NUM, &uart_config);
}

void serial_register_commands(const cmd_entry_t *commands)
{
    s_commands = commands;
}

static void serial_send_line(const char *line)
{
    uart_write_bytes(UART_NUM, line, strlen(line));
    uart_write_bytes(UART_NUM, "\n", 1);
}

void serial_send_response_ok(const char *cmd, cJSON *payload)
{
    if (payload) {
        char *json_str = cJSON_PrintUnformatted(payload);
        snprintf(s_tx_buf, sizeof(s_tx_buf), "RSP %s OK %s", cmd, json_str);
        free(json_str);
    } else {
        snprintf(s_tx_buf, sizeof(s_tx_buf), "RSP %s OK", cmd);
    }
    serial_send_line(s_tx_buf);
}

void serial_send_response_err(const char *cmd, const char *error_msg)
{
    snprintf(s_tx_buf, sizeof(s_tx_buf), "RSP %s ERR {\"error\":\"%s\"}", cmd, error_msg);
    serial_send_line(s_tx_buf);
}

void serial_send_event(const char *event_type, cJSON *payload)
{
    if (payload) {
        char *json_str = cJSON_PrintUnformatted(payload);
        snprintf(s_tx_buf, sizeof(s_tx_buf), "EVT %s %s", event_type, json_str);
        free(json_str);
    } else {
        snprintf(s_tx_buf, sizeof(s_tx_buf), "EVT %s", event_type);
    }
    serial_send_line(s_tx_buf);
}

static int serial_read_line(char *buf, int max_len)
{
    int pos = 0;
    while (pos < max_len - 1) {
        uint8_t ch;
        int len = uart_read_bytes(UART_NUM, &ch, 1, pdMS_TO_TICKS(100));
        if (len <= 0) {
            continue;
        }
        if (ch == '\n') {
            buf[pos] = '\0';
            // Strip trailing \r
            if (pos > 0 && buf[pos - 1] == '\r') {
                buf[pos - 1] = '\0';
                pos--;
            }
            return pos;
        }
        buf[pos++] = (char)ch;
    }
    buf[pos] = '\0';
    return pos;
}

static void dispatch_command(const char *cmd_name, cJSON *args)
{
    if (!s_commands) {
        serial_send_response_err(cmd_name, "No commands registered");
        return;
    }

    for (const cmd_entry_t *entry = s_commands; entry->name != NULL; entry++) {
        if (strcmp(entry->name, cmd_name) == 0) {
            entry->handler(cmd_name, args);
            return;
        }
    }

    serial_send_response_err(cmd_name, "Unknown command");
}

void serial_run(void)
{
    ESP_LOGI(TAG, "Serial protocol ready");

    while (1) {
        int len = serial_read_line(s_line_buf, sizeof(s_line_buf));
        if (len <= 0) {
            continue;
        }

        ESP_LOGD(TAG, "RX: %s", s_line_buf);

        // Expect: CMD <command> [json args]
        if (strncmp(s_line_buf, "CMD ", 4) != 0) {
            ESP_LOGW(TAG, "Ignoring non-CMD line: %s", s_line_buf);
            continue;
        }

        // Extract command name
        char *rest = s_line_buf + 4;
        char *space = strchr(rest, ' ');
        char cmd_name[32];
        cJSON *args = NULL;

        if (space) {
            int name_len = space - rest;
            if (name_len >= (int)sizeof(cmd_name)) {
                name_len = sizeof(cmd_name) - 1;
            }
            strncpy(cmd_name, rest, name_len);
            cmd_name[name_len] = '\0';

            // Parse JSON arguments
            args = cJSON_Parse(space + 1);
            if (!args) {
                serial_send_response_err(cmd_name, "Invalid JSON arguments");
                continue;
            }
        } else {
            strncpy(cmd_name, rest, sizeof(cmd_name) - 1);
            cmd_name[sizeof(cmd_name) - 1] = '\0';
            args = cJSON_CreateObject();
        }

        dispatch_command(cmd_name, args);
        cJSON_Delete(args);
    }
}
