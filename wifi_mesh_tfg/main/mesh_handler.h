#pragma once

#include "esp_err.h"
#include "esp_mesh.h"


esp_err_t mesh_handler_init(void);


bool mesh_is_ready(void);


void metrics_record_ping_loss(void);