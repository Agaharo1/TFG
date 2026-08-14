#pragma once

#include <stdbool.h>
#include "protocol.h"
#include "esp_err.h"


esp_err_t metrics_init(void);


void metrics_collect(metrics_payload_t *m);


void metrics_record_tx(bool success);

void metrics_record_rx(void);


void metrics_update_latency(uint32_t rtt_ms);


uint32_t metrics_get_current_power(void);


void metrics_record_seq(const uint8_t *mac, uint32_t seq);

float metrics_get_pdr_pct(const uint8_t *mac);

