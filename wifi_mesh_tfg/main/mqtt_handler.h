#pragma once

#include <stdbool.h>
#include "protocol.h"
#include "esp_err.h"

/* ─── Topics MQTT ────────────────────────────────────────────────────────── */
#define MQTT_TOPIC_METRICS_FMT  "iot/mesh/node/%02x%02x%02x%02x%02x%02x/metrics"
#define MQTT_TOPIC_TOPO         "iot/mesh/topology"
#define MQTT_TOPIC_PING_FMT     "iot/mesh/node/%02x%02x%02x%02x%02x%02x/ping"
#define MQTT_TOPIC_STATUS       "iot/mesh/status"

/**
 * @brief Arranca el cliente MQTT (solo debe llamarse en el nodo root).
 *        Bloquea hasta que la conexión es exitosa o agota reintentos.
 */
esp_err_t mqtt_handler_start(void);

/**
 * @brief Publica las métricas de un nodo en el broker.
 *
 * @param mac  MAC del nodo origen (6 bytes).
 * @param m    Payload de métricas a serializar como JSON.
 */
void mqtt_publish_metrics(const uint8_t *mac, const metrics_payload_t *m,
                          uint32_t seq);


/**
 * @brief Devuelve true si el cliente MQTT está conectado al broker.
 */
bool mqtt_is_connected(void);



