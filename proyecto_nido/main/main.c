#include <stdio.h>
#include "esp_log.h"
#include "esp_camera.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_http_client.h"
#include "wifi_settings.h"
#include "esp_sleep.h"
#include <time.h>
#include <sys/time.h>
#include "esp_sntp.h"

static const char *TAG = "CAMARA_NIDO";

// --- GRUPO DE EVENTOS PARA ESPERAR AL WI-FI ---
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

// --- 1. MAPA DE PINES CÁMARA ---
#define CAM_PIN_PWDN 32
#define CAM_PIN_RESET -1
#define CAM_PIN_XCLK 0
#define CAM_PIN_SIOD 26
#define CAM_PIN_SIOC 27
#define CAM_PIN_D7 35
#define CAM_PIN_D6 34
#define CAM_PIN_D5 39
#define CAM_PIN_D4 36
#define CAM_PIN_D3 21
#define CAM_PIN_D2 19
#define CAM_PIN_D1 18
#define CAM_PIN_D0 5
#define CAM_PIN_VSYNC 25
#define CAM_PIN_HREF 23
#define CAM_PIN_PCLK 22

static camera_config_t camera_config = {
    .pin_pwdn = CAM_PIN_PWDN,
    .pin_reset = CAM_PIN_RESET,
    .pin_xclk = CAM_PIN_XCLK,
    .pin_sscb_sda = CAM_PIN_SIOD,
    .pin_sscb_scl = CAM_PIN_SIOC,
    .pin_d7 = CAM_PIN_D7,
    .pin_d6 = CAM_PIN_D6,
    .pin_d5 = CAM_PIN_D5,
    .pin_d4 = CAM_PIN_D4,
    .pin_d3 = CAM_PIN_D3,
    .pin_d2 = CAM_PIN_D2,
    .pin_d1 = CAM_PIN_D1,
    .pin_d0 = CAM_PIN_D0,
    .pin_vsync = CAM_PIN_VSYNC,
    .pin_href = CAM_PIN_HREF,
    .pin_pclk = CAM_PIN_PCLK,
    .xclk_freq_hz = 20000000,
    .ledc_timer = LEDC_TIMER_0,
    .ledc_channel = LEDC_CHANNEL_0,
    .pixel_format = PIXFORMAT_JPEG,
    .frame_size = FRAMESIZE_UXGA,
    .jpeg_quality = 12,
    .fb_count = 1,
    .grab_mode = CAMERA_GRAB_WHEN_EMPTY};



// --- 2. MANEJADOR DE EVENTOS WI-FI ---
static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        ESP_LOGW(TAG, "Conexión Wi-Fi perdida. Reintentando reconectar...");
        esp_wifi_connect();
        // Aseguramos que el bit esté apagado si nos desconectamos
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "¡Conectado al Wi-Fi! IP asignada: " IPSTR, IP2STR(&event->ip_info.ip));
        // Encendemos el semáforo en verde para que el programa continúe
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// --- 3. INICIALIZACIÓN DE WI-FI ---
void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        ESP_LOGE(TAG, "No se pudo crear el grupo de eventos Wi-Fi");
        return;
    }

    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL);

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);

    ESP_LOGI(TAG, "Conexión confirmada. Continuando con la lógica principal...");
}

