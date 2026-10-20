#include "doubao_realtime.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app_config.h"
#include "audio_board.h"
#include "cJSON.h"
#include "display_ui.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "simple_websocket.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "doubao";
static const char *DOUBAO_HOST = "openspeech.bytedance.com";
static const char *DOUBAO_PATH = "/api/v3/realtime/dialogue";

enum {
    EVT_START_CONNECTION = 1,
    EVT_FINISH_CONNECTION = 2,
    EVT_START_SESSION = 100,
    EVT_FINISH_SESSION = 102,
    EVT_TASK_REQUEST = 200,
    EVT_END_ASR = 400,
    EVT_CLIENT_INTERRUPT = 515,

    EVT_CONNECTION_STARTED = 50,
    EVT_CONNECTION_FAILED = 51,
    EVT_CONNECTION_FINISHED = 52,
    EVT_SESSION_STARTED = 150,
    EVT_SESSION_FINISHED = 152,
    EVT_SESSION_FAILED = 153,
    EVT_TTS_SENTENCE_START = 350,
    EVT_TTS_RESPONSE = 352,
    EVT_TTS_ENDED = 359,
    EVT_ASR_INFO = 450,
    EVT_ASR_RESPONSE = 451,
    EVT_ASR_ENDED = 459,
    EVT_CHAT_RESPONSE = 550,
    EVT_CHAT_ENDED = 559,
    EVT_DIALOG_ERROR = 599,
};

static SemaphoreHandle_t s_send_lock;
static QueueHandle_t s_control_queue;
static volatile bool s_ready;
static volatile bool s_recording;
static char s_session_id[37];
static char s_connect_id[37];
static char s_headers[1024];
static uint8_t *s_ws_frame;
static size_t s_ws_frame_capacity;

static void put_be32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value >> 24);
    dst[1] = (uint8_t)(value >> 16);
    dst[2] = (uint8_t)(value >> 8);
    dst[3] = (uint8_t)value;
}

static uint32_t get_be32(const uint8_t *src)
{
    return ((uint32_t)src[0] << 24) | ((uint32_t)src[1] << 16) |
           ((uint32_t)src[2] << 8) | src[3];
}

static void make_uuid(char output[37])
{
    uint32_t a = esp_random();
    uint32_t b = esp_random();
    uint32_t c = esp_random();
    uint32_t d = esp_random();
    snprintf(output, 37, "%08" PRIx32 "-%04" PRIx32 "-%04" PRIx32 "-%04" PRIx32 "-%04" PRIx32 "%08" PRIx32,
             a, b >> 16, (b & 0x0fff) | 0x4000, (c >> 16 & 0x3fff) | 0x8000,
             c & 0xffff, d);
}

