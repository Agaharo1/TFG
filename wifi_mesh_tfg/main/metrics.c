#include "metrics.h"
#include "esp_mesh.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include "driver/i2c.h"

static const char *TAG = "METRICS";

/* ─── Contadores internos (protegidos por mutex) ─────────────────────────── */
static SemaphoreHandle_t s_mutex;
static uint32_t s_tx_count   = 0;
static uint32_t s_rx_count   = 0;
static uint32_t s_tx_fail    = 0;
static uint32_t s_latency_ms = 0;   /* último RTT medido */
static uint32_t s_ping_lost_count = 0;
static uint32_t s_power_mw = 0;
static char s_i2c_hex[48] = "";

/* ─── Noise floor empírico para estimar SNR ──────────────────────────────── */
#define NOISE_FLOOR_DBM  (-95)


// 2. Configuración I2C
#define I2C_SLAVE_SCL_IO     22
#define I2C_SLAVE_SDA_IO     21
#define I2C_SLAVE_NUM        I2C_NUM_0
#define ESP_SLAVE_ADDR       0x3F 

static void i2c_read_task(void *arg) {
    uint8_t data[64];
    static char lcd_text[128] = {0}; 
    static int text_idx = 0;
    static bool high_nibble_ready = false;
    static uint8_t high_nibble = 0;
    static uint32_t ultima_lectura_ms = 0;

    while (true) {
        int size = i2c_slave_read_buffer(I2C_SLAVE_NUM, data, sizeof(data), pdMS_TO_TICKS(100));

        if (size > 0) {
            // 1. TRADUCTOR A TEXTO PLANO
            for (int i = 0; i < size; i++) {
                uint8_t val = data[i];
                bool En = (val & 0x04) != 0; 
                bool RS = (val & 0x01) != 0; 

                if (En) { 
                    if (RS) { 
                        uint8_t nibble = (val & 0xF0);
                        if (!high_nibble_ready) {
                            high_nibble = nibble;
                            high_nibble_ready = true;
                        } else {
                            char c = high_nibble | (nibble >> 4);
                            high_nibble_ready = false;

                            if (c >= 32 && c <= 126) { 
                                if (text_idx < sizeof(lcd_text) - 1) {
                                    lcd_text[text_idx++] = c;
                                    lcd_text[text_idx] = '\0';
                                }
                            }
                        }
                    } else { 
                        high_nibble_ready = false;
                        if (text_idx > 0 && lcd_text[text_idx-1] != ' ') {
                            if (text_idx < sizeof(lcd_text) - 1) {
                                lcd_text[text_idx++] = ' ';
                                lcd_text[text_idx] = '\0';
                            }
                        }
                    }
                }
            }

         
            extern char s_i2c_hex[48]; 
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            
            int start_idx = (text_idx > 47) ? text_idx - 47 : 0;
            strncpy(s_i2c_hex, &lcd_text[start_idx], 47);
            xSemaphoreGive(s_mutex);

      
            char *w_pos = strstr(lcd_text, " W "); 
            
            if (w_pos != NULL) {
                
                char *ptr = w_pos - 1;
                while (ptr >= lcd_text && (*ptr == ' ' || (*ptr >= '0' && *ptr <= '9') || *ptr == '.')) {
                    ptr--;
                }
                
                float vatios = atof(ptr + 1);
                
                if (strchr(ptr + 1, '.') != NULL && vatios < 25.0) {
                    
                    uint32_t milivatios = (uint32_t)(vatios * 1000.0);

                    xSemaphoreTake(s_mutex, portMAX_DELAY);
                    s_power_mw = milivatios;
                    xSemaphoreGive(s_mutex);
                    
                    ultima_lectura_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
                }

        
                memset(lcd_text, 0, sizeof(lcd_text));
                text_idx = 0;
            }

     
            if (text_idx > 80) {
                memset(lcd_text, 0, sizeof(lcd_text));
                text_idx = 0;
            }
        }


        uint32_t ahora = (uint32_t)(esp_timer_get_time() / 1000ULL);
        if ((ahora - ultima_lectura_ms) > 3000) {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_power_mw = 0; 
            xSemaphoreGive(s_mutex);
        }
    }
}

esp_err_t metrics_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        ESP_LOGE(TAG, "No se pudo crear el mutex de métricas");
        return ESP_ERR_NO_MEM;
    }

    i2c_config_t conf_slave = {
            .sda_io_num = I2C_SLAVE_SDA_IO,
            .sda_pullup_en = GPIO_PULLUP_ENABLE,
            .scl_io_num = I2C_SLAVE_SCL_IO,
            .scl_pullup_en = GPIO_PULLUP_ENABLE,
            .mode = I2C_MODE_SLAVE,
            .slave.addr_10bit_en = 0,
            .slave.slave_addr = ESP_SLAVE_ADDR,
        };
        i2c_param_config(I2C_SLAVE_NUM, &conf_slave);
        i2c_driver_install(I2C_SLAVE_NUM, conf_slave.mode, 256, 256, 0);

        xTaskCreate(i2c_read_task, "i2c_read", 2048, NULL, 5, NULL);

    ESP_LOGI(TAG, "Módulo de métricas inicializado");
    return ESP_OK;
}



uint32_t metrics_get_current_power(void)
{
    uint32_t current_power;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    current_power = s_power_mw; 
    xSemaphoreGive(s_mutex);
    return current_power;
}

void metrics_record_ping_loss(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_ping_lost_count++;
    xSemaphoreGive(s_mutex);
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
    m->ping_lost_count = s_ping_lost_count;
    strncpy(m->i2c_raw, s_i2c_hex, sizeof(m->i2c_raw) - 1);
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
