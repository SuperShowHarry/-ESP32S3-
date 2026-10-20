#pragma once

#include <stdbool.h>
#include "esp_err.h"

esp_err_t doubao_realtime_start(void);
bool doubao_realtime_ready(void);
void doubao_realtime_set_push_to_talk(bool pressed);