static int send_event(int event, const void *payload, size_t payload_len, bool audio)
{
    if (!simple_ws_is_connected()) {
        return -1;
    }

    bool connection_event = event == EVT_START_CONNECTION || event == EVT_FINISH_CONNECTION;
    size_t session_len = connection_event ? 0 : strlen(s_session_id);
    size_t frame_len = 4 + 4 + (connection_event ? 0 : 4 + session_len) + 4 + payload_len;
    uint8_t *frame = heap_caps_malloc(frame_len, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (frame == NULL) return -1;

    size_t offset = 0;
    frame[offset++] = 0x11;
    frame[offset++] = audio ? 0x24 : 0x14;
    frame[offset++] = audio ? 0x00 : 0x10;
    frame[offset++] = 0x00;
    put_be32(frame + offset, (uint32_t)event);
    offset += 4;
    if (!connection_event) {
        put_be32(frame + offset, (uint32_t)session_len);
        offset += 4;
        memcpy(frame + offset, s_session_id, session_len);
        offset += session_len;
    }
    put_be32(frame + offset, (uint32_t)payload_len);
    offset += 4;
    if (payload_len > 0) {
        memcpy(frame + offset, payload, payload_len);
    }

    xSemaphoreTake(s_send_lock, portMAX_DELAY);
    int sent = simple_ws_send_binary(frame, frame_len);
    xSemaphoreGive(s_send_lock);
    if (sent < 0) {
        ESP_LOGE(TAG, "send event %d failed", event);
    } else {
        ESP_LOGI(TAG, "sent event %d: %d/%u bytes", event, sent, (unsigned)frame_len);
    }
    free(frame);
    return sent;
}

static void send_start_session(void)
{
    make_uuid(s_session_id);

    cJSON *root = cJSON_CreateObject();
    cJSON *asr = cJSON_AddObjectToObject(root, "asr");
    cJSON *audio_info = cJSON_AddObjectToObject(asr, "audio_info");
    cJSON_AddStringToObject(audio_info, "format", "pcm");
    cJSON_AddNumberToObject(audio_info, "sample_rate", AUDIO_CLOUD_SAMPLE_RATE);
    cJSON_AddNumberToObject(audio_info, "channel", 1);
    cJSON *asr_extra = cJSON_AddObjectToObject(asr, "extra");
    cJSON_AddNumberToObject(asr_extra, "end_smooth_window_ms", 800);
    cJSON_AddBoolToObject(asr_extra, "enable_custom_vad", false);
    cJSON_AddBoolToObject(asr_extra, "enable_asr_twopass", true);

    cJSON *tts = cJSON_AddObjectToObject(root, "tts");
    cJSON_AddStringToObject(tts, "speaker", CONFIG_DOUBAO_SPEAKER);
    cJSON *audio_cfg = cJSON_AddObjectToObject(tts, "audio_config");
    cJSON_AddNumberToObject(audio_cfg, "channel", 1);
    cJSON_AddStringToObject(audio_cfg, "format", "pcm_s16le");
    cJSON_AddNumberToObject(audio_cfg, "sample_rate", AUDIO_HW_SAMPLE_RATE);

    cJSON *dialog = cJSON_AddObjectToObject(root, "dialog");
    cJSON_AddStringToObject(dialog, "bot_name", CONFIG_DOUBAO_BOT_NAME);
    cJSON_AddStringToObject(dialog, "system_role", CONFIG_DOUBAO_SYSTEM_ROLE);
    cJSON_AddStringToObject(dialog, "speaking_style", CONFIG_DOUBAO_SPEAKING_STYLE);
    cJSON_AddStringToObject(dialog, "dialog_id", "");
    cJSON *dialog_extra = cJSON_AddObjectToObject(dialog, "extra");
    cJSON_AddBoolToObject(dialog_extra, "strict_audit", true);
    cJSON_AddStringToObject(dialog_extra, "input_mod", "push_to_talk");
    cJSON_AddBoolToObject(dialog_extra, "enable_loudness_norm", true);
    cJSON_AddBoolToObject(dialog_extra, "enable_conversation_truncate", true);
    cJSON_AddStringToObject(dialog_extra, "model", CONFIG_DOUBAO_MODEL);

    char *json = cJSON_PrintUnformatted(root);
    if (json != NULL) {
        ESP_LOGI(TAG, "starting session %s", s_session_id);
        send_event(EVT_START_SESSION, json, strlen(json), false);
        cJSON_free(json);
    }
    cJSON_Delete(root);
}

static void control_task(void *arg)
{
    (void)arg;
    int command;
    static const char empty_json[] = "{}";
    while (true) {
        if (xQueueReceive(s_control_queue, &command, portMAX_DELAY) == pdTRUE) {
            if (command == EVT_START_CONNECTION) {
                send_event(EVT_START_CONNECTION, empty_json, 2, false);
            } else if (command == EVT_START_SESSION) {
                send_start_session();
            }
        }
    }
}

static void parse_json_event(uint32_t event, const uint8_t *payload, size_t payload_len)
{
    ESP_LOGI(TAG, "server event %" PRIu32 ", payload=%u", event, (unsigned)payload_len);
    char *json_text = malloc(payload_len + 1);
    if (json_text == NULL) return;
    memcpy(json_text, payload, payload_len);
    json_text[payload_len] = '\0';
    cJSON *json = cJSON_Parse(json_text);

    switch (event) {
        case EVT_CONNECTION_STARTED:
            ESP_LOGI(TAG, "cloud connection started");
            display_ui_set_status("AI CONNECTED - STARTING SESSION...");
            send_start_session();
            break;
        case EVT_SESSION_STARTED:
            s_ready = true;
            display_ui_set_status("READY - HOLD BOOT TO TALK");
            ESP_LOGI(TAG, "voice session ready");
            break;
        case EVT_ASR_INFO:
            audio_board_clear_playback();
            display_ui_set_status("RECOGNIZING SPEECH...");
            break;
        case EVT_ASR_RESPONSE:
            if (json != NULL) {
                cJSON *results = cJSON_GetObjectItem(json, "results");
                cJSON *first = cJSON_IsArray(results) ? cJSON_GetArrayItem(results, 0) : NULL;
                cJSON *text = first ? cJSON_GetObjectItem(first, "text") : NULL;
                if (cJSON_IsString(text)) {
                    display_ui_set_user_text(text->valuestring);
                    ESP_LOGI(TAG, "USER: %s", text->valuestring);
                }
            }
            break;
        case EVT_ASR_ENDED:
            display_ui_set_status("AI IS THINKING...");
            display_ui_begin_assistant_text();
            break;
        case EVT_CHAT_RESPONSE:
            if (json != NULL) {
                cJSON *content = cJSON_GetObjectItem(json, "content");
                if (cJSON_IsString(content)) {
                    display_ui_append_assistant_text(content->valuestring);
                    ESP_LOGI(TAG, "AI: %s", content->valuestring);
                }
            }
            break;
        case EVT_TTS_SENTENCE_START:
            display_ui_set_status("AI IS ANSWERING...");
            break;
        case EVT_TTS_ENDED:
            display_ui_set_status("READY - HOLD BOOT TO TALK");
            break;
        case EVT_SESSION_FAILED:
        case EVT_CONNECTION_FAILED:
        case EVT_DIALOG_ERROR:
            s_ready = false;
            display_ui_set_status("AI SERVICE ERROR - CHECK SERIAL");
            ESP_LOGE(TAG, "server event %" PRIu32 ": %s", event, json_text);
            break;
        default:
            ESP_LOGD(TAG, "event %" PRIu32 ": %s", event, json_text);
            break;
    }

    if (json != NULL) cJSON_Delete(json);
    free(json_text);
}

static void parse_protocol_frame(const uint8_t *frame, size_t length)
{
    if (length < 12 || (frame[0] >> 4) != 1) return;
    uint8_t message_type = frame[1] >> 4;
    uint8_t flags = frame[1] & 0x0f;
    size_t offset = 4;
    uint32_t event = 0;

    if (message_type == 0x0f) {
        if (offset + 4 > length) return;
        uint32_t error_code = get_be32(frame + offset);
        offset += 4;
        if (offset + 4 > length) return;
        uint32_t payload_len = get_be32(frame + offset);
        offset += 4;
        ESP_LOGE(TAG, "protocol error %" PRIu32 ": %.*s", error_code,
                 (int)((offset + payload_len <= length) ? payload_len : length - offset),
                 (const char *)(frame + offset));
        display_ui_set_status("AI PROTOCOL ERROR - CHECK SERIAL");
        return;
    }

    if (flags == 1 || flags == 3) {
        if (offset + 4 > length) return;
        offset += 4;
    }
    if ((flags & 4) == 0 || offset + 4 > length) return;
    event = get_be32(frame + offset);
    offset += 4;

    if ((event >= 50 && event <= 52) || event >= 100) {
        if (offset + 4 > length) return;
        uint32_t id_len = get_be32(frame + offset);
        offset += 4;
        if (id_len > 128 || offset + id_len > length) return;
        offset += id_len;
    }
    if (offset + 4 > length) return;
    uint32_t payload_len = get_be32(frame + offset);
    offset += 4;
    if (offset + payload_len > length) return;

    if (event == EVT_TTS_RESPONSE) {
        ESP_LOGI(TAG, "TTS PCM: %u bytes", (unsigned)payload_len);
        audio_board_queue_playback(frame + offset, payload_len);
    } else {
        parse_json_event(event, frame + offset, payload_len);
    }
}

static void websocket_event_handler(simple_ws_event_t event_id, const uint8_t *data,
                                    size_t data_len, void *context)
{
    (void)context;

    if (event_id == SIMPLE_WS_CONNECTED) {
        ESP_LOGI(TAG, "websocket authenticated and connected");
        display_ui_set_status("GATEWAY OK - AUTHENTICATING...");
        static const char empty_json[] = "{}";
        send_event(EVT_START_CONNECTION, empty_json, 2, false);
    } else if (event_id == SIMPLE_WS_DISCONNECTED) {
        s_ready = false;
        s_recording = false;
        display_ui_set_status("AI DISCONNECTED - RECONNECTING...");
        ESP_LOGW(TAG, "websocket disconnected");
    } else if (event_id == SIMPLE_WS_ERROR) {
        s_ready = false;
        display_ui_set_status("AI CONNECTION ERROR - CHECK SERIAL");
        ESP_LOGE(TAG, "websocket transport error");
    } else if (event_id == SIMPLE_WS_DATA && data_len > 0) {
        if (data_len > s_ws_frame_capacity) {
            ESP_LOGW(TAG, "invalid websocket frame size: %u", (unsigned)data_len);
            return;
        }
        memcpy(s_ws_frame, data, data_len);
        parse_protocol_frame(s_ws_frame, data_len);
    }
}

static void capture_task(void *arg)
{
    (void)arg;
    int16_t samples[AUDIO_CLOUD_SAMPLES];
    while (true) {
        esp_err_t err = audio_board_read_cloud_packet(samples);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "microphone read: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (s_recording && s_ready) {
            if (send_event(EVT_TASK_REQUEST, samples, sizeof(samples), true) < 0) {
                ESP_LOGW(TAG, "audio upload failed");
            }
        }
    }
}

