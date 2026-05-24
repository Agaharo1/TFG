#include "mesh_handler.h"
#include "mqtt_handler.h"
#include "metrics.h"
#include "protocol.h"

#include "esp_mesh.h"
#include "esp_mesh_internal.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_event.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "MESH";

/* ─── Fallbacks por si Kconfig no los definió todavía ───────────────────── */
#ifndef CONFIG_MESH_SSID
#define CONFIG_MESH_SSID           "TFG_Mesh_Network"
#endif
#ifndef CONFIG_MESH_PASSWORD
#define CONFIG_MESH_PASSWORD       "mesh_pass_2024"
#endif
#ifndef CONFIG_MESH_ROUTER_SSID
#define CONFIG_MESH_ROUTER_SSID    "MiRouter_SSID"
#endif
#ifndef CONFIG_MESH_ROUTER_PASS
#define CONFIG_MESH_ROUTER_PASS    "router_password"
#endif
#ifndef CONFIG_MESH_MAX_LAYER
#define CONFIG_MESH_MAX_LAYER      4
#endif
#ifndef CONFIG_MESH_TX_INTERVAL_MS
#define CONFIG_MESH_TX_INTERVAL_MS 5000
#endif
#ifndef CONFIG_MESH_PING_INTERVAL_MS
#define CONFIG_MESH_PING_INTERVAL_MS 10000
#endif
/* CONFIG_MESH_ROUTE_TABLE_SIZE es un Kconfig interno de esp_mesh (default 50) */
#ifndef CONFIG_MESH_ROUTE_TABLE_SIZE
#define CONFIG_MESH_ROUTE_TABLE_SIZE 50
#endif

/* ID de red mesh — 6 bytes únicos para este despliegue */
static const uint8_t MESH_ID[6] = { 0x77, 0x77, 0x77, 0x77, 0x77, 0x77 };

/* ─── Estado ─────────────────────────────────────────────────────────────── */
static EventGroupHandle_t  s_mesh_evt_group;
#define MESH_CONNECTED_BIT   BIT0
#define ROUTER_CONNECTED_BIT BIT1

static uint8_t    s_my_mac[6]   = {0};
static mesh_addr_t s_root_addr  = {{{0}}};  /* MAC del root elegido */
static bool       s_root_known  = false;

static uint32_t   s_tx_seq      = 0;
static uint32_t   s_ping_seq    = 0;

uint32_t last_known_latency = 0; // Variable global para almacenar la última latencia conocida, accesible desde mqtt_handler.c

#define MAX_PENDING_PINGS 8
static struct {
    uint32_t seq;
    uint32_t sent_ms;
} s_pending_pings[MAX_PENDING_PINGS];
static int s_pending_count = 0;

/* ─── Helpers ────────────────────────────────────────────────────────────── */

static inline uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static void build_header(mesh_hdr_t *hdr, mesh_msg_type_t type)
{
    hdr->version      = PROTO_VERSION;
    hdr->type         = (uint8_t)type;
    memcpy(hdr->src_mac, s_my_mac, 6);
    hdr->seq          = ++s_tx_seq;
    hdr->timestamp_ms = now_ms();
}

/* ─── Envío de métricas hacia el root ───────────────────────────────────── */

static void send_metrics_to_root(void)
{
    if (!s_root_known) {
        ESP_LOGD(TAG, "Root aún desconocido — esperando MESH_EVENT_ROOT_ADDRESS");
        return;
    }

    mesh_packet_t pkt;
    build_header(&pkt.hdr, MSG_METRICS);
    metrics_collect(&pkt.payload.metrics);

    mesh_data_t data = {
        .data  = (uint8_t *)&pkt,
        .size  = MESH_PACKET_SIZE,
        .proto = MESH_PROTO_BIN,
        .tos   = MESH_TOS_P2P,
    };

    /* Dirección P2P al root cuya MAC obtuvimos de MESH_EVENT_ROOT_ADDRESS */
    esp_err_t ret = esp_mesh_send(&s_root_addr, &data, MESH_DATA_P2P, NULL, 0);
    metrics_record_tx(ret == ESP_OK);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "send_metrics error: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGD(TAG, "Métricas → root (seq=%lu)", (unsigned long)s_tx_seq);
    }
}

/* ─── Ping desde el root a todos los nodos ───────────────────────────────── */

