#include "simple_websocket.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"

static const char *TAG = "simple_ws";
static const char *s_host, *s_path, *s_headers;
static simple_ws_callback_t s_callback;
static void *s_context;
static esp_tls_t *s_tls;
static SemaphoreHandle_t s_tx_lock;
static volatile bool s_connected;

static int tls_write_all(const uint8_t *data, size_t length) {
    size_t offset = 0;
    while (offset < length) { ssize_t n = esp_tls_conn_write(s_tls, data + offset, length - offset); if (n <= 0) return -1; offset += (size_t)n; }
    return (int)offset;
}
static int tls_read_all(uint8_t *data, size_t length) {
    size_t offset = 0;
    while (offset < length) { ssize_t n = esp_tls_conn_read(s_tls, data + offset, length - offset); if (n <= 0) return -1; offset += (size_t)n; }
    return (int)offset;
}
static int ws_send_frame(uint8_t opcode, const void *payload, size_t length) {
    if (!s_connected || s_tls == NULL || length > 0xffffffffu) return -1;
    size_t ext = length <= 125 ? 0 : (length <= 65535 ? 2 : 8), header_length = 2 + ext + 4;
    uint8_t *frame = malloc(header_length + length); if (!frame) return -1;
    size_t p = 0; frame[p++] = 0x80 | (opcode & 0x0f);
    if (length <= 125) frame[p++] = 0x80 | (uint8_t)length;
    else if (length <= 65535) { frame[p++] = 0x80 | 126; frame[p++] = (uint8_t)(length >> 8); frame[p++] = (uint8_t)length; }
    else { frame[p++] = 0x80 | 127; for (int shift = 56; shift >= 0; shift -= 8) frame[p++] = (uint8_t)(length >> shift); }
    uint32_t random_mask = esp_random(); uint8_t mask[4]; memcpy(mask, &random_mask, 4); memcpy(frame + p, mask, 4); p += 4;
    const uint8_t *source = payload; for (size_t i = 0; i < length; ++i) frame[p + i] = source[i] ^ mask[i & 3];
    /* Keep the WebSocket header and masked body in separate TLS writes.  This
     * mirrors the framing used by the verified Volcano Engine reference test
     * and avoids a peer-side compatibility issue seen with a coalesced write. */
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    int header_result = tls_write_all(frame, header_length);
    if (header_result == (int)header_length && length > 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    int body_result = header_result == (int)header_length
                          ? tls_write_all(frame + header_length, length)
                          : -1;
    xSemaphoreGive(s_tx_lock);
    free(frame);
    return header_result < 0 || body_result < 0 ? -1 : (int)length;
}
int simple_ws_send_binary(const void *data, size_t length) { return ws_send_frame(0x2, data, length); }
bool simple_ws_is_connected(void) { return s_connected; }

static bool perform_upgrade(void) {
    uint8_t nonce[16]; for (size_t i = 0; i < sizeof(nonce); i += 4) { uint32_t r = esp_random(); memcpy(nonce + i, &r, 4); }
    unsigned char key[32]; size_t key_length = 0;
    if (mbedtls_base64_encode(key, sizeof(key) - 1, &key_length, nonce, sizeof(nonce)) != 0) return false;
    key[key_length] = 0;
    size_t capacity = strlen(s_headers) + 768; char *request = malloc(capacity); if (!request) return false;
    int length = snprintf(request, capacity, "GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\nUser-Agent: ESP32 Websocket Client\r\n%s\r\n", s_path, s_host, key, s_headers);
    bool ok = length > 0 && (size_t)length < capacity && tls_write_all((const uint8_t *)request, length) == length; free(request); if (!ok) return false;
    /* Volcano Engine currently returns a handshake header larger than 1 KiB
     * (trace and CORS headers included).  Never declare the upgrade complete
     * until CRLF-CRLF is consumed; otherwise the unread HTTP tail is mistaken
     * for binary WebSocket frames. */
    char response[4096]; size_t used = 0; bool complete = false;
    while (used + 1 < sizeof(response)) {
        if (tls_read_all((uint8_t *)&response[used], 1) < 0) return false;
        used++;
        if (used >= 4 && memcmp(response + used - 4, "\r\n\r\n", 4) == 0) {
            complete = true;
            break;
        }
    }
    response[used] = 0;
    if (!complete) { ESP_LOGE(TAG, "upgrade response headers too large"); return false; }
    if (!strstr(response, "HTTP/1.1 101")) { ESP_LOGE(TAG, "upgrade rejected: %.80s", response); return false; }
    ESP_LOGI(TAG, "WebSocket upgrade accepted"); return true;
}
static bool receive_frame(void) {
    uint8_t h[2]; if (tls_read_all(h, 2) < 0) return false;
    ESP_LOGI(TAG, "received WebSocket frame header: %02x %02x", h[0], h[1]);
    uint8_t opcode = h[0] & 0x0f; bool masked = h[1] & 0x80; uint64_t length = h[1] & 0x7f;
    if (length == 126) { uint8_t e[2]; if (tls_read_all(e, 2) < 0) return false; length = ((uint16_t)e[0] << 8) | e[1]; }
    else if (length == 127) { uint8_t e[8]; if (tls_read_all(e, 8) < 0) return false; length = 0; for (int i = 0; i < 8; ++i) length = (length << 8) | e[i]; }
    if (length > 128 * 1024) { ESP_LOGE(TAG, "frame too large: %llu", (unsigned long long)length); return false; }
    uint8_t mask[4] = {0}; if (masked && tls_read_all(mask, 4) < 0) return false;
    uint8_t *payload = malloc((size_t)length + 1); if (!payload) return false;
    if (length && tls_read_all(payload, (size_t)length) < 0) { free(payload); return false; }
    for (size_t i = 0; masked && i < length; ++i) payload[i] ^= mask[i & 3];
    payload[length] = 0;
    if (opcode == 2 || opcode == 1) s_callback(SIMPLE_WS_DATA, payload, (size_t)length, s_context);
    else if (opcode == 9) ws_send_frame(10, payload, (size_t)length);
    else if (opcode == 8) { free(payload); return false; }
    free(payload); return true;
}
static void websocket_task(void *argument) {
    (void)argument;
    while (true) {
        esp_tls_cfg_t cfg = { .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = 30000 };
        s_tls = esp_tls_init();
        if (s_tls && esp_tls_conn_new_sync(s_host, strlen(s_host), 443, &cfg, s_tls) == 1 && perform_upgrade()) {
            s_connected = true; s_callback(SIMPLE_WS_CONNECTED, NULL, 0, s_context); while (s_connected && receive_frame()) {}
        } else s_callback(SIMPLE_WS_ERROR, NULL, 0, s_context);
        s_connected = false; if (s_tls) { esp_tls_conn_destroy(s_tls); s_tls = NULL; }
        s_callback(SIMPLE_WS_DISCONNECTED, NULL, 0, s_context); vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
esp_err_t simple_ws_start(const char *host, const char *path, const char *headers, simple_ws_callback_t callback, void *context) {
    if (!host || !path || !headers || !callback) return ESP_ERR_INVALID_ARG;
    s_host = host; s_path = path; s_headers = headers; s_callback = callback; s_context = context; s_tx_lock = xSemaphoreCreateMutex(); if (!s_tx_lock) return ESP_ERR_NO_MEM;
    return xTaskCreatePinnedToCore(websocket_task, "simple_ws", 10240, NULL, 5, NULL, 1) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

