#include "metrics.h"
#include "peripherals.h"
#include "esp_mesh.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "METRICS";

static SemaphoreHandle_t s_mutex = NULL;
static uint32_t s_tx_count        = 0;
static uint32_t s_rx_count        = 0;
static uint32_t s_tx_fail         = 0;
static uint32_t s_latency_ms      = 0;   /* último RTT medido */
static uint32_t s_ping_lost_count = 0;

#define NOISE_FLOOR_DBM  (-95)

esp_err_t metrics_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        ESP_LOGE(TAG, "No se pudo crear el mutex de métricas");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Módulo de métricas inicializado");
    return ESP_OK;
}

uint32_t metrics_get_current_power(void)
{
    // Obtiene el valor de potencia actualizado desde el módulo de periféricos
    return peripherals_get_power_mw();
}

void metrics_record_ping_loss(void)
{
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_ping_lost_count++;
    xSemaphoreGive(s_mutex);
}

void metrics_collect(metrics_payload_t *m)
{
    if (!m) return;
    memset(m, 0, sizeof(*m));

    // 1. Datos de topología
    m->layer        = (uint8_t)esp_mesh_get_layer();
    m->hops_to_root = m->layer;       

    // 2. Subnodos conectados en la tabla de ruteo
    #define MAX_CHILD_NODES 50  
    mesh_addr_t child_table[MAX_CHILD_NODES];
    int child_num = 0;
    esp_mesh_get_routing_table(child_table,
                               MAX_CHILD_NODES * sizeof(mesh_addr_t),
                               &child_num);
   
    m->connected_subs = (uint8_t)(child_num > 0 ? child_num : 0);

    // 3. RSSI y estimación de SNR respecto al padre
    mesh_addr_t parent_addr;
    wifi_ap_record_t ap_info;

    if (esp_mesh_get_parent_bssid(&parent_addr) == ESP_OK) {
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            m->rssi_parent = ap_info.rssi;
            m->snr_estimate = (int8_t)(ap_info.rssi - NOISE_FLOOR_DBM);
        }
    }

    // 4. RSSI respecto al router (solo si el nodo es Root)
    if (esp_mesh_is_root()) {
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            m->rssi_router = ap_info.rssi;
        }
    }

    // 5. Contadores de paquetes y latencia
    if (s_mutex) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        m->tx_count        = s_tx_count;
        m->rx_count        = s_rx_count;
        m->tx_fail         = s_tx_fail;
        m->latency_ms      = s_latency_ms;
        m->ping_lost_count = s_ping_lost_count;
        xSemaphoreGive(s_mutex);
    }

    // 6. Cadena de texto I2C obtenida del módulo de periféricos
    peripherals_get_i2c_raw(m->i2c_raw, sizeof(m->i2c_raw));

    // 7. Métricas de recursos del sistema
    m->free_heap = esp_get_free_heap_size();
    m->uptime_s  = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    m->ps_mode = peripherals_get_ps_mode();
}

void metrics_record_tx(bool success)
{
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_tx_count++;
    if (!success) s_tx_fail++;
    xSemaphoreGive(s_mutex);
}

void metrics_record_rx(void)
{
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_rx_count++;
    xSemaphoreGive(s_mutex);
}

void metrics_update_latency(uint32_t rtt_ms)
{
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_latency_ms = rtt_ms;
    xSemaphoreGive(s_mutex);
}