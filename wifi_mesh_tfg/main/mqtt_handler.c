#include "mqtt_handler.h"
#include <mqtt_client.h>
#include "esp_log.h"
#include "esp_mesh.h"
#include "esp_wifi.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "MQTT";





static esp_mqtt_client_handle_t s_client = NULL;
static EventGroupHandle_t       s_evt_group;
#define MQTT_CONNECTED_BIT  BIT0


static void mqtt_event_handler(void *arg, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t ev = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Conectado al broker MQTT");
        xEventGroupSetBits(s_evt_group, MQTT_CONNECTED_BIT);

   
        esp_mqtt_client_publish(s_client, MQTT_TOPIC_STATUS,
                                "{\"root\":\"online\"}", 0, 1, 1);
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Desconectado del broker MQTT — reintentando...");
        xEventGroupClearBits(s_evt_group, MQTT_CONNECTED_BIT);
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "Error MQTT (tipo=%d)",
                 ev->error_handle->error_type);
        break;

    default:
        break;
    }
}



esp_err_t mqtt_handler_start(void)
{
    s_evt_group = xEventGroupCreate();

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri       = CONFIG_MQTT_BROKER_URI,
        .credentials.username     = CONFIG_MQTT_USERNAME,
        .credentials.authentication.password = CONFIG_MQTT_PASSWORD,
        .session.keepalive        = 30,
        .session.last_will = {
            .topic   = MQTT_TOPIC_STATUS,
            .msg     = "{\"root\":\"offline\"}",
            .qos     = 1,
            .retain  = 1,
        },
        .network.reconnect_timeout_ms = 5000,
    };

    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) {
        ESP_LOGE(TAG, "Error al inicializar cliente MQTT");
        return ESP_FAIL;
    }

    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID,
                                   mqtt_event_handler, NULL);

    esp_err_t ret = esp_mqtt_client_start(s_client);
    if (ret != ESP_OK) return ret;

 
    EventBits_t bits = xEventGroupWaitBits(s_evt_group, MQTT_CONNECTED_BIT,
                                           pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(15000));
    if (!(bits & MQTT_CONNECTED_BIT)) {
        ESP_LOGW(TAG, "Timeout esperando conexión MQTT — se reintentará en background");
    }
    return ESP_OK;
}

bool mqtt_is_connected(void)
{
    if (!s_evt_group) return false;
    return (xEventGroupGetBits(s_evt_group) & MQTT_CONNECTED_BIT) != 0;
}



void mqtt_publish_metrics(const uint8_t *mac, const metrics_payload_t *m,
                          uint32_t seq)
{

    ESP_LOGI(TAG, "Publicando métricas de nodo %02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    if (!mqtt_is_connected()) {
        ESP_LOGD(TAG, "MQTT desconectado — descartando métricas");
        return;
    }

    char topic[64];
    snprintf(topic, sizeof(topic), MQTT_TOPIC_METRICS_FMT,
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    /* Construir JSON */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "seq",            (double)seq);
    cJSON_AddNumberToObject(root, "rssi_parent",    m->rssi_parent);
    cJSON_AddNumberToObject(root, "rssi_router",    m->rssi_router);
    cJSON_AddNumberToObject(root, "snr_estimate",   m->snr_estimate);
    cJSON_AddNumberToObject(root, "layer",          m->layer);
    cJSON_AddNumberToObject(root, "hops_to_root",   m->hops_to_root);
    cJSON_AddNumberToObject(root, "connected_subs", m->connected_subs);
    cJSON_AddNumberToObject(root, "tx_count",       (double)m->tx_count);
    cJSON_AddNumberToObject(root, "rx_count",       (double)m->rx_count);
    cJSON_AddNumberToObject(root, "tx_fail",        (double)m->tx_fail);


    uint32_t total = m->tx_count + m->rx_count;
    float pdr = total > 0 ? (float)m->rx_count / total * 100.0f : 0.0f;
    cJSON_AddNumberToObject(root, "pdr_pct",        (double)pdr);

    cJSON_AddNumberToObject(root, "latency_ms",     (double)m->latency_ms);
    cJSON_AddNumberToObject(root, "free_heap",      (double)m->free_heap);
    cJSON_AddNumberToObject(root, "uptime_s",       (double)m->uptime_s);
    cJSON_AddNumberToObject(root, "ping_lost_count", (double)m->ping_lost_count);
    cJSON_AddNumberToObject(root, "power_json", (double)m->power_json_prev_mw);
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    cJSON_AddStringToObject(root, "mac", mac_str);

    char *payload = cJSON_PrintUnformatted(root);
    if (payload) {
        int msg_id = esp_mqtt_client_publish(s_client, topic, payload,
                                             0, 0, 0);
        ESP_LOGI(TAG, "Publicadas métricas de %s (msg_id=%d)", mac_str, msg_id);
        free(payload);
    }
    cJSON_Delete(root);
    
}


void mqtt_publish_exp_result(const uint8_t *src_mac, const exp_packet_t *exp)
{
    if (!mqtt_is_connected() || s_client == NULL) {
        ESP_LOGW(TAG, "MQTT desconectado — descartando experimento");
        return;
    }

    char topic[128];
    snprintf(topic, sizeof(topic), "iot/mesh/node/%02x%02x%02x%02x%02x%02x/metrics/%lukb",
             src_mac[0], src_mac[1], src_mac[2], src_mac[3], src_mac[4], src_mac[5],
             (unsigned long)exp->kb);

    char json_payload[256];
    snprintf(json_payload, sizeof(json_payload),
             "{\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\","
             "\"payload_kb\":%lu,"
             "\"duration_ms\":%lu,"
             "\"power_idle_mw\":%lu,"
             "\"power_active_mw\":%lu}",
             src_mac[0], src_mac[1], src_mac[2], src_mac[3], src_mac[4], src_mac[5],
             (unsigned long)exp->kb,
             (unsigned long)exp->time_ms,
             (unsigned long)exp->p_idle,
             (unsigned long)exp->p_active);

    int msg_id = esp_mqtt_client_publish(s_client, topic, json_payload, 0, 0, 0);
    
    if (msg_id != -1) {
        ESP_LOGI(TAG, "Experimento enviado al tópico: %s (msg_id=%d)", topic, msg_id);
    } else {
        ESP_LOGE(TAG, "Fallo al enviar por MQTT");
    }
}




