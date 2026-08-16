#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>


esp_err_t mqtt_handler_start(void);

bool mqtt_is_connected(void);


void mqtt_publish_ping_result(uint16_t node_addr,
                               uint16_t received, uint16_t expected,
                               float loss_pct,
                               int16_t rssi_min, float rssi_avg, int16_t rssi_max,
                               float snr_estimate_avg);


void mqtt_publish_transfer_result(uint16_t node_addr,
                                   uint32_t total_bytes_observed,
                                   uint16_t total_chunks, uint16_t received_chunks,
                                   uint16_t crc_fail_count, float loss_pct,
                                   int16_t rssi_min, float rssi_avg, int16_t rssi_max,
                                   float snr_estimate_avg);