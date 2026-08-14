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
static uint32_t s_latency_ms      = 0;   
static uint32_t s_ping_lost_count = 0;

#define NOISE_FLOOR_DBM  (-95)
#define MAX_TRACKED_NODES 20

typedef struct {
    uint8_t  mac[6];
    bool     in_use;
    uint32_t first_seq;
    uint32_t last_seq;
    uint32_t received_count;
} seq_track_t;

static seq_track_t      s_seq_table[MAX_TRACKED_NODES];
static SemaphoreHandle_t s_seq_mutex = NULL;

esp_err_t metrics_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        ESP_LOGE(TAG, "No se pudo crear el mutex de métricas");
        return ESP_ERR_NO_MEM;
    }

    s_seq_mutex = xSemaphoreCreateMutex();
    if (!s_seq_mutex) {
        ESP_LOGE(TAG, "No se pudo crear el mutex de PDR real");
        return ESP_ERR_NO_MEM;
    }
    memset(s_seq_table, 0, sizeof(s_seq_table));

    ESP_LOGI(TAG, "Módulo de métricas inicializado");
    return ESP_OK;
}

uint32_t metrics_get_current_power(void)
{
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

 
    m->layer        = (uint8_t)esp_mesh_get_layer();
    m->hops_to_root = m->layer;       

    #define MAX_CHILD_NODES 50  
    mesh_addr_t child_table[MAX_CHILD_NODES];
    int child_num = 0;
    esp_mesh_get_routing_table(child_table,
                               MAX_CHILD_NODES * sizeof(mesh_addr_t),
                               &child_num);
   
    m->connected_subs = (uint8_t)(child_num > 0 ? child_num : 0);

    mesh_addr_t parent_addr;
    wifi_ap_record_t ap_info;

    if (esp_mesh_get_parent_bssid(&parent_addr) == ESP_OK) {
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            m->rssi_parent = ap_info.rssi;
            m->snr_estimate = (int8_t)(ap_info.rssi - NOISE_FLOOR_DBM);
        }
    }

    if (esp_mesh_is_root()) {
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            m->rssi_router = ap_info.rssi;
        }
    }

    if (s_mutex) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        m->tx_count        = s_tx_count;
        m->rx_count        = s_rx_count;
        m->tx_fail         = s_tx_fail;
        m->latency_ms      = s_latency_ms;
        m->ping_lost_count = s_ping_lost_count;
        xSemaphoreGive(s_mutex);
    }

    peripherals_get_i2c_raw(m->i2c_raw, sizeof(m->i2c_raw));

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


static seq_track_t *find_or_create_entry(const uint8_t *mac)
{
    seq_track_t *free_slot = NULL;
    for (int i = 0; i < MAX_TRACKED_NODES; i++) {
        if (s_seq_table[i].in_use && memcmp(s_seq_table[i].mac, mac, 6) == 0) {
            return &s_seq_table[i];
        }
        if (!s_seq_table[i].in_use && free_slot == NULL) {
            free_slot = &s_seq_table[i];
        }
    }
    if (free_slot) {
        memcpy(free_slot->mac, mac, 6);
        free_slot->in_use = true;
        free_slot->first_seq = 0;
        free_slot->last_seq = 0;
        free_slot->received_count = 0;
    }
    return free_slot;  // NULL si la tabla está llena
}

void metrics_record_seq(const uint8_t *mac, uint32_t seq)
{
    if (!s_seq_mutex) return;
    xSemaphoreTake(s_seq_mutex, portMAX_DELAY);

    seq_track_t *e = find_or_create_entry(mac);
    if (e) {
        if (e->received_count == 0) {
            e->first_seq = seq;
            e->last_seq  = seq;
            e->received_count = 1;
        } else if (seq <= e->last_seq) {
         
            ESP_LOGD(TAG, "PDR: duplicado o reordenamiento, seq=%lu ignorado (last_seq=%lu)",
                     (unsigned long)seq, (unsigned long)e->last_seq);
        } else {
            
            e->last_seq = seq;
            e->received_count++;
        }
    } else {
        ESP_LOGW(TAG, "Tabla de PDR real llena (MAX_TRACKED_NODES=%d) — MAC descartada", MAX_TRACKED_NODES);
    }

    xSemaphoreGive(s_seq_mutex);
}

float metrics_get_pdr_pct(const uint8_t *mac)
{
    float pdr = 0.0f;
    if (!s_seq_mutex) return pdr;
    xSemaphoreTake(s_seq_mutex, portMAX_DELAY);

    for (int i = 0; i < MAX_TRACKED_NODES; i++) {
        if (s_seq_table[i].in_use && memcmp(s_seq_table[i].mac, mac, 6) == 0) {
            uint32_t expected = s_seq_table[i].last_seq - s_seq_table[i].first_seq + 1;
            if (expected > 0) {
                pdr = (float)s_seq_table[i].received_count / (float)expected * 100.0f;
            }
            break;
        }
    }

    xSemaphoreGive(s_seq_mutex);
    return pdr;
}