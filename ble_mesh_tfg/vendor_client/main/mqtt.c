
#include "mqtt.h"
#include <mqtt_client.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

static const char *TAG = "MQTT";

static esp_mqtt_client_handle_t s_client = NULL;
static EventGroupHandle_t       s_evt_group;
#define MQTT_CONNECTED_BIT  BIT0

#define MQTT_TOPIC_STATUS  "iot/blemesh/gateway/status"

static void mqtt_event_handler(void *arg, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    (void)arg;
    (void)base;
    esp_mqtt_event_handle_t ev = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Conectado al broker MQTT");
        xEventGroupSetBits(s_evt_group, MQTT_CONNECTED_BIT);

        esp_mqtt_client_publish(s_client, MQTT_TOPIC_STATUS,
                                "{\"gateway\":\"online\"}", 0, 1, 1);
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Desconectado del broker MQTT -- reintentando...");
        xEventGroupClearBits(s_evt_group, MQTT_CONNECTED_BIT);
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "Error MQTT (tipo=%d)", ev->error_handle->error_type);
        break;

    default:
        break;
    }
}

esp_err_t mqtt_handler_start(void)
{
    s_evt_group = xEventGroupCreate();

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri   = CONFIG_MQTT_BROKER_URI,
        .credentials.username = CONFIG_MQTT_USERNAME,
        .credentials.authentication.password = CONFIG_MQTT_PASSWORD,
        .session.keepalive     = 30,
        .session.last_will = {
            .topic  = MQTT_TOPIC_STATUS,
            .msg    = "{\"gateway\":\"offline\"}",
            .qos    = 1,
            .retain = 1,
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
        ESP_LOGW(TAG, "Timeout esperando conexion MQTT -- se reintentara en background");
    }
    return ESP_OK;
}

bool mqtt_is_connected(void)
{
    if (!s_evt_group) return false;
    return (xEventGroupGetBits(s_evt_group) & MQTT_CONNECTED_BIT) != 0;
}

void mqtt_publish_ping_result(uint16_t node_addr,
                               uint16_t received, uint16_t expected,
                               float loss_pct,
                               int16_t rssi_min, float rssi_avg, int16_t rssi_max,
                               float snr_estimate_avg)
{
    if (!mqtt_is_connected() || s_client == NULL) {
        ESP_LOGW(TAG, "MQTT desconectado -- descartando resultado de ping de 0x%04x", node_addr);
        return;
    }

    char topic[96];
    snprintf(topic, sizeof(topic), "iot/blemesh/node/%04x/metrics/ping", node_addr);

    char json_payload[320];
    snprintf(json_payload, sizeof(json_payload),
             "{\"node_addr\":\"0x%04x\","
             "\"received\":%u,"
             "\"expected\":%u,"
             "\"loss_pct\":%.2f,"
             "\"rssi_min_dbm\":%d,"
             "\"rssi_avg_dbm\":%.1f,"
             "\"rssi_max_dbm\":%d,"
             "\"snr_estimate_avg_db\":%.1f}",
             node_addr, received, expected, loss_pct,
             rssi_min, rssi_avg, rssi_max, snr_estimate_avg);

    int msg_id = esp_mqtt_client_publish(s_client, topic, json_payload, 0, 1, 0);
    if (msg_id != -1) {
        ESP_LOGI(TAG, "Resultado de ping publicado en %s (msg_id=%d)", topic, msg_id);
    } else {
        ESP_LOGE(TAG, "Fallo al publicar resultado de ping por MQTT");
    }
}

void mqtt_publish_transfer_result(uint16_t node_addr,
                                   uint32_t total_bytes_observed,
                                   uint16_t total_chunks, uint16_t received_chunks,
                                   uint16_t crc_fail_count, float loss_pct,
                                   int16_t rssi_min, float rssi_avg, int16_t rssi_max,
                                   float snr_estimate_avg)
{
    if (!mqtt_is_connected() || s_client == NULL) {
        ESP_LOGW(TAG, "MQTT desconectado -- descartando resultado de transferencia (%" PRIu32 " bytes) de 0x%04x",
                 total_bytes_observed, node_addr);
        return;
    }

    char topic[112];
    snprintf(topic, sizeof(topic), "iot/blemesh/node/%04x/metrics/transfer_%" PRIu32 "b",
             node_addr, total_bytes_observed);

    char json_payload[384];
    snprintf(json_payload, sizeof(json_payload),
             "{\"node_addr\":\"0x%04x\","
             "\"total_bytes_observed\":%" PRIu32 ","
             "\"total_chunks\":%u,"
             "\"received_chunks\":%u,"
             "\"crc_fail_count\":%u,"
             "\"loss_pct\":%.2f,"
             "\"rssi_min_dbm\":%d,"
             "\"rssi_avg_dbm\":%.1f,"
             "\"rssi_max_dbm\":%d,"
             "\"snr_estimate_avg_db\":%.1f}",
             node_addr, total_bytes_observed, total_chunks, received_chunks, crc_fail_count,
             loss_pct, rssi_min, rssi_avg, rssi_max, snr_estimate_avg);

    int msg_id = esp_mqtt_client_publish(s_client, topic, json_payload, 0, 1, 0);
    if (msg_id != -1) {
        ESP_LOGI(TAG, "Resultado de transferencia publicado en %s (msg_id=%d)", topic, msg_id);
    } else {
        ESP_LOGE(TAG, "Fallo al publicar resultado de transferencia por MQTT");
    }
}