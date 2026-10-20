#include "board_io.h"

#include <stdbool.h>
#include "app_config.h"
#include "esp_check.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
#include "driver/i2c_master.h"
#else
#include "driver/i2c.h"
#endif

static const char *TAG = "board_io";
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_xl9555;
#endif
static SemaphoreHandle_t s_lock;
static uint16_t s_output = 0xffff;
static bool s_initialized;

static esp_err_t xl9555_write_reg(uint8_t reg, uint8_t value)
{
    uint8_t packet[2] = {reg, value};
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
    return i2c_master_transmit(s_xl9555, packet, sizeof(packet), 1000);
#else
    return i2c_master_write_to_device(BOARD_I2C_PORT, BOARD_XL9555_ADDR,
                                      packet, sizeof(packet), pdMS_TO_TICKS(1000));
#endif
}

esp_err_t board_io_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = BOARD_I2C_PORT,
        .sda_io_num = BOARD_I2C_SDA,
        .scl_io_num = BOARD_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_bus), TAG, "create I2C bus");

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BOARD_XL9555_ADDR,
        .scl_speed_hz = BOARD_I2C_FREQ_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_bus, &dev_cfg, &s_xl9555),
                        TAG, "add XL9555");
#else
    /* ESP-IDF 5.1.2 uses the legacy I2C driver. esp_codec_dev 1.3.2
     * automatically uses the same driver on IDF versions below 5.3. */
    i2c_config_t bus_cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = BOARD_I2C_SDA,
        .scl_io_num = BOARD_I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = BOARD_I2C_FREQ_HZ,
        .clk_flags = 0,
    };
    ESP_RETURN_ON_ERROR(i2c_param_config(BOARD_I2C_PORT, &bus_cfg),
                        TAG, "configure legacy I2C bus");
    ESP_RETURN_ON_ERROR(i2c_driver_install(BOARD_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0),
                        TAG, "install legacy I2C driver");
#endif

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* P0.0..P0.1 input, P0.2..P0.7 output; P1.0..P1.3 output. */
    ESP_RETURN_ON_ERROR(xl9555_write_reg(0x06, 0x03), TAG, "XL9555 config P0");
    ESP_RETURN_ON_ERROR(xl9555_write_reg(0x07, 0xf0), TAG, "XL9555 config P1");
    ESP_RETURN_ON_ERROR(xl9555_write_reg(0x02, (uint8_t)s_output), TAG, "XL9555 output P0");
    ESP_RETURN_ON_ERROR(xl9555_write_reg(0x03, (uint8_t)(s_output >> 8)), TAG, "XL9555 output P1");

    s_initialized = true;
    ESP_LOGI(TAG, "I2C and XL9555 ready");
    return ESP_OK;
}

void *board_io_i2c_bus(void)
{
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
    return s_bus;
#else
    return NULL;
#endif
}

esp_err_t board_io_xl9555_set(uint8_t bit, bool level)
{
    if (!s_initialized || bit > 15) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (level) {
        s_output |= (uint16_t)(1u << bit);
    } else {
        s_output &= (uint16_t)~(1u << bit);
    }
    uint8_t reg = bit < 8 ? 0x02 : 0x03;
    uint8_t value = bit < 8 ? (uint8_t)s_output : (uint8_t)(s_output >> 8);
    esp_err_t err = xl9555_write_reg(reg, value);
    xSemaphoreGive(s_lock);
    return err;
}
