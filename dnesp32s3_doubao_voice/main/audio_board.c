#include "audio_board.h"

#include <stdlib.h>
#include <string.h>
#include "app_config.h"
#include "board_io.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "audio";

static i2s_chan_handle_t s_tx_chan;
static i2s_chan_handle_t s_rx_chan;
static esp_codec_dev_handle_t s_input_dev;
static esp_codec_dev_handle_t s_output_dev;

#define PLAY_CHUNK_BYTES 1024
typedef struct {
    uint16_t length;
    uint8_t data[PLAY_CHUNK_BYTES];
} play_chunk_t;

static QueueHandle_t s_play_queue;

static void playback_task(void *arg)
{
    (void)arg;
    play_chunk_t chunk;
    int16_t stereo[PLAY_CHUNK_BYTES / sizeof(int16_t) * 2];
    while (true) {
        if (xQueueReceive(s_play_queue, &chunk, portMAX_DELAY) == pdTRUE && chunk.length > 0) {
            /* Cloud TTS is mono, while the DNESP32S3 ES8388/amp path is wired
             * like the vendor music example and expects both I2S slots. */
            const int16_t *mono = (const int16_t *)chunk.data;
            size_t sample_count = chunk.length / sizeof(int16_t);
            for (size_t i = 0; i < sample_count; ++i) {
                stereo[i * 2] = mono[i];
                stereo[i * 2 + 1] = mono[i];
            }
            esp_err_t err = esp_codec_dev_write(s_output_dev, stereo,
                                                sample_count * 2 * sizeof(int16_t));
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "playback write failed: %s", esp_err_to_name(err));
            }
        }
    }
}

