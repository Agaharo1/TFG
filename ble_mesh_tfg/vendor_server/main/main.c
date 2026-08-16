
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_bt.h"
#include "esp_timer.h"

#include "esp_ble_mesh_defs.h"
#include "esp_ble_mesh_common_api.h"
#include "esp_ble_mesh_networking_api.h"
#include "esp_ble_mesh_provisioning_api.h"
#include "esp_ble_mesh_config_model_api.h"
#include "esp_ble_mesh_local_data_operation_api.h"

#include "ble_mesh_example_init.h"

#define TAG "EXAMPLE_SERVER"

#define CID_ESP      0x02E5


#define TARGET_ADDR  0x0001


#define APP_KEY_IDX  0x0000

#define MSG_SEND_TTL 3
#define MSG_TIMEOUT  0
#define MSG_ROLE     ROLE_NODE


#define ESP_BLE_MESH_VND_MODEL_ID_CLIENT    0x0000   
#define ESP_BLE_MESH_VND_MODEL_ID_SERVER    0x0001   

#define ESP_BLE_MESH_VND_MODEL_OP_PING       ESP_BLE_MESH_MODEL_OP_3(0x01, CID_ESP)
#define ESP_BLE_MESH_VND_MODEL_OP_PONG       ESP_BLE_MESH_MODEL_OP_3(0x02, CID_ESP)

#define ESP_BLE_MESH_VND_MODEL_OP_DATA_CHUNK ESP_BLE_MESH_MODEL_OP_3(0x03, CID_ESP)
#define ESP_BLE_MESH_VND_MODEL_OP_DATA_ACK   ESP_BLE_MESH_MODEL_OP_3(0x04, CID_ESP)

typedef struct __attribute__((packed)) {
    uint16_t seq_num;
    uint16_t total_in_round;  
    int64_t  timestamp;
} ping_payload_t;


#define DATA_CHUNK_PAYLOAD_MAX  250

typedef struct __attribute__((packed)) {
    uint16_t seq_num;                       
    uint16_t total_chunks;                  
    uint16_t crc16;                         
    uint8_t  data[DATA_CHUNK_PAYLOAD_MAX];  
} data_chunk_payload_t;

#define DATA_CHUNK_HEADER_SIZE  (sizeof(uint16_t) * 3)  

typedef struct __attribute__((packed)) {
    uint16_t seq_num;    
    uint8_t  crc_ok;     
} data_ack_payload_t;


