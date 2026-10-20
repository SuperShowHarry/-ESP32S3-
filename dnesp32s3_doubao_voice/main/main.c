#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "app_config.h"
#include "audio_board.h"
#include "board_io.h"
#include "display_ui.h"
#include "doubao_realtime.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "wifi_station.h"

static const char *TAG = "app";

static void stop_with_error(const char *stage, esp_err_t err)
{
    char message[96];
    snprintf(message, sizeof(message), "%s FAILED: %s", stage, esp_err_to_name(err));
    ESP_LOGE(TAG, "%s failed: %s (0x%x)", stage, esp_err_to_name(err), (unsigned)err);
    display_ui_set_status(message);
    gpio_set_level(BOARD_LED, 0);
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

static void init_button_and_led(void)
{
    gpio_config_t button_cfg = {
        .pin_bit_mask = 1ULL << BOARD_BOOT_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&button_cfg));

    gpio_config_t led_cfg = {
        .pin_bit_mask = 1ULL << BOARD_LED,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&led_cfg));
    gpio_set_level(BOARD_LED, 1);
}

void app_main(void)
{
    init_nvs();
    init_button_and_led();

    esp_err_t err = board_io_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "board I/O init failed: %s (0x%x)", esp_err_to_name(err), (unsigned)err);
        while (true) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    err = display_ui_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "display init failed: %s (0x%x)", esp_err_to_name(err), (unsigned)err);
        while (true) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    display_ui_set_status("INIT ES8388 AUDIO...");
    err = audio_board_init();
    if (err != ESP_OK) stop_with_error("ES8388 AUDIO", err);
    audio_board_play_startup_tone();

    if (strcmp(CONFIG_APP_WIFI_SSID, "YOUR_WIFI_NAME") == 0) {
        ESP_LOGE(TAG, "Wi-Fi is not configured. Use idf.py menuconfig.");
        display_ui_set_status("SET WI-FI IN MENUCONFIG");
        return;
    }
    err = wifi_station_connect();
    if (err != ESP_OK) stop_with_error("WI-FI", err);
    err = doubao_realtime_start();
    if (err != ESP_OK) stop_with_error("DOUBAO", err);

    bool stable_pressed = false;
    bool last_sample = false;
    int same_count = 0;
    while (true) {
        bool sample = gpio_get_level(BOARD_BOOT_BUTTON) == 0;
        if (sample == last_sample) {
            if (same_count < 4) ++same_count;
        } else {
            last_sample = sample;
            same_count = 0;
        }
        if (same_count >= 3 && stable_pressed != sample) {
            stable_pressed = sample;
            gpio_set_level(BOARD_LED, stable_pressed ? 0 : 1);
            doubao_realtime_set_push_to_talk(stable_pressed);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
