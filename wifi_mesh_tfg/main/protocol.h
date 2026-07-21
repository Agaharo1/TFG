#pragma once

#include <stdint.h>

#define PROTO_VERSION       1


typedef enum {
    MSG_METRICS = 0x01,   
    MSG_PING    = 0x02,   
    MSG_PONG    = 0x03,   
    MSG_CMD     = 0x04,   
    MSG_EXP_RESULT = 99, 
    MSG_EXP_DUMMY = 100
} mesh_msg_type_t;


typedef struct __attribute__((packed)) {
    uint8_t  version;         
    uint8_t  type;            
    uint8_t  src_mac[6];     
    uint32_t seq;             
    uint32_t timestamp_ms;   
} mesh_hdr_t;


typedef struct __attribute__((packed)) {
    int8_t   rssi_parent;      
    int8_t   rssi_router;      
    int8_t   snr_estimate;     
    uint8_t  layer;            
    uint8_t  hops_to_root;     
    uint8_t  connected_subs;   
    uint32_t tx_count;         
    uint32_t rx_count;         
    uint32_t tx_fail;          
    uint32_t latency_ms;       
    uint32_t free_heap;        
    uint32_t uptime_s;         
    uint32_t ping_lost_count;  
    uint32_t power_json_prev_mw;
    char i2c_raw[48];
} metrics_payload_t;

typedef struct
{
    mesh_hdr_t hdr;
    uint32_t kb;
    uint32_t time_ms;
    uint32_t p_idle;
    uint32_t p_active;
} exp_packet_t;



typedef struct __attribute__((packed)) {
    uint32_t ping_seq;       
    uint32_t sent_ms;          
    uint32_t recv_ms;         
} ping_payload_t;


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