static uint16_t crc16_ccitt(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

#define TOTAL_PING_MESSAGES 50


#define DATA_TEST_SIZE_1KB    (1   * 1024)
#define DATA_TEST_SIZE_10KB   (10  * 1024)
#define DATA_TEST_SIZE_100KB  (100 * 1024)


#define CONTINUOUS_PING_COUNT        5     
#define CONTINUOUS_DATA_TEST_SIZE    DATA_TEST_SIZE_1KB  
#define CONTINUOUS_ROUND_INTERVAL_MS 15000  


#define ASSUMED_NOISE_FLOOR_DBM  (-95)

typedef struct {
    int32_t  sum;
    int16_t  min;
    int16_t  max;
    uint16_t count;
} rssi_stats_t;

static void rssi_stats_reset(rssi_stats_t *s)
{
    s->sum = 0;
    s->count = 0;
    s->min = INT16_MAX;
    s->max = INT16_MIN;
}

static void rssi_stats_add(rssi_stats_t *s, int8_t rssi)
{
    s->sum += rssi;
    s->count++;
    if (rssi < s->min) s->min = rssi;
    if (rssi > s->max) s->max = rssi;
}

static float rssi_stats_avg(const rssi_stats_t *s)
{
    return (s->count > 0) ? ((float)s->sum / s->count) : 0.0f;
}


static void rssi_stats_log(const char *tag_prefix, const rssi_stats_t *s)
{
    if (s->count == 0) {
        ESP_LOGW(TAG, "%s: sin muestras de RSSI", tag_prefix);
        return;
    }
    float avg = rssi_stats_avg(s);
    float snr_estimate = avg - ASSUMED_NOISE_FLOOR_DBM;
    ESP_LOGI(TAG, "%s: RSSI min/avg/max = %d / %.1f / %d dBm (n=%d) | SNR estimado (avg): %.1f dB",
             tag_prefix, s->min, avg, s->max, s->count, snr_estimate);
}

static uint16_t rx_count = 0;
static uint16_t lost_count = 0;
static int64_t  start_test_time = 0;
static TaskHandle_t ping_task_handle = NULL;


static uint16_t node_net_idx = 0;


static volatile uint16_t last_data_ack_seq = 0;
static volatile bool     last_data_ack_ok  = false;
static volatile int8_t   last_data_ack_rssi = 0;


static rssi_stats_t ping_rssi_stats;
static rssi_stats_t data_rssi_stats;

static uint8_t dev_uuid[ESP_BLE_MESH_OCTET16_LEN] = { 0x32, 0x10 };

static esp_ble_mesh_cfg_srv_t config_server = {
    .net_transmit = ESP_BLE_MESH_TRANSMIT(2, 20),
    .relay = ESP_BLE_MESH_RELAY_DISABLED,
    .relay_retransmit = ESP_BLE_MESH_TRANSMIT(2, 20),
    .beacon = ESP_BLE_MESH_BEACON_ENABLED,
#if defined(CONFIG_BLE_MESH_GATT_PROXY_SERVER)
    .gatt_proxy = ESP_BLE_MESH_GATT_PROXY_ENABLED,
#else
    
#endif
#if defined(CONFIG_BLE_MESH_FRIEND)
    
#else
    .friend_state = ESP_BLE_MESH_FRIEND_NOT_SUPPORTED,
#endif
    .default_ttl = 7,
};


static const esp_ble_mesh_client_op_pair_t vnd_op_pair[] = {
    { ESP_BLE_MESH_VND_MODEL_OP_PING,       ESP_BLE_MESH_VND_MODEL_OP_PONG },
    { ESP_BLE_MESH_VND_MODEL_OP_DATA_CHUNK, ESP_BLE_MESH_VND_MODEL_OP_DATA_ACK },
};

static esp_ble_mesh_client_t vendor_client = {
    .op_pair_size = ARRAY_SIZE(vnd_op_pair),
    .op_pair = vnd_op_pair,
};


static esp_ble_mesh_model_op_t vnd_op[] = {
    ESP_BLE_MESH_MODEL_OP(ESP_BLE_MESH_VND_MODEL_OP_PONG, sizeof(ping_payload_t)),
    ESP_BLE_MESH_MODEL_OP(ESP_BLE_MESH_VND_MODEL_OP_DATA_ACK, sizeof(data_ack_payload_t)),
    ESP_BLE_MESH_MODEL_OP_END,
};

static esp_ble_mesh_model_t root_models[] = {
    ESP_BLE_MESH_MODEL_CFG_SRV(&config_server),
};

static esp_ble_mesh_model_t vnd_models[] = {
    ESP_BLE_MESH_VENDOR_MODEL(CID_ESP, ESP_BLE_MESH_VND_MODEL_ID_CLIENT,
    vnd_op, NULL, &vendor_client),
};

static esp_ble_mesh_elem_t elements[] = {
    ESP_BLE_MESH_ELEMENT(0, root_models, vnd_models),
};

static esp_ble_mesh_comp_t composition = {
    .cid = CID_ESP,
    .element_count = ARRAY_SIZE(elements),
    .elements = elements,
};

static esp_ble_mesh_prov_t provision = {
    .uuid = dev_uuid,
};


static void run_data_transfer_test(uint32_t total_bytes, const char *label)
{
    uint16_t total_chunks = (uint16_t)((total_bytes + DATA_CHUNK_PAYLOAD_MAX - 1) / DATA_CHUNK_PAYLOAD_MAX);
    uint16_t chunk_lost = 0;
    uint32_t bytes_ok = 0;

    uint8_t filler[DATA_CHUNK_PAYLOAD_MAX];
    memset(filler, 0xAA, sizeof(filler));   

    esp_ble_mesh_msg_ctx_t ctx = {
        .net_idx = node_net_idx,
        .app_idx = APP_KEY_IDX,
        .addr = TARGET_ADDR,
        .send_ttl = MSG_SEND_TTL,
    };

    ESP_LOGI(TAG, "=== INICIANDO TRANSFERENCIA DE DATOS: %s (%" PRIu32 " bytes, %d chunks) ===",
             label, total_bytes, total_chunks);

    rssi_stats_reset(&data_rssi_stats);

    int64_t start = esp_timer_get_time();

    for (uint16_t i = 1; i <= total_chunks; i++) {
        uint32_t remaining = total_bytes - (uint32_t)(i - 1) * DATA_CHUNK_PAYLOAD_MAX;
        uint16_t chunk_len = (remaining > DATA_CHUNK_PAYLOAD_MAX) ? DATA_CHUNK_PAYLOAD_MAX : (uint16_t)remaining;

        data_chunk_payload_t payload;
        payload.seq_num = i;
        payload.total_chunks = total_chunks;
        memcpy(payload.data, filler, chunk_len);
        payload.crc16 = crc16_ccitt(payload.data, chunk_len);


        uint16_t msg_len = (uint16_t)(DATA_CHUNK_HEADER_SIZE + chunk_len);

        ulTaskNotifyTake(pdTRUE, 0);

        esp_ble_mesh_client_model_send_msg(
            vendor_client.model, &ctx, ESP_BLE_MESH_VND_MODEL_OP_DATA_CHUNK,
            msg_len, (uint8_t *)&payload, MSG_TIMEOUT, true, MSG_ROLE
        );


        TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(5000);
        bool acked = false;
        bool crc_ok = false;

        while (xTaskGetTickCount() < deadline) {
            TickType_t remaining = deadline - xTaskGetTickCount();
            uint32_t notified = ulTaskNotifyTake(pdTRUE, remaining);
            if (notified == 0) {
                break;   
            }
            if (last_data_ack_seq == i) {
                acked  = true;
                crc_ok = last_data_ack_ok;
      
                rssi_stats_add(&data_rssi_stats, last_data_ack_rssi);
                break;
            }

        }

        if (!acked) {
            chunk_lost++;
            ESP_LOGW(TAG, "TIMEOUT: Chunk #%d/%d de %s sin ACK valido (5s)", i, total_chunks, label);
        } else if (!crc_ok) {
            chunk_lost++;
            ESP_LOGE(TAG, "CRC INVALIDO: Chunk #%d/%d de %s llego corrupto segun el receptor", i, total_chunks, label);
        } else {
            bytes_ok += chunk_len;
        }


        vTaskDelay(20 / portTICK_PERIOD_MS);
    }

    int64_t elapsed_us = esp_timer_get_time() - start;
    float loss_pct = ((float)chunk_lost / total_chunks) * 100.0f;
    float throughput_bps = (elapsed_us > 0) ? (bytes_ok * 8.0f) / (elapsed_us / 1000000.0f) : 0.0f;

    ESP_LOGI(TAG, "=== RESULTADOS TRANSFERENCIA %s ===", label);
    ESP_LOGI(TAG, "Chunks: %d | Perdidos: %d | Loss: %.2f %%", total_chunks, chunk_lost, loss_pct);
    ESP_LOGI(TAG, "Bytes OK: %" PRIu32 " / %" PRIu32, bytes_ok, total_bytes);
    ESP_LOGI(TAG, "Tiempo: %.2f s | Throughput: %.2f bps (%.2f kbps)",
             elapsed_us / 1000000.0f, throughput_bps, throughput_bps / 1000.0f);
    rssi_stats_log("RSSI/SNR (DATA_ACK)", &data_rssi_stats);
}


static void run_ping_round(uint16_t round_size)
{
    ESP_LOGI(TAG, "=== RONDA DE PING (%d mensajes) ===", round_size);

    esp_ble_mesh_msg_ctx_t ctx = {
        .net_idx = node_net_idx,
        .app_idx = APP_KEY_IDX,
        .addr = TARGET_ADDR,
        .send_ttl = MSG_SEND_TTL,
    };

    start_test_time = esp_timer_get_time();
    rx_count = 0;
    lost_count = 0;
    rssi_stats_reset(&ping_rssi_stats);

    for (uint16_t tx_count = 1; tx_count <= round_size; tx_count++) {
        ping_payload_t payload = {
            .seq_num = tx_count,
            .total_in_round = round_size,
            .timestamp = esp_timer_get_time()
        };

        ulTaskNotifyTake(pdTRUE, 0);

        esp_ble_mesh_client_model_send_msg(
            vendor_client.model, &ctx, ESP_BLE_MESH_VND_MODEL_OP_PING,
            sizeof(ping_payload_t), (uint8_t *)&payload, MSG_TIMEOUT, true, MSG_ROLE
        );

        uint32_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));

        if (notified == 0) {
            lost_count++;
            ESP_LOGW(TAG, "TIMEOUT: Ping #%d sin respuesta (5s)", tx_count);
        }

        vTaskDelay(200 / portTICK_PERIOD_MS);
    }

    int64_t total_time = esp_timer_get_time() - start_test_time;
    float packet_loss = ((float)lost_count / round_size) * 100.0f;
    float throughput = (rx_count * sizeof(ping_payload_t) * 8.0f) / (total_time / 1000000.0f);

    ESP_LOGI(TAG, "=== RESULTADOS RONDA DE PING ===");
    ESP_LOGI(TAG, "Enviados: %d | Recibidos: %d | Perdidos (Timeout): %d", round_size, rx_count, lost_count);
    ESP_LOGI(TAG, "Packet Loss: %.2f %%", packet_loss);
    ESP_LOGI(TAG, "Throughput Aproximado: %.2f bps", throughput);
    rssi_stats_log("RSSI/SNR (PONG)", &ping_rssi_stats);
}


