#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "mqtt_client.h"
#include "esp_random.h"

// --- CONFIGURACIÓN ---
#define WIFI_SSID      ""
#define WIFI_PASS      ""
#define MQTT_BROKER_IP "mqtt://192.168.1.44" // IP de tu Ubuntu Server

static const char *TAG = "ESP32_IOT";

// --- 1. TAREA DE FREERTOS PARA GENERAR Y PUBLICAR DATOS ---
// --- 1. TAREA DE FREERTOS ACTUALIZADA ---
static void publisher_task(void *pvParameters) {
    esp_mqtt_client_handle_t client = (esp_mqtt_client_handle_t) pvParameters;
    char temp_str[10];
    char hum_str[10];
    int contador_mensajes = 0; // Iniciamos el contador

    ESP_LOGI(TAG, "Iniciando ráfaga de 200 mensajes (1 por segundo)...");

    while (contador_mensajes < 200) {
        // Generar números aleatorios
        float temp = 20.0 + ((esp_random() % 101) / 10.0); 
        float hum = 40.0 + ((esp_random() % 201) / 10.0);

        snprintf(temp_str, sizeof(temp_str), "%.1f", temp);
        snprintf(hum_str, sizeof(hum_str), "%.1f", hum);

        // Publicar datos
        esp_mqtt_client_publish(client, "casa/temperatura", temp_str, 0, 1, 0);
        esp_mqtt_client_publish(client, "casa/humedad", hum_str, 0, 1, 0);

        contador_mensajes++; // Incrementamos el contador

        ESP_LOGI(TAG, "[Mensaje %d/200] Enviado -> Temp: %s | Hum: %s", 
                 contador_mensajes, temp_str, hum_str);

        // Esperar exactamente 1 segundo (1000 ms)
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGW(TAG, "Se han enviado 200 mensajes. Deteniendo publicaciones.");
    
    // Opcional: Si quieres que el ESP32 se desconecte al terminar
    // esp_mqtt_client_stop(client);

    // Eliminamos la tarea para liberar memoria RAM
    vTaskDelete(NULL); 
}

// --- 2. EVENTOS MQTT ---
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;

    if (event_id == MQTT_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "¡Conectado al broker Mosquitto!");
        // Arrancamos la tarea de publicar datos
        xTaskCreate(publisher_task, "mqtt_publisher", 2048, (void *)client, 5, NULL);
    } else if (event_id == MQTT_EVENT_DISCONNECTED) {
        ESP_LOGW(TAG, "Desconectado del broker MQTT");
    }
}

// --- 3. INICIAR MQTT ---
static void iniciar_mqtt(void) {
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_IP,
        .broker.address.port = 1883,
    };

    ESP_LOGI(TAG, "Iniciando cliente MQTT...");
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);
}

// --- 4. EVENTOS WI-FI ---
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        ESP_LOGI(TAG, "Conectando al Wi-Fi...");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Fallo de conexión Wi-Fi, reintentando...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "¡Conectado! IP asignada: " IPSTR, IP2STR(&event->ip_info.ip));
        
        // --- ¡AQUÍ ESTÁ LA MAGIA! ---
        // Solo cuando tenemos IP de red, arrancamos el cliente MQTT
        iniciar_mqtt();
    }
}

// --- 5. INICIAR WI-FI ---
void iniciar_wifi(void) {
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();
}

// --- 6. FUNCIÓN PRINCIPAL ---
void app_main(void) {
    // Inicializar la memoria Flash (NVS) requerida para el Wi-Fi
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "Iniciando sistema ESP-IDF...");
    iniciar_wifi();
}
