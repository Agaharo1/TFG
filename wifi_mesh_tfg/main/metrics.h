#pragma once

#include <stdbool.h>
#include "protocol.h"
#include "esp_err.h"

/**
 * @brief Inicializa el módulo de métricas.
 *        Debe llamarse tras esp_mesh_start().
 */
esp_err_t metrics_init(void);

/**
 * @brief Rellena una estructura metrics_payload_t con los valores actuales.
 *        Internamente consulta la capa mesh, RSSI al padre, heap, etc.
 *
 * @param[out] m  Puntero a la estructura a rellenar.
 */
void metrics_collect(metrics_payload_t *m);

/**
 * @brief Incrementa el contador de TX.  Llamar tras cada esp_mesh_send().
 * @param success  true si el envío fue OK, false si falló.
 */
void metrics_record_tx(bool success);

/**
 * @brief Incrementa el contador de RX.  Llamar tras cada esp_mesh_recv().
 */
void metrics_record_rx(void);

/**
 * @brief Actualiza la latencia RTT tras recibir un PONG.
 * @param rtt_ms  Tiempo de ida y vuelta en milisegundos.
 */
void metrics_update_latency(uint32_t rtt_ms);
