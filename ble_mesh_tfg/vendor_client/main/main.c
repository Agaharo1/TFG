

#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_bt.h"

#include "esp_ble_mesh_defs.h"
#include "esp_ble_mesh_common_api.h"
#include "esp_ble_mesh_provisioning_api.h"
#include "esp_ble_mesh_networking_api.h"
#include "esp_ble_mesh_config_model_api.h"

#include "ble_mesh_example_init.h"
#include "ble_mesh_example_nvs.h"
#include "wifi.h"
#include "mqtt.h"

#define TAG "EXAMPLE_CLIENT"

#define CID_ESP             0x02E5
#define PROV_OWN_ADDR       0x0001   
#define PROV_START_ADDR     0x0005
#define MSG_SEND_TTL        3
#define MSG_TIMEOUT         0
#define MSG_ROLE            ROLE_PROVISIONER
#define COMP_DATA_PAGE_0    0x00
#define APP_KEY_IDX         0x0000
#define APP_KEY_OCTET       0x12

#define ESP_BLE_MESH_VND_MODEL_ID_CLIENT    0x0000
#define ESP_BLE_MESH_VND_MODEL_ID_SERVER    0x0001

#define ESP_BLE_MESH_VND_MODEL_OP_PING       ESP_BLE_MESH_MODEL_OP_3(0x01, CID_ESP)
#define ESP_BLE_MESH_VND_MODEL_OP_PONG       ESP_BLE_MESH_MODEL_OP_3(0x02, CID_ESP)
/* NUEVOS opcodes para la fase de transferencia de datos (1KB/10KB/100KB) */
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

static void rssi_stats_log(const char *tag_prefix, const rssi_stats_t *s)
{
    if (s->count == 0) {
        ESP_LOGW(TAG, "%s: sin muestras de RSSI", tag_prefix);
        return;
    }
    float avg = (float)s->sum / s->count;
    float snr_estimate = avg - ASSUMED_NOISE_FLOOR_DBM;
    ESP_LOGI(TAG, "%s: RSSI min/avg/max = %d / %.1f / %d dBm (n=%d) | SNR estimado (avg): %.1f dB",
             tag_prefix, s->min, avg, s->max, s->count, snr_estimate);
}

static rssi_stats_t ping_rssi_stats;
static rssi_stats_t data_rssi_stats;


static uint16_t ping_received_count   = 0;
static uint16_t data_chunks_received  = 0;
static uint16_t data_crc_fail_count   = 0;
static uint32_t data_bytes_received   = 0;

static uint8_t dev_uuid[ESP_BLE_MESH_OCTET16_LEN];


static struct example_info_store {
    uint16_t last_node_addr;
    uint16_t vnd_tid;
} store = {
    .last_node_addr = ESP_BLE_MESH_ADDR_UNASSIGNED,
    .vnd_tid = 0,
};

static nvs_handle_t NVS_HANDLE;
static const char * NVS_KEY = "vendor_client";

static struct esp_ble_mesh_key {
    uint16_t net_idx;
    uint16_t app_idx;
    uint8_t  app_key[ESP_BLE_MESH_OCTET16_LEN];
} prov_key;

static esp_ble_mesh_cfg_srv_t config_server = {
    .net_transmit = ESP_BLE_MESH_TRANSMIT(2, 20),
    .relay = ESP_BLE_MESH_RELAY_DISABLED,
    .relay_retransmit = ESP_BLE_MESH_TRANSMIT(2, 20),
    .beacon = ESP_BLE_MESH_BEACON_DISABLED,
#if defined(CONFIG_BLE_MESH_FRIEND)
    .friend_state = ESP_BLE_MESH_FRIEND_ENABLED,
#else
    .friend_state = ESP_BLE_MESH_FRIEND_NOT_SUPPORTED,
#endif
    .default_ttl = 7,
};

static esp_ble_mesh_client_t config_client;


