#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t audio_board_init(void);
esp_err_t audio_board_read_cloud_packet(int16_t out_samples[320]);
void audio_board_queue_playback(const uint8_t *pcm_s16le, size_t length);
void audio_board_clear_playback(void);
void audio_board_play_startup_tone(void);

