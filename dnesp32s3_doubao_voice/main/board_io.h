#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t board_io_init(void);
/* Returns an IDF 5.3+ i2c_master_bus_handle_t as an opaque pointer.
 * On IDF 5.1/5.2 the legacy I2C driver is used and this returns NULL. */
void *board_io_i2c_bus(void);
esp_err_t board_io_xl9555_set(uint8_t bit, bool level);

