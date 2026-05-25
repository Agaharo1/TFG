#pragma once

#include <stdint.h>

/* ─── Versión del protocolo interno mesh ─────────────────────────────────── */
#define PROTO_VERSION       1

/* ─── Tipos de mensaje ───────────────────────────────────────────────────── */
typedef enum {
    MSG_METRICS = 0x01,   /* nodo → root: payload de métricas             */
    MSG_PING    = 0x02,   /* root/nodo → nodo: solicitud de latencia       */
    MSG_PONG    = 0x03,   /* nodo → root: respuesta a ping con timestamp   */
    MSG_CMD     = 0x04,   /* root → nodo: comando remoto (futuro)          */
} mesh_msg_type_t;

/* ─── Cabecera común a todos los mensajes ────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint8_t  version;          /* PROTO_VERSION                            */
    uint8_t  type;             /* mesh_msg_type_t                          */
    uint8_t  src_mac[6];       /* MAC del nodo origen                      */
    uint32_t seq;              /* Número de secuencia (monotónico)         */
    uint32_t timestamp_ms;     /* esp_log_timestamp() en el momento de envío*/
} mesh_hdr_t;

/* ─── Payload de métricas ────────────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    int8_t   rssi_parent;      /* RSSI hacia el nodo padre [dBm]           */
    int8_t   rssi_router;      /* RSSI hacia el router (solo root) [dBm]   */
    int8_t   snr_estimate;     /* SNR estimado (noise floor empírico)      */
    uint8_t  layer;            /* Capa en el árbol mesh (0 = root)         */
    uint8_t  hops_to_root;     /* Saltos hasta el root                     */
    uint8_t  connected_subs;   /* Nodos hijos directos                     */
    uint32_t tx_count;         /* Paquetes enviados desde el arranque      */
    uint32_t rx_count;         /* Paquetes recibidos desde el arranque     */
    uint32_t tx_fail;          /* Fallos de envío acumulados               */
    uint32_t latency_ms;       /* Última latencia RTT medida [ms]          */
    uint32_t free_heap;        /* Heap libre [bytes]                       */
    uint32_t uptime_s;         /* Tiempo activo [segundos]                 */
    uint32_t ping_lost_count;
} metrics_payload_t;

/* ─── Payload de ping / pong ─────────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t ping_seq;         /* Eco del seq del ping original            */
    uint32_t sent_ms;          /* Timestamp de envío del ping              */
    uint32_t recv_ms;          /* Timestamp de recepción (relleno en pong) */
} ping_payload_t;

/* ─── Paquete completo (cabecera + payload) ──────────────────────────────── */
#define MAX_PAYLOAD_SIZE sizeof(metrics_payload_t)

typedef struct __attribute__((packed)) {
    mesh_hdr_t hdr;
    union {
        metrics_payload_t metrics;
        ping_payload_t    ping;
        uint8_t           raw[MAX_PAYLOAD_SIZE];
    } payload;
} mesh_packet_t;

#define MESH_PACKET_SIZE sizeof(mesh_packet_t)
