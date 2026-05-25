#pragma once

#include "esp_err.h"
#include "esp_mesh.h"

/**
 * @brief Inicializa y arranca la red WiFi + ESP-MESH.
 *        - Configura el SSID/password de la red mesh y del router.
 *        - Registra los event handlers necesarios.
 *        - Lanza la tarea de recepción y la de envío periódico.
 *
 * @return ESP_OK si todo fue bien.
 */
esp_err_t mesh_handler_init(void);

/**
 * @brief Devuelve true cuando el nodo está conectado a la red mesh
 *        (y el root además tiene IP de router).
 */
bool mesh_is_ready(void);


void metrics_record_ping_loss(void);