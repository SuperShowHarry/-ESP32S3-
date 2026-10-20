#pragma once

#include "esp_err.h"

esp_err_t display_ui_init(void);
void display_ui_set_status(const char *status);
void display_ui_set_user_text(const char *text);
void display_ui_begin_assistant_text(void);
void display_ui_append_assistant_text(const char *fragment);