esp_err_t doubao_realtime_start(void)
{
    s_send_lock = xSemaphoreCreateMutex();
    if (s_send_lock == NULL) return ESP_ERR_NO_MEM;
    s_control_queue = xQueueCreate(4, sizeof(int));
    if (s_control_queue == NULL) return ESP_ERR_NO_MEM;
    if (xTaskCreatePinnedToCore(control_task, "doubao_ctrl", 6144, NULL, 6, NULL, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    s_ws_frame_capacity = 128 * 1024;
    s_ws_frame = heap_caps_malloc(s_ws_frame_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_ws_frame == NULL) {
        s_ws_frame = heap_caps_malloc(s_ws_frame_capacity, MALLOC_CAP_8BIT);
    }
    if (s_ws_frame == NULL) return ESP_ERR_NO_MEM;

    make_uuid(s_connect_id);
#if CONFIG_DOUBAO_AUTH_API_KEY
    if (strlen(CONFIG_DOUBAO_API_KEY) == 0) {
        ESP_LOGE(TAG, "DOUBAO_API_KEY is empty; open menuconfig and configure it");
        display_ui_set_status("SET DOUBAO API KEY IN MENUCONFIG");
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(s_headers, sizeof(s_headers),
             "X-Api-Key: %s\r\n"
             "X-Api-Resource-Id: %s\r\n"
             "X-Api-Connect-Id: %s\r\n",
             CONFIG_DOUBAO_API_KEY, CONFIG_DOUBAO_RESOURCE_ID, s_connect_id);
#else
    if (strlen(CONFIG_DOUBAO_APP_ID) == 0 || strlen(CONFIG_DOUBAO_ACCESS_TOKEN) == 0) {
        ESP_LOGE(TAG, "legacy App ID or Access Token is empty");
        display_ui_set_status("SET DOUBAO CREDENTIALS");
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(s_headers, sizeof(s_headers),
             "X-Api-App-ID: %s\r\n"
             "X-Api-Access-Key: %s\r\n"
             "X-Api-Resource-Id: %s\r\n"
             "X-Api-App-Key: PlgvMymc7f3tQnJ6\r\n"
             "X-Api-Connect-Id: %s\r\n",
             CONFIG_DOUBAO_APP_ID, CONFIG_DOUBAO_ACCESS_TOKEN,
             CONFIG_DOUBAO_RESOURCE_ID, s_connect_id);
#endif

    ESP_RETURN_ON_ERROR(simple_ws_start(DOUBAO_HOST, DOUBAO_PATH, s_headers,
                                        websocket_event_handler, NULL),
                        TAG, "start WebSocket client");

    if (xTaskCreatePinnedToCore(capture_task, "audio_capture", 6144, NULL, 7, NULL, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool doubao_realtime_ready(void)
{
    return s_ready;
}

void doubao_realtime_set_push_to_talk(bool pressed)
{
    if (!s_ready || pressed == s_recording) return;

    if (pressed) {
        audio_board_clear_playback();
        static const char empty_json[] = "{}";
        send_event(EVT_CLIENT_INTERRUPT, empty_json, 2, false);
        display_ui_begin_assistant_text();
        display_ui_set_user_text("Listening...");
        display_ui_set_status("RECORDING - RELEASE BOOT TO SEND");
        s_recording = true;
        ESP_LOGI(TAG, "push-to-talk started");
    } else {
        s_recording = false;
        static const char empty_json[] = "{}";
        send_event(EVT_END_ASR, empty_json, 2, false);
        display_ui_set_status("SENT - RECOGNIZING...");
        ESP_LOGI(TAG, "push-to-talk ended");
    }
}
