#include "display_ui.h"

#include <stdio.h>
#include <string.h>
#include "app_config.h"
#include "board_io.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

LV_FONT_DECLARE(lv_font_simhei_16_full);

static const char *TAG = "display";
static SemaphoreHandle_t s_lvgl_lock;
static esp_lcd_panel_handle_t s_panel;
static lv_disp_drv_t s_disp_drv;
static lv_obj_t *s_status_label;
static lv_obj_t *s_user_label;
static lv_obj_t *s_assistant_label;
static char s_assistant_text[1536];

static bool lcd_color_done(esp_lcd_panel_io_handle_t panel_io,
                           esp_lcd_panel_io_event_data_t *edata,
                           void *user_ctx)
{
    (void)panel_io;
    (void)edata;
    lv_disp_flush_ready((lv_disp_drv_t *)user_ctx);
    return false;
}

static void lvgl_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *pixels)
{
    esp_err_t err = esp_lcd_panel_draw_bitmap((esp_lcd_panel_handle_t)drv->user_data,
                                               area->x1, area->y1,
                                               area->x2 + 1, area->y2 + 1,
                                               pixels);
    if (err != ESP_OK) {
        lv_disp_flush_ready(drv);
    }
}

static void lvgl_tick(void *arg)
{
    (void)arg;
    lv_tick_inc(2);
}

static void lvgl_task(void *arg)
{
    (void)arg;
    while (true) {
        uint32_t wait_ms = 10;
        if (xSemaphoreTakeRecursive(s_lvgl_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
            wait_ms = lv_timer_handler();
            xSemaphoreGiveRecursive(s_lvgl_lock);
        }
        if (wait_ms < 2) wait_ms = 2;
        if (wait_ms > 20) wait_ms = 20;
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
    }
}

static void label_common(lv_obj_t *label, int x, int y, int w, int h)
{
    lv_obj_set_pos(label, x, y);
    lv_obj_set_size(label, w, h);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    /* LVGL's bundled SimSun font contains only a small 1166-glyph subset.
     * The assistant can return arbitrary Chinese, so use the generated full
     * CJK font to prevent missing-glyph boxes/garbled conversation text. */
    lv_obj_set_style_text_font(label, &lv_font_simhei_16_full, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0xe8edf7), 0);
    lv_obj_set_style_pad_all(label, 7, 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(label, lv_color_hex(0x172033), 0);
    lv_obj_set_style_radius(label, 8, 0);
}

static void create_ui(void)
{
    lv_obj_t *screen = lv_scr_act();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0b1020), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "DOUBAO VOICE");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x7dd3fc), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    s_status_label = lv_label_create(screen);
    lv_label_set_text(s_status_label, "STARTING...");
    lv_obj_set_style_text_font(s_status_label, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0xfbbf24), 0);
    lv_obj_align(s_status_label, LV_ALIGN_TOP_MID, 0, 34);

    s_user_label = lv_label_create(screen);
    label_common(s_user_label, 8, 60, BOARD_LCD_WIDTH - 16, 64);
    lv_label_set_text(s_user_label, "YOU: Waiting for speech");

    s_assistant_label = lv_label_create(screen);
    label_common(s_assistant_label, 8, 130, BOARD_LCD_WIDTH - 16, 80);
    lv_obj_set_style_bg_color(s_assistant_label, lv_color_hex(0x122b2a), 0);
    lv_label_set_text(s_assistant_label, "AI: Hold BOOT to talk. Release it to send.");

    lv_obj_t *hint = lv_label_create(screen);
    lv_label_set_text(hint, "HOLD BOOT TO TALK");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x94a3b8), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -7);
}