static void ping_test_task(void *pvParameters) {

    vTaskDelay(2000 / portTICK_PERIOD_MS);

    ESP_LOGI(TAG, "=== BENCHMARK INICIAL (una sola vez) ===");
    run_ping_round(TOTAL_PING_MESSAGES);
    run_data_transfer_test(DATA_TEST_SIZE_1KB,   "1KB");
    run_data_transfer_test(DATA_TEST_SIZE_10KB,  "10KB");
    run_data_transfer_test(DATA_TEST_SIZE_100KB, "100KB");
    ESP_LOGI(TAG, "=== BENCHMARK INICIAL FINALIZADO -- entrando en modo continuo ===");


    for (;;) {
        run_ping_round(CONTINUOUS_PING_COUNT);
        run_data_transfer_test(CONTINUOUS_DATA_TEST_SIZE, "1KB_continuo");
        vTaskDelay(pdMS_TO_TICKS(CONTINUOUS_ROUND_INTERVAL_MS));
    }
}

static void prov_complete(uint16_t net_idx, uint16_t addr, uint8_t flags, uint32_t iv_index)
{
    ESP_LOGI(TAG, "net_idx 0x%03x, addr 0x%04x", net_idx, addr);
    node_net_idx = net_idx;

}

static void example_ble_mesh_provisioning_cb(esp_ble_mesh_prov_cb_event_t event,
                                             esp_ble_mesh_prov_cb_param_t *param)
{
    switch (event) {
    case ESP_BLE_MESH_NODE_PROV_COMPLETE_EVT:
        ESP_LOGI(TAG, "ESP_BLE_MESH_NODE_PROV_COMPLETE_EVT");
        prov_complete(param->node_prov_complete.net_idx, param->node_prov_complete.addr,
            param->node_prov_complete.flags, param->node_prov_complete.iv_index);
        break;
    default:
        break;
    }
}

