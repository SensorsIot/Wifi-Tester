#include "http_relay.h"

#include <string.h>
#include <stdlib.h>

#include "esp_http_client.h"
#include "esp_log.h"
#include "mbedtls/base64.h"

static const char *TAG = "http_relay";

typedef struct {
    char *data;
    int len;
    int capacity;
} response_buffer_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    response_buffer_t *buf = (response_buffer_t *)evt->user_data;

    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (buf->len + evt->data_len < buf->capacity) {
            memcpy(buf->data + buf->len, evt->data, evt->data_len);
            buf->len += evt->data_len;
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

static char *base64_encode(const uint8_t *data, size_t len)
{
    size_t out_len = 0;
    // Calculate required output size
    mbedtls_base64_encode(NULL, 0, &out_len, data, len);

    char *out = malloc(out_len + 1);
    if (!out) return NULL;

    mbedtls_base64_encode((unsigned char *)out, out_len + 1, &out_len, data, len);
    out[out_len] = '\0';
    return out;
}

static uint8_t *base64_decode(const char *input, size_t *out_len)
{
    size_t decoded_len = 0;
    mbedtls_base64_decode(NULL, 0, &decoded_len, (const unsigned char *)input, strlen(input));

    uint8_t *out = malloc(decoded_len + 1);
    if (!out) return NULL;

    mbedtls_base64_decode(out, decoded_len + 1, &decoded_len, (const unsigned char *)input, strlen(input));
    out[decoded_len] = '\0';
    *out_len = decoded_len;
    return out;
}

http_result_t http_relay_request(cJSON *args)
{
    http_result_t result = {0};

    const cJSON *method_json = cJSON_GetObjectItem(args, "method");
    const cJSON *url_json = cJSON_GetObjectItem(args, "url");
    const cJSON *headers_json = cJSON_GetObjectItem(args, "headers");
    const cJSON *body_json = cJSON_GetObjectItem(args, "body");
    const cJSON *timeout_json = cJSON_GetObjectItem(args, "timeout");

    if (!method_json || !cJSON_IsString(method_json)) {
        result.error = strdup("Missing 'method' field");
        return result;
    }
    if (!url_json || !cJSON_IsString(url_json)) {
        result.error = strdup("Missing 'url' field");
        return result;
    }

    const char *method = method_json->valuestring;
    const char *url = url_json->valuestring;
    int timeout_sec = timeout_json && cJSON_IsNumber(timeout_json) ? timeout_json->valueint : 10;

    // Allocate response buffer
    response_buffer_t resp_buf = {
        .data = malloc(HTTP_MAX_RESPONSE_SIZE),
        .len = 0,
        .capacity = HTTP_MAX_RESPONSE_SIZE,
    };

    if (!resp_buf.data) {
        result.error = strdup("Out of memory");
        return result;
    }

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &resp_buf,
        .timeout_ms = timeout_sec * 1000,
        .disable_auto_redirect = false,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(resp_buf.data);
        result.error = strdup("Failed to initialize HTTP client");
        return result;
    }

    // Set method
    if (strcmp(method, "POST") == 0) {
        esp_http_client_set_method(client, HTTP_METHOD_POST);
    } else if (strcmp(method, "PUT") == 0) {
        esp_http_client_set_method(client, HTTP_METHOD_PUT);
    } else if (strcmp(method, "DELETE") == 0) {
        esp_http_client_set_method(client, HTTP_METHOD_DELETE);
    } else {
        esp_http_client_set_method(client, HTTP_METHOD_GET);
    }

    // Set custom headers
    if (headers_json && cJSON_IsObject(headers_json)) {
        cJSON *header = NULL;
        cJSON_ArrayForEach(header, headers_json) {
            if (cJSON_IsString(header)) {
                esp_http_client_set_header(client, header->string, header->valuestring);
            }
        }
    }

    // Decode and set request body
    uint8_t *body_data = NULL;
    size_t body_len = 0;
    if (body_json && cJSON_IsString(body_json) && strlen(body_json->valuestring) > 0) {
        body_data = base64_decode(body_json->valuestring, &body_len);
        if (body_data) {
            esp_http_client_set_post_field(client, (const char *)body_data, body_len);
        }
    }

    // Perform request
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        result.status_code = esp_http_client_get_status_code(client);

        // Base64 encode response body
        if (resp_buf.len > 0) {
            result.body_base64 = base64_encode((uint8_t *)resp_buf.data, resp_buf.len);
        } else {
            result.body_base64 = strdup("");
        }

        // Collect response headers as JSON
        result.headers_json = strdup("{}");

        ESP_LOGI(TAG, "HTTP %s %s -> %d (%d bytes)",
                 method, url, result.status_code, resp_buf.len);
    } else {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        result.status_code = -1;
        char err_msg[64];
        snprintf(err_msg, sizeof(err_msg), "HTTP request failed: %s", esp_err_to_name(err));
        result.error = strdup(err_msg);
    }

    free(body_data);
    free(resp_buf.data);
    esp_http_client_cleanup(client);

    return result;
}

void http_result_free(http_result_t *result)
{
    free(result->headers_json);
    free(result->body_base64);
    free(result->error);
    memset(result, 0, sizeof(http_result_t));
}
