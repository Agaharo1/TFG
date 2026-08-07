#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

esp_err_t peripherals_init(void);

void peripherals_start_button_task(void);


uint32_t peripherals_get_power_mw(void);


void peripherals_get_i2c_raw(char *dest, size_t max_len);

uint8_t peripherals_get_ps_mode(void);