static void example_ble_mesh_config_server_cb(esp_ble_mesh_cfg_server_cb_event_t event,
                                              esp_ble_mesh_cfg_server_cb_param_t *param)
{
    if (event == ESP_BLE_MESH_CFG_SERVER_STATE_CHANGE_EVT) {
        switch (param->ctx.recv_op) {
        case ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD:
            ESP_LOGI(TAG, "ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD");
            break;
        case ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND:
            ESP_LOGI(TAG, "ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND - listo para iniciar pings");

            if (ping_task_handle == NULL) {
                xTaskCreate(ping_test_task, "ping_test_task", 4096, NULL, 5, &ping_task_handle);
            }
            break;
        default:
            break;
        }
    }
}

static void example_ble_mesh_custom_model_cb(esp_ble_mesh_model_cb_event_t event,
                                             esp_ble_mesh_model_cb_param_t *param)
{
    switch (event) {
    case ESP_BLE_MESH_MODEL_OPERATION_EVT:
        if (param->model_operation.opcode == ESP_BLE_MESH_VND_MODEL_OP_PONG) {
            int64_t time_now = esp_timer_get_time();
            ping_payload_t *recv_payload = (ping_payload_t *)param->model_operation.msg;
            int8_t rssi = param->model_operation.ctx->recv_rssi;
            rx_count++;
            rssi_stats_add(&ping_rssi_stats, rssi);
            float rtt_ms = (float)(time_now - recv_payload->timestamp) / 1000.0;
            ESP_LOGI(TAG, "PONG #%d recibido. RTT: %.2f ms | RSSI: %d dBm", recv_payload->seq_num, rtt_ms, rssi);

            if (ping_task_handle != NULL) {
                xTaskNotifyGive(ping_task_handle);
            }
        } else if (param->model_operation.opcode == ESP_BLE_MESH_VND_MODEL_OP_DATA_ACK) {

            data_ack_payload_t *ack = (data_ack_payload_t *)param->model_operation.msg;
            last_data_ack_seq  = ack->seq_num;
            last_data_ack_ok   = (ack->crc_ok == 1);
            last_data_ack_rssi = param->model_operation.ctx->recv_rssi;

            if (ping_task_handle != NULL) {
                xTaskNotifyGive(ping_task_handle);
            }
        }
        break;
    default:
        break;
    }
}

static esp_err_t ble_mesh_init(void)
{
    esp_err_t err;

    esp_ble_mesh_register_prov_callback(example_ble_mesh_provisioning_cb);
    esp_ble_mesh_register_config_server_callback(example_ble_mesh_config_server_cb);
    esp_ble_mesh_register_custom_model_callback(example_ble_mesh_custom_model_cb);

    err = esp_ble_mesh_init(&provision, &composition);
    if (err != ESP_OK) return err;


    err = esp_ble_mesh_client_model_init(&vnd_models[0]);
    if (err) return err;

    err = esp_ble_mesh_node_prov_enable((esp_ble_mesh_prov_bearer_t)(ESP_BLE_MESH_PROV_ADV | ESP_BLE_MESH_PROV_GATT));
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "BLE Mesh Node initialized (modo PINGER hacia 0x%04x)", TARGET_ADDR);

    return ESP_OK;
}

void app_main(void)
{
    esp_err_t err;

    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    err = bluetooth_init();
    if (err) return;

    ble_mesh_get_dev_uuid(dev_uuid);
    ble_mesh_init();
}