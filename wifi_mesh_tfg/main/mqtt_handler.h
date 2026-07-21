#pragma once

#include <stdbool.h>
#include "protocol.h"
#include "esp_err.h"

#define MQTT_TOPIC_METRICS_FMT  "iot/mesh/node/%02x%02x%02x%02x%02x%02x/metrics"
#define MQTT_TOPIC_TOPO         "iot/mesh/topology"
#define MQTT_TOPIC_PING_FMT     "iot/mesh/node/%02x%02x%02x%02x%02x%02x/ping"
#define MQTT_TOPIC_STATUS       "iot/mesh/status"


esp_err_t mqtt_handler_start(void);


void mqtt_publish_metrics(const uint8_t *mac, const metrics_payload_t *m,
                          uint32_t seq);


void mqtt_publish_exp_result(const uint8_t *src_mac, const exp_packet_t *exp);


bool mqtt_is_connected(void);



