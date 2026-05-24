#include "metrics.h"
#include "esp_mesh.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "METRICS";

/* ─── Contadores internos (protegidos por mutex) ─────────────────────────── */
static SemaphoreHandle_t s_mutex;
static uint32_t s_tx_count   = 0;
static uint32_t s_rx_count   = 0;
static uint32_t s_tx_fail    = 0;
static uint32_t s_latency_ms = 0;   /* último RTT medido */

/* ─── Noise floor empírico para estimar SNR ──────────────────────────────── */
#define NOISE_FLOOR_DBM  (-95)

/* ─── API pública ────────────────────────────────────────────────────────── */

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

void metrics_collect(metrics_payload_t *m)
{
    if (!m) return;
    memset(m, 0, sizeof(*m));

    /* ── Información de la capa mesh ────────────────────────────────────── */
    m->layer        = (uint8_t)esp_mesh_get_layer();
    m->hops_to_root = m->layer;          /* en árbol mesh layer == hops    */

    /* Número de nodos hijos directos */
    #define MAX_CHILD_NODES 50  /* Máximo tamaño de tabla de enrutamiento */
    mesh_addr_t child_table[MAX_CHILD_NODES];
    int child_num = 0;
    esp_mesh_get_routing_table(child_table,
                               MAX_CHILD_NODES * sizeof(mesh_addr_t),
                               &child_num);
    /* child_num incluye todos los descendientes; aproximamos con subs directos */
    m->connected_subs = (uint8_t)(child_num > 0 ? child_num : 0);

    /* ── RSSI al padre ──────────────────────────────────────────────────── */
    mesh_addr_t parent_addr;
    wifi_ap_record_t ap_info;

    if (esp_mesh_get_parent_bssid(&parent_addr) == ESP_OK) {
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            m->rssi_parent = ap_info.rssi;
            /* SNR = RSSI - noise_floor (estimación conservadora) */
            m->snr_estimate = (int8_t)(ap_info.rssi - NOISE_FLOOR_DBM);
        }
    }

    /* RSSI al router (solo tiene sentido en el root) */
    if (esp_mesh_is_root()) {
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            m->rssi_router = ap_info.rssi;
        }
    }

    /* ── Contadores con lock ─────────────────────────────────────────────── */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    m->tx_count   = s_tx_count;
    m->rx_count   = s_rx_count;
    m->tx_fail    = s_tx_fail;
    m->latency_ms = s_latency_ms;
    xSemaphoreGive(s_mutex);

    /* ── Sistema ─────────────────────────────────────────────────────────── */
    m->free_heap = esp_get_free_heap_size();
    m->uptime_s  = (uint32_t)(esp_timer_get_time() / 1000000ULL);
}

void metrics_record_tx(bool success)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_tx_count++;
    if (!success) s_tx_fail++;
    xSemaphoreGive(s_mutex);
}

void metrics_record_rx(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_rx_count++;
    xSemaphoreGive(s_mutex);
}

void metrics_update_latency(uint32_t rtt_ms)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_latency_ms = rtt_ms;
    xSemaphoreGive(s_mutex);
}