static void ping_all_nodes(void)
{
    int node_num = esp_mesh_get_routing_table_size();
    if (node_num <= 0) return;

    /* Tabla en heap para no depender de tamaño estático */
    mesh_addr_t *table = malloc((size_t)node_num * sizeof(mesh_addr_t));
    if (!table) return;

    int actual = 0;
    esp_mesh_get_routing_table(table, node_num * (int)sizeof(mesh_addr_t), &actual);

    for (int i = 0; i < actual; i++) {
        if (memcmp(table[i].addr, s_my_mac, 6) == 0) continue;

        mesh_packet_t pkt;
        build_header(&pkt.hdr, MSG_PING);
        pkt.payload.ping.ping_seq = ++s_ping_seq;
        pkt.payload.ping.sent_ms  = now_ms();
        pkt.payload.ping.recv_ms  = 0;

        if (s_pending_count < MAX_PENDING_PINGS) {
            s_pending_pings[s_pending_count].seq     = s_ping_seq;
            s_pending_pings[s_pending_count].sent_ms = pkt.payload.ping.sent_ms;
            s_pending_count++;
        }

        mesh_data_t data = {
            .data  = (uint8_t *)&pkt,
            .size  = MESH_PACKET_SIZE,
            .proto = MESH_PROTO_BIN,
            .tos   = MESH_TOS_P2P,
        };
        esp_mesh_send(&table[i], &data, MESH_DATA_P2P, NULL, 0);
        ESP_LOGI(TAG, "PING → %02x:%02x:%02x:%02x:%02x:%02x", MAC2STR(table[i].addr));
    }
    free(table);
}

/* ─── Responder PONG ─────────────────────────────────────────────────────── */

static void reply_pong(const mesh_addr_t *requester, const ping_payload_t *ping)
{
    mesh_packet_t pkt;
    build_header(&pkt.hdr, MSG_PONG);
    pkt.payload.ping.ping_seq = ping->ping_seq;
    pkt.payload.ping.sent_ms  = ping->sent_ms;
    pkt.payload.ping.recv_ms  = now_ms();

    mesh_data_t data = {
        .data  = (uint8_t *)&pkt,
        .size  = MESH_PACKET_SIZE,
        .proto = MESH_PROTO_BIN,
        .tos   = MESH_TOS_P2P,
    };
    esp_mesh_send(requester, &data, MESH_DATA_P2P, NULL, 0);
}

/* ─── Procesar PONG recibido en el root ──────────────────────────────────── */

static void process_pong(const uint8_t *src_mac, const ping_payload_t *pong)
{
    uint32_t rtt = now_ms() - pong->sent_ms;
    
    ESP_LOGI(TAG, "PONG recibido de %02x:%02x:%02x:%02x:%02x:%02x — RTT=%lu ms",
             src_mac[0], src_mac[1], src_mac[2], src_mac[3], src_mac[4], src_mac[5],
             (unsigned long)rtt);

    extern uint32_t last_known_latency;
    last_known_latency = rtt;

    for (int i = 0; i < s_pending_count; i++) {
        if (s_pending_pings[i].seq == pong->ping_seq) {
            s_pending_pings[i] = s_pending_pings[--s_pending_count];
            break;
        }
    }
}

/* ─── Tarea RX ───────────────────────────────────────────────────────────── */

static void rx_task(void *arg)
{
    static uint8_t rx_buf[MESH_PACKET_SIZE + 64];
    mesh_addr_t    from;
    mesh_data_t    data;
    int            flag = 0;

    while (true) {
        data.data = rx_buf;
        data.size = sizeof(rx_buf);

        esp_err_t ret = esp_mesh_recv(&from, &data, portMAX_DELAY, &flag, NULL, 0);
        if (ret != ESP_OK || data.size < (int)sizeof(mesh_hdr_t)) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        metrics_record_rx();

        mesh_packet_t *pkt = (mesh_packet_t *)rx_buf;
        if (pkt->hdr.version != PROTO_VERSION) continue;

        switch ((mesh_msg_type_t)pkt->hdr.type) {

        case MSG_METRICS:
            if (esp_mesh_is_root()) {
                extern uint32_t last_known_latency;
                pkt->payload.metrics.latency_ms = last_known_latency;
                ESP_LOGI(TAG, "Métricas de %02x:%02x:%02x:%02x:%02x:%02x"
                         " | capa=%d rssi=%d dBm latency=%lu ms",
                         MAC2STR(pkt->hdr.src_mac),
                         pkt->payload.metrics.layer,
                         pkt->payload.metrics.rssi_parent,
                         (unsigned long)pkt->payload.metrics.latency_ms);
                mqtt_publish_metrics(pkt->hdr.src_mac,
                                     &pkt->payload.metrics,
                                     pkt->hdr.seq);
            }
            break;

        case MSG_PING:
            ESP_LOGD(TAG, "PING de %02x:%02x:%02x:%02x:%02x:%02x", MAC2STR(from.addr));
            reply_pong(&from, &pkt->payload.ping);
            break;

        case MSG_PONG:
            if (esp_mesh_is_root()) process_pong(pkt->hdr.src_mac, &pkt->payload.ping);
            break;

        default:
            break;
        }
    }
}