esp_err_t audio_board_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(BOARD_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = 480;
    /* auto_clear is the field name in ESP-IDF 5.1.2 and remains a
     * compatibility alias in the later 5.x releases. */
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx_chan, &s_rx_chan),
                        TAG, "create I2S channels");

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = AUDIO_HW_SAMPLE_RATE,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = I2S_STD_SLOT_BOTH,
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = true,
            .big_endian = false,
            .bit_order_lsb = false,
        },
        .gpio_cfg = {
            .mclk = BOARD_I2S_MCLK,
            .bclk = BOARD_I2S_BCLK,
            .ws = BOARD_I2S_WS,
            .dout = BOARD_I2S_DOUT,
            .din = BOARD_I2S_DIN,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx_chan, &std_cfg), TAG, "init I2S RX");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx_chan, &std_cfg), TAG, "init I2S TX");

    audio_codec_i2s_cfg_t data_cfg = {
        .port = BOARD_I2S_PORT,
        .rx_handle = s_rx_chan,
        .tx_handle = s_tx_chan,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&data_cfg);

    audio_codec_i2c_cfg_t ctrl_cfg = {
        .port = BOARD_I2C_PORT,
        .addr = BOARD_ES8388_ADDR,
        .bus_handle = board_io_i2c_bus(),
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&ctrl_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    if (data_if == NULL || ctrl_if == NULL || gpio_if == NULL) {
        return ESP_ERR_NO_MEM;
    }

    es8388_codec_cfg_t codec_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        /* ESP32-S3 provides MCLK/BCLK/WS, so ES8388 must be the I2S slave.
         * Enabling master mode on both ends can make codec open fail and the
         * old ESP_ERROR_CHECK in app_main then caused an endless reboot. */
        .master_mode = false,
        .pa_pin = GPIO_NUM_NC,
        .pa_reverted = false,
        .hw_gain = {
            .pa_voltage = 5.0,
            .codec_dac_voltage = 3.3,
        },
    };
    const audio_codec_if_t *codec_if = es8388_codec_new(&codec_cfg);
    if (codec_if == NULL) {
        return ESP_FAIL;
    }

    esp_codec_dev_cfg_t in_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    esp_codec_dev_cfg_t out_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    s_input_dev = esp_codec_dev_new(&in_cfg);
    s_output_dev = esp_codec_dev_new(&out_cfg);
    if (s_input_dev == NULL || s_output_dev == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_codec_dev_sample_info_t input_info = {
        .bits_per_sample = 16,
        .channel = 1,
        .channel_mask = 0,
        .sample_rate = AUDIO_HW_SAMPLE_RATE,
        .mclk_multiple = 0,
    };
    esp_codec_dev_sample_info_t output_info = input_info;
    output_info.channel = 2;
    output_info.channel_mask = 0;
    ESP_RETURN_ON_ERROR(esp_codec_dev_open(s_input_dev, &input_info), TAG, "open ES8388 input");
    ESP_RETURN_ON_ERROR(esp_codec_dev_open(s_output_dev, &output_info), TAG, "open ES8388 output");
    ESP_RETURN_ON_ERROR(esp_codec_dev_set_in_gain(s_input_dev, 40.0), TAG, "set microphone gain");
    ESP_RETURN_ON_ERROR(esp_codec_dev_set_out_vol(s_output_dev, CONFIG_AUDIO_OUTPUT_VOLUME),
                        TAG, "set speaker volume");

    /* esp_codec_dev's ES8388 default leaves LOUT1/ROUT1/LOUT2/ROUT2 at
     * register value 0x00 (-30 dB).  Digital volume 100 alone therefore still
     * sounds very quiet.  0x21 is the ES8388 analogue-output maximum (+3 dB). */
    for (int reg = 0x2e; reg <= 0x31; ++reg) {
        int maximum_analog_gain = 0x21;
        if (ctrl_if->write_reg(ctrl_if, reg, 1, &maximum_analog_gain, 1) != 0) {
            ESP_LOGE(TAG, "failed to set ES8388 analogue output reg 0x%02x", reg);
            return ESP_FAIL;
        }
    }
    ESP_LOGI(TAG, "ES8388 analogue outputs set to maximum (+3 dB)");

    /* V1.2 schematic: XL9555 P0.2 drives pin 1 (SHUTDOWN) of
     * the MD8002A-compatible amplifier.  SHUTDOWN is active high, so a low
     * level enables normal speaker operation. */
    ESP_RETURN_ON_ERROR(board_io_xl9555_set(XL9555_SPK_EN_BIT, false), TAG, "enable speaker");

    s_play_queue = xQueueCreate(12, sizeof(play_chunk_t));
    if (s_play_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreatePinnedToCore(playback_task, "audio_play", 4096, NULL, 6, NULL, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "ES8388 ready: duplex 24 kHz, mono application stream");
    return ESP_OK;
}

esp_err_t audio_board_read_cloud_packet(int16_t out_samples[AUDIO_CLOUD_SAMPLES])
{
    int16_t input[AUDIO_HW_SAMPLES];
    esp_err_t err = esp_codec_dev_read(s_input_dev, input, sizeof(input));
    if (err != ESP_OK) {
        return err;
    }

    /* 24 kHz -> 16 kHz. Every three source samples become two output samples. */
    for (int i = 0; i < AUDIO_HW_SAMPLES / 3; ++i) {
        int src = i * 3;
        int dst = i * 2;
        out_samples[dst] = input[src];
        out_samples[dst + 1] = (int16_t)(((int32_t)input[src + 1] + input[src + 2]) / 2);
    }
    return ESP_OK;
}

void audio_board_queue_playback(const uint8_t *pcm_s16le, size_t length)
{
    if (s_play_queue == NULL || pcm_s16le == NULL) {
        return;
    }
    while (length >= 2) {
        play_chunk_t chunk = {0};
        size_t amount = length > PLAY_CHUNK_BYTES ? PLAY_CHUNK_BYTES : length;
        amount &= ~(size_t)1;
        chunk.length = (uint16_t)amount;
        memcpy(chunk.data, pcm_s16le, amount);
        /* Apply back-pressure instead of dropping most of a burst of TTS PCM.
         * One 1024-byte chunk is about 21 ms at 24 kHz mono s16le. */
        if (xQueueSend(s_play_queue, &chunk, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGW(TAG, "playback queue full; dropping %u bytes", (unsigned)amount);
            return;
        }
        pcm_s16le += amount;
        length -= amount;
    }
}

void audio_board_clear_playback(void)
{
    if (s_play_queue != NULL) {
        xQueueReset(s_play_queue);
    }
}

void audio_board_play_startup_tone(void)
{
    /* Short 750 Hz square-wave chime.  It verifies I2S, ES8388, amplifier and
     * speaker independently of Wi-Fi and the cloud TTS response. */
    int16_t samples[480];
    for (int block = 0; block < 15; ++block) {
        for (int i = 0; i < 480; ++i) {
            int phase = (block * 480 + i) % 32;
            samples[i] = phase < 16 ? 20000 : -20000;
        }
        audio_board_queue_playback((const uint8_t *)samples, sizeof(samples));
    }
    ESP_LOGI(TAG, "startup speaker test tone queued");
}
