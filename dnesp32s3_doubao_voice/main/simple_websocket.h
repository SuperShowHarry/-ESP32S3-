#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef enum { SIMPLE_WS_CONNECTED, SIMPLE_WS_DISCONNECTED, SIMPLE_WS_DATA, SIMPLE_WS_ERROR } simple_ws_event_t;
typedef void (*simple_ws_callback_t)(simple_ws_event_t event, const uint8_t *data, size_t length, void *context);
esp_err_t simple_ws_start(const char *host, const char *path, const char *headers, simple_ws_callback_t callback, void *context);
int simple_ws_send_binary(const void *data, size_t length);
bool simple_ws_is_connected(void);