esp_err_t display_ui_init(void)
{
    spi_bus_config_t bus_cfg = {
        .sclk_io_num = BOARD_LCD_SCLK,
        .mosi_io_num = BOARD_LCD_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = BOARD_LCD_WIDTH * 48 * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(BOARD_LCD_HOST, &bus_cfg, SPI_DMA_CH_AUTO),
                        TAG, "init LCD SPI bus");

    esp_lcd_panel_io_handle_t panel_io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = BOARD_LCD_CS,
        .dc_gpio_num = BOARD_LCD_DC,
        .spi_mode = 0,
        /* 20 MHz is more tolerant of the board/cable signal quality and keeps
         * glyph edges stable. 40 MHz can produce fuzzy or torn pixels. */
        .pclk_hz = 20 * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BOARD_LCD_HOST,
                                                 &io_cfg, &panel_io),
                        TAG, "create LCD panel IO");

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        /* LVGL stores RGB565 pixels little-endian on ESP32-S3.  Matching the
         * controller order avoids cyan/magenta fringes on antialiased text. */
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(panel_io, &panel_cfg, &s_panel),
                        TAG, "create ST7789 panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "reset LCD");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "init LCD");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, true), TAG, "invert LCD colors");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(s_panel, true), TAG, "LCD landscape");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel, true, false), TAG, "LCD mirror");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(s_panel, BOARD_LCD_X_GAP, 0), TAG, "LCD gap");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "LCD on");
    ESP_RETURN_ON_ERROR(board_io_xl9555_set(XL9555_LCD_BL_BIT, true), TAG, "LCD backlight");

    lv_init();
    /* Keep only a compact single DMA buffer.  The old pair of 40-line buffers
     * consumed 50 KiB of scarce internal RAM and prevented the TLS/WebSocket
     * worker task from being created after Wi-Fi came up. */
    const size_t draw_rows = 20;
    lv_color_t *buffer1 = heap_caps_malloc(BOARD_LCD_WIDTH * draw_rows * sizeof(lv_color_t),
                                           MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (buffer1 == NULL) {
        return ESP_ERR_NO_MEM;
    }

    static lv_disp_draw_buf_t draw_buf;
    lv_disp_draw_buf_init(&draw_buf, buffer1, NULL, BOARD_LCD_WIDTH * draw_rows);
    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res = BOARD_LCD_WIDTH;
    s_disp_drv.ver_res = BOARD_LCD_HEIGHT;
    s_disp_drv.flush_cb = lvgl_flush;
    s_disp_drv.draw_buf = &draw_buf;
    s_disp_drv.user_data = s_panel;
    lv_disp_drv_register(&s_disp_drv);

    esp_lcd_panel_io_callbacks_t io_callbacks = {
        .on_color_trans_done = lcd_color_done,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_register_event_callbacks(panel_io, &io_callbacks, &s_disp_drv),
                        TAG, "register LCD callback");

    s_lvgl_lock = xSemaphoreCreateRecursiveMutex();
    if (s_lvgl_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    create_ui();

    const esp_timer_create_args_t tick_args = {
        .callback = lvgl_tick,
        .name = "lvgl_tick",
    };
    esp_timer_handle_t tick_timer;
    ESP_RETURN_ON_ERROR(esp_timer_create(&tick_args, &tick_timer), TAG, "create LVGL tick");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(tick_timer, 2000), TAG, "start LVGL tick");
    if (xTaskCreatePinnedToCore(lvgl_task, "lvgl", 6144, NULL, 3, NULL, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "ST7789 320x240 display ready");
    return ESP_OK;
}

void display_ui_set_status(const char *status)
{
    if (s_lvgl_lock == NULL || status == NULL) return;
    if (xSemaphoreTakeRecursive(s_lvgl_lock, pdMS_TO_TICKS(200)) == pdTRUE) {
        lv_label_set_text(s_status_label, status);
        xSemaphoreGiveRecursive(s_lvgl_lock);
    }
}

void display_ui_set_user_text(const char *text)
{
    if (s_lvgl_lock == NULL || text == NULL) return;
    if (xSemaphoreTakeRecursive(s_lvgl_lock, pdMS_TO_TICKS(200)) == pdTRUE) {
        char line[640];
        snprintf(line, sizeof(line), "YOU: %s", text);
        lv_label_set_text(s_user_label, line);
        xSemaphoreGiveRecursive(s_lvgl_lock);
    }
}

void display_ui_begin_assistant_text(void)
{
    if (s_lvgl_lock == NULL) return;
    if (xSemaphoreTakeRecursive(s_lvgl_lock, pdMS_TO_TICKS(200)) == pdTRUE) {
        s_assistant_text[0] = '\0';
        lv_label_set_text(s_assistant_label, "AI: ");
        xSemaphoreGiveRecursive(s_lvgl_lock);
    }
}

void display_ui_append_assistant_text(const char *fragment)
{
    if (s_lvgl_lock == NULL || fragment == NULL) return;
    if (xSemaphoreTakeRecursive(s_lvgl_lock, pdMS_TO_TICKS(200)) == pdTRUE) {
        size_t used = strlen(s_assistant_text);
        if (used < sizeof(s_assistant_text) - 1) {
            strlcpy(s_assistant_text + used, fragment, sizeof(s_assistant_text) - used);
        }
        char line[1600];
        snprintf(line, sizeof(line), "AI: %s", s_assistant_text);
        lv_label_set_text(s_assistant_label, line);
        xSemaphoreGiveRecursive(s_lvgl_lock);
    }
}
