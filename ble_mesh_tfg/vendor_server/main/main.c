/* main.c - VENDOR NODE ("PINGER")
 *
 * Este nodo se deja provisionar de forma pasiva por el cliente central
 * (igual que antes: esp_ble_mesh_node_prov_enable). Lo que cambia es
 * que ahora ALOJA el modelo vendor CLIENT: en cuanto queda configurado
 * (Model App Bind confirmado), lanza su propia tarea de pings dirigida
 * SIEMPRE a la misma dirección fija: la del provisioner central.
 *
 * Flashea este mismo firmware en tantos dispositivos como "N servers"
 * quieras -- cada uno se provisiona por turnos contra el mismo cliente
 * central y, una vez configurado, empieza a hacerle ping de forma
 * independiente.
 */

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

/* IMPORTANTE: debe coincidir EXACTAMENTE con PROV_OWN_ADDR del
 * firmware del cliente central -- es la dirección a la que este
 * nodo mandará todos sus pings. */
#define TARGET_ADDR  0x0001

/* IMPORTANTE: debe coincidir con APP_KEY_IDX del cliente central.
 * Al ser la primera (y única) AppKey añadida a una red nueva, el
 * stack le asigna el índice 0 -- por eso podemos asumirlo fijo aquí
 * sin tener que negociarlo dinámicamente. */
#define APP_KEY_IDX  0x0000

#define MSG_SEND_TTL 3
#define MSG_TIMEOUT  0
#define MSG_ROLE     ROLE_NODE

/* Mismos nombres/valores que en el cliente central -- ver comentario
 * equivalente en ese fichero. */
#define ESP_BLE_MESH_VND_MODEL_ID_CLIENT    0x0000   /* AQUÍ: este nodo inicia el PING */
#define ESP_BLE_MESH_VND_MODEL_ID_SERVER    0x0001   /* En el provisioner: responde PONG */

#define ESP_BLE_MESH_VND_MODEL_OP_PING      ESP_BLE_MESH_MODEL_OP_3(0x01, CID_ESP)
#define ESP_BLE_MESH_VND_MODEL_OP_PONG      ESP_BLE_MESH_MODEL_OP_3(0x02, CID_ESP)

typedef struct __attribute__((packed)) {
    uint16_t seq_num;
    int64_t  timestamp;
} ping_payload_t;

#define TOTAL_PING_MESSAGES 50

static uint16_t rx_count = 0;
static uint16_t lost_count = 0;
static int64_t  start_test_time = 0;
static TaskHandle_t ping_task_handle = NULL;

/* net_idx sí se aprende dinámicamente del evento de provisioning
 * (siempre será 0 = ESP_BLE_MESH_KEY_PRIMARY en este ejemplo, pero lo
 * leemos del evento real en vez de asumirlo, por si acaso). */
static uint16_t node_net_idx = 0;

static uint8_t dev_uuid[ESP_BLE_MESH_OCTET16_LEN] = { 0x32, 0x10 };

static esp_ble_mesh_cfg_srv_t config_server = {
    .net_transmit = ESP_BLE_MESH_TRANSMIT(2, 20),
    .relay = ESP_BLE_MESH_RELAY_DISABLED,
    .relay_retransmit = ESP_BLE_MESH_TRANSMIT(2, 20),
    .beacon = ESP_BLE_MESH_BEACON_ENABLED,
#if defined(CONFIG_BLE_MESH_GATT_PROXY_SERVER)
    .gatt_proxy = ESP_BLE_MESH_GATT_PROXY_ENABLED,
#else
    .gatt_proxy = ESP_BLE_MESH_GATT_PROXY_NOT_SUPPORTED,
#endif
#if defined(CONFIG_BLE_MESH_FRIEND)
    .friend_state = ESP_BLE_MESH_FRIEND_ENABLED,
#else
    .friend_state = ESP_BLE_MESH_FRIEND_NOT_SUPPORTED,
#endif
    .default_ttl = 7,
};

/* NUEVO respecto al nodo original: necesitamos esp_ble_mesh_client_t +
 * op_pair, porque este nodo ahora ALOJA el modelo vendor CLIENT
 * (antes solo tenía un modelo Server pasivo y no lo necesitaba). */
static const esp_ble_mesh_client_op_pair_t vnd_op_pair[] = {
    { ESP_BLE_MESH_VND_MODEL_OP_PING, ESP_BLE_MESH_VND_MODEL_OP_PONG },
};

static esp_ble_mesh_client_t vendor_client = {
    .op_pair_size = ARRAY_SIZE(vnd_op_pair),
    .op_pair = vnd_op_pair,
};

static esp_ble_mesh_model_op_t vnd_op[] = {
    ESP_BLE_MESH_MODEL_OP(ESP_BLE_MESH_VND_MODEL_OP_PONG, sizeof(ping_payload_t)),
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

/* Tarea de FreeRTOS con lógica Stop-and-Wait y timeout de 5s.
 * Misma lógica que antes vivía en el cliente: ahora vive aquí, en
 * cada nodo, y siempre apunta a TARGET_ADDR (el provisioner central). */
static void ping_test_task(void *pvParameters) {
    
    ESP_LOGI(TAG, "=== INICIANDO TEST DE RENDIMIENTO (STOP-AND-WAIT) ===");
    vTaskDelay(2000 / portTICK_PERIOD_MS);

    esp_ble_mesh_msg_ctx_t ctx = {
        .net_idx = node_net_idx,
        .app_idx = APP_KEY_IDX,
        .addr = TARGET_ADDR,
        .send_ttl = MSG_SEND_TTL,
    };

    start_test_time = esp_timer_get_time();
    rx_count = 0;
    lost_count = 0;

    for (uint16_t tx_count = 1; tx_count <= TOTAL_PING_MESSAGES; tx_count++) {
        ping_payload_t payload = {
            .seq_num = tx_count,
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
    float packet_loss = ((float)lost_count / TOTAL_PING_MESSAGES) * 100.0;
    float throughput = (rx_count * sizeof(ping_payload_t) * 8.0) / (total_time / 1000000.0);

    ESP_LOGI(TAG, "=== RESULTADOS FINALES ===");
    ESP_LOGI(TAG, "Enviados: %d | Recibidos: %d | Perdidos (Timeout): %d", TOTAL_PING_MESSAGES, rx_count, lost_count);
    ESP_LOGI(TAG, "Packet Loss: %.2f %%", packet_loss);
    ESP_LOGI(TAG, "Throughput Aproximado: %.2f bps", throughput);

    ping_task_handle = NULL;
    vTaskDelete(NULL);
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
            /* Justo lo que vimos antes: solo lanzamos la tarea de
             * pings DESPUÉS de que el bind se haya confirmado, nunca
             * antes (si no, el mensaje saldría sin autorización). */
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
            rx_count++;
            float rtt_ms = (float)(time_now - recv_payload->timestamp) / 1000.0;
            ESP_LOGI(TAG, "PONG #%d recibido. RTT: %.2f ms", recv_payload->seq_num, rtt_ms);

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

    /* NUEVO respecto al nodo original: hace falta inicializar el
     * modelo como Client (antes no existía esta llamada aquí, porque
     * el nodo original solo tenía un modelo Server pasivo). */
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