static esp_ble_mesh_model_op_t vnd_op[] = {
    ESP_BLE_MESH_MODEL_OP(ESP_BLE_MESH_VND_MODEL_OP_PING, sizeof(ping_payload_t)),
    ESP_BLE_MESH_MODEL_OP(ESP_BLE_MESH_VND_MODEL_OP_DATA_CHUNK, DATA_CHUNK_HEADER_SIZE),
    ESP_BLE_MESH_MODEL_OP_END,
};

static esp_ble_mesh_model_t root_models[] = {
    ESP_BLE_MESH_MODEL_CFG_SRV(&config_server),
    ESP_BLE_MESH_MODEL_CFG_CLI(&config_client),
};

static esp_ble_mesh_model_t vnd_models[] = {
    ESP_BLE_MESH_VENDOR_MODEL(CID_ESP, ESP_BLE_MESH_VND_MODEL_ID_SERVER,
    vnd_op, NULL, NULL),
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
    .prov_uuid          = dev_uuid,
    .prov_unicast_addr  = PROV_OWN_ADDR,
    .prov_start_address = PROV_START_ADDR,
};

static void mesh_example_info_store(void)
{
    ble_mesh_nvs_store(NVS_HANDLE, NVS_KEY, &store, sizeof(store));
}

static void mesh_example_info_restore(void)
{
    esp_err_t err = ESP_OK;
    bool exist = false;
    err = ble_mesh_nvs_restore(NVS_HANDLE, NVS_KEY, &store, sizeof(store), &exist);
    if (err != ESP_OK) return;
    if (exist) {
        ESP_LOGI(TAG, "Restore, last_node_addr 0x%04x, vnd_tid 0x%04x", store.last_node_addr, store.vnd_tid);
    }
}

static void example_ble_mesh_set_msg_common(esp_ble_mesh_client_common_param_t *common,
                                            esp_ble_mesh_node_t *node,
                                            esp_ble_mesh_model_t *model, uint32_t opcode)
{
    common->opcode = opcode;
    common->model = model;
    common->ctx.net_idx = prov_key.net_idx;
    common->ctx.app_idx = prov_key.app_idx;
    common->ctx.addr = node->unicast_addr;
    common->ctx.send_ttl = MSG_SEND_TTL;
    common->msg_timeout = MSG_TIMEOUT;
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 2, 0)
    common->msg_role = MSG_ROLE;
#endif
}

static esp_err_t prov_complete(uint16_t node_index, const esp_ble_mesh_octet16_t uuid,
                               uint16_t primary_addr, uint8_t element_num, uint16_t net_idx)
{
    esp_ble_mesh_client_common_param_t common = {0};
    esp_ble_mesh_cfg_client_get_state_t get = {0};
    esp_ble_mesh_node_t *node = NULL;
    char name[10] = {'\0'};
    esp_err_t err;

    store.last_node_addr = primary_addr;
    mesh_example_info_store();

    sprintf(name, "%s%02x", "NODE-", node_index);
    esp_ble_mesh_provisioner_set_node_name(node_index, name);

    node = esp_ble_mesh_provisioner_get_node_with_addr(primary_addr);
    if (node == NULL) return ESP_FAIL;

    example_ble_mesh_set_msg_common(&common, node, config_client.model, ESP_BLE_MESH_MODEL_OP_COMPOSITION_DATA_GET);
    get.comp_data_get.page = COMP_DATA_PAGE_0;
    err = esp_ble_mesh_config_client_get_state(&common, &get);
    if (err != ESP_OK) return ESP_FAIL;

    return ESP_OK;
}