/* ─── Tarea TX periódica ─────────────────────────────────────────────────── */

static void tx_metrics_task(void *arg)
{
    xEventGroupWaitBits(s_mesh_evt_group, MESH_CONNECTED_BIT,
                        pdFALSE, pdTRUE, portMAX_DELAY);
    vTaskDelay(pdMS_TO_TICKS(2000));

    while (true) {
        if (!esp_mesh_is_root()) {
            ESP_LOGI(TAG, "Enviando métricas al root...");
            send_metrics_to_root();
        }
        vTaskDelay(pdMS_TO_TICKS(CONFIG_MESH_TX_INTERVAL_MS));
    }
}

/* ─── Tarea de ping (solo root) ──────────────────────────────────────────── */

static void ping_task(void *arg)
{
    ESP_LOGI(TAG, "Esperando conexión a la red mesh para arrancar tarea de ping...");
    xEventGroupWaitBits(s_mesh_evt_group,
                        MESH_CONNECTED_BIT,
                        pdFALSE, pdTRUE, portMAX_DELAY);

    if (!esp_mesh_is_root()) {
        ESP_LOGI(TAG, "No somos root, no arrancamos tarea de ping");
        vTaskDelete(NULL);
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(5000));

    while (true) {
        ESP_LOGI(TAG, "Enviando ping a todos los nodos...");
        ping_all_nodes();
        mqtt_publish_topology();
        vTaskDelay(pdMS_TO_TICKS(CONFIG_MESH_PING_INTERVAL_MS));
        ESP_LOGI(TAG, "Próximo ping en %d ms", CONFIG_MESH_PING_INTERVAL_MS);
    }
}


static void ip_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *data)
{
    if (event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) data;
        

        if (esp_mesh_is_root()) {
            ESP_LOGI(TAG, "============= ¡BINGO! TENEMOS IP COMO ROOT: " IPSTR " =============", IP2STR(&event->ip_info.ip));
            ESP_LOGI(TAG, "Iniciando cliente MQTT...");
            mqtt_handler_start();
        } else {
            ESP_LOGI(TAG, "IP interna asignada (" IPSTR "), nodo hijo. MQTT deshabilitado.", IP2STR(&event->ip_info.ip));
        }
    }
}

/* ─── Event handler ──────────────────────────────────────────────────────── */

static void mesh_event_handler(void *arg, esp_event_base_t base,
                                int32_t event_id, void *data)
{
    mesh_event_info_t *ev = (mesh_event_info_t *)data;

    switch (event_id) {

    case MESH_EVENT_STARTED:
        /* Releer MAC tras arranque WiFi */
        esp_wifi_get_mac(WIFI_IF_STA, s_my_mac);
        ESP_LOGI(TAG, "Mesh iniciada. MAC propia: %02x:%02x:%02x:%02x:%02x:%02x", MAC2STR(s_my_mac));
        break;

    case MESH_EVENT_STOPPED:
        xEventGroupClearBits(s_mesh_evt_group, MESH_CONNECTED_BIT);
        s_root_known = false;
        break;

    case MESH_EVENT_PARENT_CONNECTED:
        ESP_LOGI(TAG, "Padre conectado — capa %d", esp_mesh_get_layer());
        xEventGroupSetBits(s_mesh_evt_group, MESH_CONNECTED_BIT);
        break;

    case MESH_EVENT_PARENT_DISCONNECTED:
        ESP_LOGW(TAG, "Padre desconectado (reason=%d)",
                 ev->disconnected.reason);
        xEventGroupClearBits(s_mesh_evt_group, MESH_CONNECTED_BIT);
        break;

    case MESH_EVENT_ROOT_ADDRESS:
        /* Este evento indica qué nodo es el root — guardamos su MAC */
        memcpy(s_root_addr.addr, ev->root_addr.addr, 6);
        s_root_known = true;
        ESP_LOGI(TAG, "Root elegido: %02x:%02x:%02x:%02x:%02x:%02x", MAC2STR(s_root_addr.addr));
        break;

    /* Nota: MESH_EVENT_ROOT_GOT_IP y MESH_EVENT_ROOT_LOST_IP fueron removidos en ESP-IDF 5.x.
       Los eventos de IP ahora se manejan a través de IP_EVENT en el subsistema de eventos */

    case MESH_EVENT_TODS_STATE:
        ESP_LOGI(TAG, "ToDS: %s",
                 ev->toDS_state == MESH_TODS_REACHABLE ? "alcanzable" : "no alcanzable");
        break;

    default:
        ESP_LOGD(TAG, "Evento mesh id=%ld", (long)event_id);
        break;
    }
}

