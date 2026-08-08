#include "peripherals.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "PERIPHERALS";


#define BUTTON_PIN          GPIO_NUM_16
#define I2C_SLAVE_SCL_IO    22
#define I2C_SLAVE_SDA_IO    21
#define I2C_SLAVE_NUM       I2C_NUM_0
#define ESP_SLAVE_ADDR      0x3F


static SemaphoreHandle_t s_periph_mutex = NULL;
static uint32_t s_power_mw = 0;
static char s_i2c_hex[48] = "";


static void i2c_read_task(void *arg) 
{
    uint8_t data[64];
    static char lcd_text[128] = {0}; 
    static int text_idx = 0;
    static bool high_nibble_ready = false;
    static uint8_t high_nibble = 0;
    static uint32_t ultima_lectura_ms = 0;

    while (true) {
        int size = i2c_slave_read_buffer(I2C_SLAVE_NUM, data, sizeof(data), pdMS_TO_TICKS(100));

        if (size > 0) {
            for (int i = 0; i < size; i++) {
                uint8_t val = data[i];
                bool En = (val & 0x04) != 0; 
                bool RS = (val & 0x01) != 0; 

                if (En) { 
                    if (RS) { 
                        uint8_t nibble = (val & 0xF0);
                        if (!high_nibble_ready) {
                            high_nibble = nibble;
                            high_nibble_ready = true;
                        } else {
                            char c = high_nibble | (nibble >> 4);
                            high_nibble_ready = false;

                            if (c >= 32 && c <= 126) { 
                                if (text_idx < sizeof(lcd_text) - 1) {
                                    lcd_text[text_idx++] = c;
                                    lcd_text[text_idx] = '\0';
                                }
                            }
                        }
                    } else { 
                        high_nibble_ready = false;
                        if (text_idx > 0 && lcd_text[text_idx-1] != ' ') {
                            if (text_idx < sizeof(lcd_text) - 1) {
                                lcd_text[text_idx++] = ' ';
                                lcd_text[text_idx] = '\0';
                            }
                        }
                    }
                }
            }

            xSemaphoreTake(s_periph_mutex, portMAX_DELAY);
            int start_idx = (text_idx > 47) ? text_idx - 47 : 0;
            strncpy(s_i2c_hex, &lcd_text[start_idx], 47);
            s_i2c_hex[47] = '\0';
            xSemaphoreGive(s_periph_mutex);

            char *w_pos = strstr(lcd_text, " W "); 
            if (w_pos != NULL) {
                char *ptr = w_pos - 1;
                while (ptr >= lcd_text && (*ptr == ' ' || (*ptr >= '0' && *ptr <= '9') || *ptr == '.')) {
                    ptr--;
                }
                
                float vatios = atof(ptr + 1);
                
                if (strchr(ptr + 1, '.') != NULL && vatios < 25.0) {
                    uint32_t milivatios = (uint32_t)(vatios * 1000.0);

                    xSemaphoreTake(s_periph_mutex, portMAX_DELAY);
                    s_power_mw = milivatios;
                    xSemaphoreGive(s_periph_mutex);
                    
                    ultima_lectura_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
                }

                memset(lcd_text, 0, sizeof(lcd_text));
                text_idx = 0;
            }

            if (text_idx > 80) {
                memset(lcd_text, 0, sizeof(lcd_text));
                text_idx = 0;
            }
        }

        uint32_t ahora = (uint32_t)(esp_timer_get_time() / 1000ULL);
        if ((ahora - ultima_lectura_ms) > 3000) {
            xSemaphoreTake(s_periph_mutex, portMAX_DELAY);
            s_power_mw = 0; 
            xSemaphoreGive(s_periph_mutex);
        }
    }
}


static void button_ps_task(void *arg) 
{
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << BUTTON_PIN),
        .pull_down_en = 0,
        .pull_up_en = 1
    };
    gpio_config(&io_conf);

    int current_ps_mode = 0; 
    bool last_state = true;

    while (true) {
        bool state = gpio_get_level(BUTTON_PIN);

        if (state == 0 && last_state == 1) {
            vTaskDelay(pdMS_TO_TICKS(50)); // Debounce

            if (gpio_get_level(BUTTON_PIN) == 0) { 
                current_ps_mode = (current_ps_mode + 1) % 3; 

                wifi_ps_type_t ps_type = WIFI_PS_NONE;
                const char* mode_str = "";

                switch(current_ps_mode) {
                    case 0: 
                        ps_type = WIFI_PS_NONE; 
                        mode_str = "NONE (Máximo consumo, mínima latencia)";
                        break;
                    case 1: 
                        ps_type = WIFI_PS_MIN_MODEM; 
                        mode_str = "MIN_MODEM (Equilibrio)";
                        break;
                    case 2: 
                        ps_type = WIFI_PS_MAX_MODEM; 
                        mode_str = "MAX_MODEM (Mínimo consumo, máxima latencia)";
                        break;
                }

                esp_wifi_set_ps(ps_type);
                ESP_LOGW(TAG, "================================================");
                ESP_LOGW(TAG, " BOTÓN PULSADO: Power Save cambiado a: %s", mode_str);
                ESP_LOGW(TAG, "================================================");

                while(gpio_get_level(BUTTON_PIN) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
            }
        }
        last_state = state;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}


uint8_t peripherals_get_ps_mode(void)
{
    wifi_ps_type_t ps_type = WIFI_PS_NONE;
    if (esp_wifi_get_ps(&ps_type) == ESP_OK) {
        return (uint8_t)ps_type;
    }
    return 0;
}




esp_err_t peripherals_init(void)
{
    s_periph_mutex = xSemaphoreCreateMutex();
    if (!s_periph_mutex) {
        ESP_LOGE(TAG, "Error al crear mutex de periféricos");
        return ESP_ERR_NO_MEM;
    }

    i2c_config_t conf_slave = {
        .sda_io_num = I2C_SLAVE_SDA_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_io_num = I2C_SLAVE_SCL_IO,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .mode = I2C_MODE_SLAVE,
        .slave.addr_10bit_en = 0,
        .slave.slave_addr = ESP_SLAVE_ADDR,
    };
    i2c_param_config(I2C_SLAVE_NUM, &conf_slave);
    i2c_driver_install(I2C_SLAVE_NUM, conf_slave.mode, 256, 256, 0);

    xTaskCreate(i2c_read_task, "i2c_read", 2048, NULL, 5, NULL);

    ESP_LOGI(TAG, "Periféricos inicializados (I2C Slave preparado)");
    return ESP_OK;
}

void peripherals_start_button_task(void)
{
    xTaskCreate(button_ps_task, "button_ps", 2048, NULL, 3, NULL);
    ESP_LOGI(TAG, "Tarea de gestión de botón Power Save iniciada en nodo hijo.");
}

uint32_t peripherals_get_power_mw(void)
{
    uint32_t power;
    xSemaphoreTake(s_periph_mutex, portMAX_DELAY);
    power = s_power_mw;
    xSemaphoreGive(s_periph_mutex);
    return power;
}

void peripherals_get_i2c_raw(char *dest, size_t max_len)
{
    if (!dest || max_len == 0) return;
    xSemaphoreTake(s_periph_mutex, portMAX_DELAY);
    strncpy(dest, s_i2c_hex, max_len - 1);
    dest[max_len - 1] = '\0';
    xSemaphoreGive(s_periph_mutex);
}