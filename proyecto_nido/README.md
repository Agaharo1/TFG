# Configuración del Proyecto ESP32-CAM

Este repositorio contiene el firmware para el dispositivo ESP32-CAM. Sigue estos pasos para configurar el entorno, las dependencias y las credenciales antes de compilar.

## 1. Instalación de Dependencias

El proyecto requiere el componente oficial de la cámara de Espressif. Para instalarlo, ejecuta los siguientes comandos en la raíz de tu proyecto:

```bash
mkdir components
cd components
git clone https://github.com/espressif/esp32-camera.git
cd ..
```
## 2.Crea tu wifi_settings.h

```bash
#define WIFI_SSID "Tu_Nombre_De_WiFi"
#define WIFI_PASS "Tu_Contraseña_De_WiFi"
#define SERVER_URL "http://<IP_DE_TU_SERVIDOR_UBUNTU>:5000/upload"
```

## 3.Compilación y Despliegue

```bash
idf.py fullclean
idf.py build flash monitor
```