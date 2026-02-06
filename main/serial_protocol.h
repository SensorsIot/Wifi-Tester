#pragma once

#include <stddef.h>
#include "cJSON.h"

#define SERIAL_MAX_LINE_LEN 4096

typedef void (*cmd_handler_t)(const char *cmd_name, cJSON *args);

typedef struct {
    const char *name;
    cmd_handler_t handler;
} cmd_entry_t;

/**
 * Initialize serial I/O (no-op when using USB Serial/JTAG via VFS).
 */
void serial_init(void);

/**
 * Register command handlers table (NULL-terminated).
 */
void serial_register_commands(const cmd_entry_t *commands);

/**
 * Send a response line: RSP <cmd> OK [json]
 */
void serial_send_response_ok(const char *cmd, cJSON *payload);

/**
 * Send an error response: RSP <cmd> ERR [json]
 */
void serial_send_response_err(const char *cmd, const char *error_msg);

/**
 * Send an async event: EVT <type> [json]
 */
void serial_send_event(const char *event_type, cJSON *payload);

/**
 * Main serial processing loop. Blocks, reads lines, dispatches commands.
 * Call from app_main after initialization.
 */
void serial_run(void);