static void recv_unprov_adv_pkt(uint8_t dev_uuid[ESP_BLE_MESH_OCTET16_LEN], uint8_t addr[BD_ADDR_LEN],
                                esp_ble_mesh_addr_type_t addr_type, uint16_t oob_info,
                                uint8_t adv_type, esp_ble_mesh_prov_bearer_t bearer)
{
    esp_ble_mesh_unprov_dev_add_t add_dev = {0};
    memcpy(add_dev.addr, addr, BD_ADDR_LEN);
    add_dev.addr_type = (esp_ble_mesh_addr_type_t)addr_type;
    memcpy(add_dev.uuid, dev_uuid, ESP_BLE_MESH_OCTET16_LEN);
    add_dev.oob_info = oob_info;
    add_dev.bearer = (esp_ble_mesh_prov_bearer_t)bearer;
    esp_ble_mesh_provisioner_add_unprov_dev(&add_dev,
            ADD_DEV_RM_AFTER_PROV_FLAG | ADD_DEV_START_PROV_NOW_FLAG | ADD_DEV_FLUSHABLE_DEV_FLAG);
}

static void example_ble_mesh_provisioning_cb(esp_ble_mesh_prov_cb_event_t event,
                                             esp_ble_mesh_prov_cb_param_t *param)
{
    switch (event) {
    case ESP_BLE_MESH_PROV_REGISTER_COMP_EVT:
        mesh_example_info_restore();
        break;
    case ESP_BLE_MESH_PROVISIONER_RECV_UNPROV_ADV_PKT_EVT:
        recv_unprov_adv_pkt(param->provisioner_recv_unprov_adv_pkt.dev_uuid, param->provisioner_recv_unprov_adv_pkt.addr,
                            param->provisioner_recv_unprov_adv_pkt.addr_type, param->provisioner_recv_unprov_adv_pkt.oob_info,
                            param->provisioner_recv_unprov_adv_pkt.adv_type, param->provisioner_recv_unprov_adv_pkt.bearer);
        break;
    case ESP_BLE_MESH_PROVISIONER_PROV_COMPLETE_EVT:
        prov_complete(param->provisioner_prov_complete.node_idx, param->provisioner_prov_complete.device_uuid,
                      param->provisioner_prov_complete.unicast_addr, param->provisioner_prov_complete.element_num,
                      param->provisioner_prov_complete.netkey_idx);
        break;
    case ESP_BLE_MESH_PROVISIONER_ADD_LOCAL_APP_KEY_COMP_EVT:
        if (param->provisioner_add_app_key_comp.err_code == 0) {
            prov_key.app_idx = param->provisioner_add_app_key_comp.app_idx;
            esp_ble_mesh_provisioner_bind_app_key_to_local_model(PROV_OWN_ADDR, prov_key.app_idx,
                    ESP_BLE_MESH_VND_MODEL_ID_SERVER, CID_ESP);
        }
        break;
    default:
        break;
    }
}

static void example_ble_mesh_config_client_cb(esp_ble_mesh_cfg_client_cb_event_t event,
                                              esp_ble_mesh_cfg_client_cb_param_t *param)
{
    esp_ble_mesh_client_common_param_t common = {0};
    esp_ble_mesh_cfg_client_set_state_t set = {0};
    esp_ble_mesh_node_t *node = NULL;

    if (param->error_code) return;

    node = esp_ble_mesh_provisioner_get_node_with_addr(param->params->ctx.addr);
    if (!node) return;

    switch (event) {
    case ESP_BLE_MESH_CFG_CLIENT_GET_STATE_EVT:
        if (param->params->opcode == ESP_BLE_MESH_MODEL_OP_COMPOSITION_DATA_GET) {
            esp_ble_mesh_provisioner_store_node_comp_data(param->params->ctx.addr,
                param->status_cb.comp_data_status.composition_data->data,
                param->status_cb.comp_data_status.composition_data->len);

            example_ble_mesh_set_msg_common(&common, node, config_client.model, ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD);
            set.app_key_add.net_idx = prov_key.net_idx;
            set.app_key_add.app_idx = prov_key.app_idx;
            memcpy(set.app_key_add.app_key, prov_key.app_key, ESP_BLE_MESH_OCTET16_LEN);
            esp_ble_mesh_config_client_set_state(&common, &set);
        }
        break;
    case ESP_BLE_MESH_CFG_CLIENT_SET_STATE_EVT:
        if (param->params->opcode == ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD) {
            example_ble_mesh_set_msg_common(&common, node, config_client.model, ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND);
            set.model_app_bind.element_addr = node->unicast_addr;
            set.model_app_bind.model_app_idx = prov_key.app_idx;
            set.model_app_bind.model_id = ESP_BLE_MESH_VND_MODEL_ID_CLIENT;
            set.model_app_bind.company_id = CID_ESP;
            esp_ble_mesh_config_client_set_state(&common, &set);
        } else if (param->params->opcode == ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND) {
            ESP_LOGW(TAG, "Nodo 0x%04x configurado y listo para iniciar sus pings", node->unicast_addr);
  
        }
        break;
    default:
        break;
    }
}


