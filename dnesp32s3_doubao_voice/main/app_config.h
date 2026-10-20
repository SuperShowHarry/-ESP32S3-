#pragma once

#include "driver/gpio.h"
#include "driver/i2c.h"

/* Pins verified against ATK_DNESP32S3_V1.2 and the vendor BSP. */
#define BOARD_I2C_PORT          I2C_NUM_0
#define BOARD_I2C_SDA           GPIO_NUM_41
#define BOARD_I2C_SCL           GPIO_NUM_42
#define BOARD_I2C_FREQ_HZ       400000

#define BOARD_XL9555_ADDR       0x20
/* esp_codec_dev uses the 8-bit I2C write address on ESP-IDF 5.1.x. */
#define BOARD_ES8388_ADDR       0x20

#define BOARD_I2S_PORT          I2S_NUM_0
#define BOARD_I2S_MCLK          GPIO_NUM_3
#define BOARD_I2S_BCLK          GPIO_NUM_46
#define BOARD_I2S_WS            GPIO_NUM_9
#define BOARD_I2S_DOUT          GPIO_NUM_10
#define BOARD_I2S_DIN           GPIO_NUM_14

#define BOARD_LCD_HOST          SPI2_HOST
#define BOARD_LCD_SCLK          GPIO_NUM_12
#define BOARD_LCD_MOSI          GPIO_NUM_11
#define BOARD_LCD_CS            GPIO_NUM_21
#define BOARD_LCD_DC            GPIO_NUM_40
#if CONFIG_APP_SPI_LCD_240X240
#define BOARD_LCD_WIDTH         240
#define BOARD_LCD_HEIGHT        240
#define BOARD_LCD_X_GAP         80
#else
#define BOARD_LCD_WIDTH         320
#define BOARD_LCD_HEIGHT        240
#define BOARD_LCD_X_GAP         0
#endif

#define BOARD_BOOT_BUTTON       GPIO_NUM_0
#define BOARD_LED               GPIO_NUM_1

/* XL9555 output bit numbers from the schematic. */
#define XL9555_SPK_EN_BIT       2
#define XL9555_LCD_BL_BIT       8

#define AUDIO_HW_SAMPLE_RATE    24000
#define AUDIO_CLOUD_SAMPLE_RATE 16000
#define AUDIO_PACKET_MS         20
#define AUDIO_CLOUD_SAMPLES     320
#define AUDIO_HW_SAMPLES        480
