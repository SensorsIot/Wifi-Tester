#pragma once

#include "cJSON.h"

#define HTTP_MAX_RESPONSE_SIZE 4096

typedef struct {
    int status_code;
    char *headers_json;
    char *body_base64;
    char *error;
} http_result_t;

/**
 * Execute an HTTP request as specified by the JSON args from the CMD HTTP command.
 * Args: method, url, headers (optional), body (optional, base64), timeout (optional).
 * Caller must free the result with http_result_free().
 */
http_result_t http_relay_request(cJSON *args);

/**
 * Free resources allocated in an http_result_t.
 */
void http_result_free(http_result_t *result);