// --- 4. FUNCIÓN PARA ENVIAR LA FOTO ---
void enviar_foto_a_ubuntu(camera_fb_t *fb)
{
    esp_http_client_config_t config = {
        .url = SERVER_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_post_field(client, (const char *)fb->buf, fb->len);
    esp_http_client_set_header(client, "Content-Type", "image/jpeg");

    ESP_LOGI(TAG, "Enviando foto de %zu bytes al servidor %s...", fb->len, SERVER_URL);
    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "¡Foto enviada y guardada con éxito en Ubuntu!");
    }
    else
    {
        ESP_LOGE(TAG, "Fallo al enviar la foto por HTTP: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
}

// --- 5. OBTENER HORA POR INTERNET ---
void obtener_hora_ntp()
{
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    ESP_LOGI(TAG, "Sincronizando hora con el servidor NTP...");
    int retry = 0;
    const int retry_count = 10;

    while (sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET && ++retry < retry_count)
    {
        ESP_LOGI(TAG, "Esperando a la hora del sistema... (%d/%d)", retry, retry_count);
        vTaskDelay(2000 / portTICK_PERIOD_MS);
    }

    // Configurar zona horaria de España
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();
}

// --- 6. FUNCIÓN PRINCIPAL ---
void app_main(void)
{
    ESP_LOGI(TAG, "==== INICIANDO SISTEMA DEL NIDO ====");

    // Inicializar NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Conectar al Wi-Fi
    wifi_init_sta();

    // Obtener la hora actual
    obtener_hora_ntp();

    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    int hora_actual = timeinfo.tm_hour;
    ESP_LOGI(TAG, "La hora actual es: %02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);

    // --- LÓGICA DE DÍA Y NOCHE ---
    int hora_inicio_dia = 7;    // 7:00 AM
    int hora_inicio_noche = 22; // 22:00 (10 PM)
    uint64_t tiempo_dormir_us = 0;

    if (hora_actual >= hora_inicio_dia && hora_actual < hora_inicio_noche)
    {
        // ========== ES DE DÍA ==========
        ESP_LOGI(TAG, "Es de día. Preparando cámara para la foto...");

        esp_err_t err = esp_camera_init(&camera_config);
        if (err == ESP_OK)
        {

            ESP_LOGI(TAG, "Dejando que el sensor ajuste la luz...");
            vTaskDelay(pdMS_TO_TICKS(2000)); // Esperamos 2 segundos

            // Descartamos las 2 primeras fotos malas (buffer antiguo)
            camera_fb_t *fb_malo = NULL;
            for (int i = 0; i < 2; i++)
            {
                fb_malo = esp_camera_fb_get();
                if (fb_malo)
                    esp_camera_fb_return(fb_malo);
            }

            // Ahora sí, tomamos la foto definitiva y estabilizada
            ESP_LOGI(TAG, "Tomando la foto definitiva...");
            camera_fb_t *fb = esp_camera_fb_get();

            if (!fb)
            {
                ESP_LOGE(TAG, "Fallo al capturar la imagen");
            }
            else
            {
                ESP_LOGI(TAG, "¡Foto capturada con éxito! Tamaño: %zu bytes", fb->len);
                enviar_foto_a_ubuntu(fb);
                esp_camera_fb_return(fb);
            }
        }
        else
        {
            ESP_LOGE(TAG, "Fallo al inicializar la cámara: 0x%x", err);
        }

        // Dormir 30 minutos exactos (1800 segundos)
        tiempo_dormir_us = 1800ULL * 1000000ULL;
        ESP_LOGI(TAG, "Entrando en Deep Sleep durante 30 minutos...");
    }
    else
    {
        // ========== ES DE NOCHE ==========
        ESP_LOGI(TAG, "Es de noche. La cámara no se activará para ahorrar batería.");

        int horas_para_dormir = 0;
        if (hora_actual >= hora_inicio_noche)
        {
            horas_para_dormir = (24 - hora_actual) + hora_inicio_dia;
        }
        else
        {
            horas_para_dormir = hora_inicio_dia - hora_actual;
        }

        int segundos_para_despertar = (horas_para_dormir * 3600) - (timeinfo.tm_min * 60) - timeinfo.tm_sec;
        tiempo_dormir_us = (uint64_t)segundos_para_despertar * 1000000ULL;

        ESP_LOGI(TAG, "Entrando en Deep Sleep hasta las %02d:00...", hora_inicio_dia);
    }

    // Ejecutar el Deep Sleep (el sistema se apagará aquí y despertará limpio al acabar el tiempo)
    esp_sleep_enable_timer_wakeup(tiempo_dormir_us);
    esp_deep_sleep_start();
}