/* ─── Inicialización ─────────────────────────────────────────────────────── */

esp_err_t mesh_handler_init(void)
{
    s_mesh_evt_group = xEventGroupCreate();

    /* ── WiFi ────────────────────────────────────────────────────────────── */
    /* esp_netif para STA lo crea el caller (main.c) antes de llamar aquí  */
    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* MAC propia — válida solo después de esp_wifi_start()               */
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA, s_my_mac));
    ESP_LOGI(TAG, "MAC del nodo: %02x:%02x:%02x:%02x:%02x:%02x", MAC2STR(s_my_mac));

    /* ── ESP-MESH ────────────────────────────────────────────────────────── */
    ESP_ERROR_CHECK(esp_mesh_init());

    ESP_ERROR_CHECK(esp_event_handler_register(MESH_EVENT, ESP_EVENT_ANY_ID,
                                               mesh_event_handler, NULL));

    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, 
                                               &ip_event_handler, NULL));


    mesh_cfg_t cfg = MESH_INIT_CONFIG_DEFAULT();

    memcpy(cfg.mesh_id.addr, MESH_ID, 6);
    cfg.channel = 0;   /* 0 = scan automático del canal del router */

    /* Router externo */
    cfg.router.ssid_len = (uint8_t)strlen(CONFIG_MESH_ROUTER_SSID);
    memcpy(cfg.router.ssid, CONFIG_MESH_ROUTER_SSID, cfg.router.ssid_len);
    memcpy(cfg.router.password, CONFIG_MESH_ROUTER_PASS,
           strlen(CONFIG_MESH_ROUTER_PASS));

    /* AP interno mesh — la contraseña es lo importante */
    const char mesh_password[] = "mesh_pass_2024";
    uint16_t pwd_len = strlen(mesh_password);
    
    memset(cfg.mesh_ap.password, 0, 65);
    memcpy(cfg.mesh_ap.password, mesh_password, pwd_len);
    
    cfg.mesh_ap.max_connection       = 6;
    cfg.mesh_ap.nonmesh_max_connection = 0;
    
    ESP_LOGI(TAG, "AP Mesh: pwd_len=%u, max_conn=%d", pwd_len, cfg.mesh_ap.max_connection);

    ESP_ERROR_CHECK(esp_mesh_set_config(&cfg));
    ESP_ERROR_CHECK(esp_mesh_set_max_layer(CONFIG_MESH_MAX_LAYER));
    ESP_ERROR_CHECK(esp_mesh_set_ap_authmode(WIFI_AUTH_OPEN));

    /* ── Módulo de métricas ──────────────────────────────────────────────── */
    ESP_ERROR_CHECK(metrics_init());

    /* ── Tareas ──────────────────────────────────────────────────────────── */
    xTaskCreate(rx_task,         "mesh_rx",   4096, NULL, 5, NULL); //Tarea de recepción con prioridad alta
    xTaskCreate(tx_metrics_task, "mesh_tx",   4096, NULL, 4, NULL); //Tarea de envío de métricas con prioridad media
    xTaskCreate(ping_task,       "mesh_ping", 3072, NULL, 3, NULL); //Tarea de ping con prioridad baja (solo root)

    /* ── Arrancar ────────────────────────────────────────────────────────── */
    ESP_ERROR_CHECK(esp_mesh_start());

    ESP_LOGI(TAG, "ESP-MESH v5.x arrancado — esperando conexión...");
    return ESP_OK;
}

bool mesh_is_ready(void)
{
    if (!s_mesh_evt_group) return false;
    return (xEventGroupGetBits(s_mesh_evt_group) & MESH_CONNECTED_BIT) != 0;
}
