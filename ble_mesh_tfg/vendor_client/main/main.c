/* main.c - VENDOR CLIENT / PROVISIONER CENTRAL ("PONGER")
 *
 * Este nodo sigue siendo el PROVISIONER (provisiona a todos los nodos
 * "pinger" que se anuncien con el UUID esperado), pero ya NO inicia
 * pings. Ahora aloja un modelo vendor SERVER: se queda esperando PINGs
 * de cualquier nodo ya provisionado y responde con PONG.
 *
 * Como solo existe UN provisioner en esta topología (N nodos -> 1
 * cliente central), su dirección puede volver a ser fija: no hay
 * riesgo de colisión entre "dos provisioners" como antes.
 */

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


#define TAG "EXAMPLE_CLIENT"

#define CID_ESP             0x02E5
#define PROV_OWN_ADDR       0x0001   /* Fija: solo hay UN provisioner en esta topología */
#define PROV_START_ADDR     0x0005
#define MSG_SEND_TTL        3
#define MSG_TIMEOUT         0
#define MSG_ROLE            ROLE_PROVISIONER
#define COMP_DATA_PAGE_0    0x00
#define APP_KEY_IDX         0x0000
#define APP_KEY_OCTET       0x12

/* IMPORTANTE: estas dos constantes deben coincidir EXACTAMENTE (mismo
 * valor) con las del firmware de los nodos "pinger". El nombre CLIENT/
 * SERVER ahora describe el rol dentro del intercambio PING-PONG, no
 * quién provisiona a quién:
 *   - VND_MODEL_ID_CLIENT: quien INICIA el ping (ahora: cada nodo)
 *   - VND_MODEL_ID_SERVER: quien RESPONDE el pong (ahora: este cliente) */
#define ESP_BLE_MESH_VND_MODEL_ID_CLIENT    0x0000
#define ESP_BLE_MESH_VND_MODEL_ID_SERVER    0x0001

#define ESP_BLE_MESH_VND_MODEL_OP_PING      ESP_BLE_MESH_MODEL_OP_3(0x01, CID_ESP)
#define ESP_BLE_MESH_VND_MODEL_OP_PONG      ESP_BLE_MESH_MODEL_OP_3(0x02, CID_ESP)

typedef struct __attribute__((packed)) {
    uint16_t seq_num;
    int64_t  timestamp;
} ping_payload_t;

static uint8_t dev_uuid[ESP_BLE_MESH_OCTET16_LEN];

/* Guarda el ÚLTIMO nodo provisionado, solo a título informativo/NVS
 * (ya no se usa para dirigir pings, porque ahora el cliente no envía
 * pings a nadie: son los nodos quienes le escriben a él). */
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

/* Modelo vendor LOCAL de este nodo: ahora es un modelo SERVER (pasivo,
 * solo responde). Ya no necesita esp_ble_mesh_client_t/op_pair, porque
 * ese struct es para modelos que INICIAN mensajes con seguimiento de
 * respuesta -- eso ahora vive en cada nodo, no aquí. */
static esp_ble_mesh_model_op_t vnd_op[] = {
    ESP_BLE_MESH_MODEL_OP(ESP_BLE_MESH_VND_MODEL_OP_PING, sizeof(ping_payload_t)),
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

/* Dirección fija: al ser constantes de compilación, sí podemos
 * inicializarla directamente como 'static' sin ningún truco de cast
 * (a diferencia de cuando la calculábamos en tiempo de ejecución). */
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
            /* Bind local: ahora bindeamos el modelo SERVER local
             * (antes era el CLIENT), porque este nodo aloja el
             * modelo que responde PONG. */
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
            /* CAMBIO: bindeamos el modelo CLIENT del nodo remoto
             * (antes era SERVER), porque ahora el nodo es quien
             * inicia el PING. */
            set.model_app_bind.model_id = ESP_BLE_MESH_VND_MODEL_ID_CLIENT;
            set.model_app_bind.company_id = CID_ESP;
            esp_ble_mesh_config_client_set_state(&common, &set);
        } else if (param->params->opcode == ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND) {
            ESP_LOGW(TAG, "Nodo 0x%04x configurado y listo para iniciar sus pings", node->unicast_addr);
            /* Ya NO lanzamos ninguna tarea aquí: el propio nodo
             * arrancará su ping_test_task al recibir este mismo
             * evento MODEL_APP_BIND, pero en su lado (config server). */
        }
        break;
    default:
        break;
    }
}

/* Ahora responde PONG a cualquier PING entrante, venga del nodo que
 * venga (misma lógica que antes tenía el servidor original). */
static void example_ble_mesh_custom_model_cb(esp_ble_mesh_model_cb_event_t event,
                                             esp_ble_mesh_model_cb_param_t *param)
{
    switch (event) {
    case ESP_BLE_MESH_MODEL_OPERATION_EVT:
        if (param->model_operation.opcode == ESP_BLE_MESH_VND_MODEL_OP_PING) {
            ping_payload_t *payload = (ping_payload_t *)param->model_operation.msg;
            ESP_LOGI(TAG, "PING #%d recibido de 0x%04x. Devolviendo PONG...",
                     payload->seq_num, param->model_operation.ctx->addr);

            esp_err_t err = esp_ble_mesh_server_model_send_msg(&vnd_models[0],
                    param->model_operation.ctx, ESP_BLE_MESH_VND_MODEL_OP_PONG,
                    sizeof(ping_payload_t), (uint8_t *)payload);
            if (err) {
                ESP_LOGE(TAG, "Error devolviendo PONG a 0x%04x", param->model_operation.ctx->addr);
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

    /* Ya NO llamamos a esp_ble_mesh_client_model_init() aquí: el
     * modelo vendor local es un modelo SERVER (pasivo), no un modelo
     * Client con seguimiento de op_pair -- eso ahora vive en cada
     * nodo (ver su ble_mesh_init()). */

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
}

/* board.c llama a esta función desde el callback del botón físico
 * (button_tap_cb). Debe existir aunque no hagamos nada especial con
 * ella, o el enlazador falla con 'undefined reference'. */
void example_ble_mesh_send_vendor_message(bool resend)
{
    ESP_LOGI(TAG, "Botón físico presionado (modo PONG central, sin acción asociada)");
}