static void example_ble_mesh_custom_model_cb(esp_ble_mesh_model_cb_event_t event,
                                             esp_ble_mesh_model_cb_param_t *param)
{
    switch (event) {
    case ESP_BLE_MESH_MODEL_OPERATION_EVT:
        if (param->model_operation.opcode == ESP_BLE_MESH_VND_MODEL_OP_PING) {
            ping_payload_t *payload = (ping_payload_t *)param->model_operation.msg;
            int8_t rssi = param->model_operation.ctx->recv_rssi;

            if (payload->seq_num == 1) {
                rssi_stats_reset(&ping_rssi_stats);
                ping_received_count = 0;
            }
            rssi_stats_add(&ping_rssi_stats, rssi);
            ping_received_count++;

            ESP_LOGI(TAG, "PING #%d recibido de 0x%04x (RSSI: %d dBm). Devolviendo PONG...",
                     payload->seq_num, param->model_operation.ctx->addr, rssi);

            esp_err_t err = esp_ble_mesh_server_model_send_msg(&vnd_models[0],
                    param->model_operation.ctx, ESP_BLE_MESH_VND_MODEL_OP_PONG,
                    sizeof(ping_payload_t), (uint8_t *)payload);
            if (err) {
                ESP_LOGE(TAG, "Error devolviendo PONG a 0x%04x", param->model_operation.ctx->addr);
            }

            if (payload->seq_num == payload->total_in_round) {
                rssi_stats_log("RSSI/SNR (PING, ronda completa)", &ping_rssi_stats);

                float avg = (ping_rssi_stats.count > 0) ? (float)ping_rssi_stats.sum / ping_rssi_stats.count : 0.0f;
                float loss_pct = ((float)(payload->total_in_round - ping_received_count) / payload->total_in_round) * 100.0f;

                mqtt_publish_ping_result(param->model_operation.ctx->addr,
                        ping_received_count, payload->total_in_round, loss_pct,
                        ping_rssi_stats.min, avg, ping_rssi_stats.max,
                        avg - ASSUMED_NOISE_FLOOR_DBM);
            }
        } else if (param->model_operation.opcode == ESP_BLE_MESH_VND_MODEL_OP_DATA_CHUNK) {

            data_chunk_payload_t *chunk = (data_chunk_payload_t *)param->model_operation.msg;
            uint16_t received_len = param->model_operation.length;
            uint16_t data_len = (received_len > DATA_CHUNK_HEADER_SIZE)
                                 ? (uint16_t)(received_len - DATA_CHUNK_HEADER_SIZE) : 0;
            int8_t rssi = param->model_operation.ctx->recv_rssi;

            if (chunk->seq_num == 1) {
                rssi_stats_reset(&data_rssi_stats);
                data_chunks_received = 0;
                data_crc_fail_count  = 0;
                data_bytes_received  = 0;
            }
            rssi_stats_add(&data_rssi_stats, rssi);
            data_chunks_received++;
            data_bytes_received += data_len;

            uint16_t computed_crc = crc16_ccitt(chunk->data, data_len);
            uint8_t  crc_ok = (computed_crc == chunk->crc16) ? 1 : 0;

            if (!crc_ok) {
                data_crc_fail_count++;
                ESP_LOGE(TAG, "CRC MISMATCH: Chunk #%d/%d de 0x%04x (esperado 0x%04x, calculado 0x%04x)",
                         chunk->seq_num, chunk->total_chunks, param->model_operation.ctx->addr,
                         chunk->crc16, computed_crc);
            } else if (chunk->seq_num == 1 || chunk->seq_num == chunk->total_chunks || (chunk->seq_num % 50 == 0)) {
                ESP_LOGI(TAG, "Chunk #%d/%d recibido de 0x%04x (CRC OK, RSSI: %d dBm)",
                         chunk->seq_num, chunk->total_chunks, param->model_operation.ctx->addr, rssi);
            }

            data_ack_payload_t ack = { .seq_num = chunk->seq_num, .crc_ok = crc_ok };
            esp_err_t err = esp_ble_mesh_server_model_send_msg(&vnd_models[0],
                    param->model_operation.ctx, ESP_BLE_MESH_VND_MODEL_OP_DATA_ACK,
                    sizeof(ack), (uint8_t *)&ack);
            if (err) {
                ESP_LOGE(TAG, "Error devolviendo DATA_ACK #%d a 0x%04x",
                         chunk->seq_num, param->model_operation.ctx->addr);
            }

            if (chunk->seq_num == chunk->total_chunks) {
                rssi_stats_log("RSSI/SNR (DATA_CHUNK, transferencia completa)", &data_rssi_stats);

                float avg = (data_rssi_stats.count > 0) ? (float)data_rssi_stats.sum / data_rssi_stats.count : 0.0f;
                float loss_pct = ((float)(chunk->total_chunks - data_chunks_received) / chunk->total_chunks) * 100.0f;

                mqtt_publish_transfer_result(param->model_operation.ctx->addr,
                        data_bytes_received, chunk->total_chunks, data_chunks_received,
                        data_crc_fail_count, loss_pct,
                        data_rssi_stats.min, avg, data_rssi_stats.max,
                        avg - ASSUMED_NOISE_FLOOR_DBM);
            }
        }
        break;
    default:
        break;
    }
}

static esp_err_t ble_mesh_init(void)
{
    uint8_t match[2] = { 0x32, 0x10 };
    esp_err_t err;

    prov_key.net_idx = ESP_BLE_MESH_KEY_PRIMARY;
    prov_key.app_idx = APP_KEY_IDX;
    memset(prov_key.app_key, APP_KEY_OCTET, sizeof(prov_key.app_key));

    esp_ble_mesh_register_prov_callback(example_ble_mesh_provisioning_cb);
    esp_ble_mesh_register_config_client_callback(example_ble_mesh_config_client_cb);
    esp_ble_mesh_register_custom_model_callback(example_ble_mesh_custom_model_cb);

    err = esp_ble_mesh_init(&provision, &composition);
    if (err != ESP_OK) return err;


    err = esp_ble_mesh_provisioner_set_dev_uuid_match(match, sizeof(match), 0x0, false);
    if (err != ESP_OK) return err;

    err = esp_ble_mesh_provisioner_prov_enable((esp_ble_mesh_prov_bearer_t)(ESP_BLE_MESH_PROV_ADV | ESP_BLE_MESH_PROV_GATT));
    if (err != ESP_OK) return err;

    err = esp_ble_mesh_provisioner_add_local_app_key(prov_key.app_key, prov_key.net_idx, prov_key.app_idx);
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "Provisioner central inicializado (own_addr=0x%04x) - modo PONG, esperando N nodos", PROV_OWN_ADDR);
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
    if (err != ESP_OK) return;

    err = ble_mesh_nvs_open(&NVS_HANDLE);
    if (err) return;

    ble_mesh_get_dev_uuid(dev_uuid);
    ble_mesh_init();

    if (wifi_init_sta() != ESP_OK) {
        ESP_LOGW(TAG, "WiFi no confirmo conexion en 15s -- seguira reintentando en background");
    }
    mqtt_handler_start();
}


void example_ble_mesh_send_vendor_message(bool resend)
{
    ESP_LOGI(TAG, "Botón físico presionado (modo PONG central, sin acción asociada)");
}