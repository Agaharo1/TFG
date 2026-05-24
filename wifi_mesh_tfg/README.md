# TFG — WiFi Mesh IoT con ESP-IDF y MQTT

## Arquitectura del sistema

```
┌──────────────────────────────────────────────────────────────────┐
│                       RED ESP-MESH                               │
│                                                                  │
│  ┌──────────┐  métricas   ┌──────────┐  métricas  ┌──────────┐ │
│  │  Nodo 2  │ ──────────► │  Nodo 1  │ ──────────► │  Root    │ │
│  │ (hoja)   │  (mesh)     │ (inter.) │  (mesh)     │ (Nodo 0) │ │
│  └──────────┘             └──────────┘             └────┬─────┘ │
│                                                         │        │
└─────────────────────────────────────────────────────────│────────┘
                                                          │ WiFi (STA)
                                                          ▼
                                                    ┌──────────┐
                                                    │  Router  │
                                                    └────┬─────┘
                                                         │
                                                    ┌────▼─────────────────┐
                                                    │   Ubuntu Server      │
                                                    │  ┌────────────────┐  │
                                                    │  │  Mosquitto     │  │
                                                    │  │  (MQTT :1883)  │  │
                                                    │  └───────┬────────┘  │
                                                    │          │            │
                                                    │  ┌───────▼────────┐  │
                                                    │  │   Telegraf     │  │
                                                    │  │ (MQTT→Influx)  │  │
                                                    │  └───────┬────────┘  │
                                                    │          │            │
                                                    │  ┌───────▼────────┐  │
                                                    │  │   InfluxDB v2  │  │
                                                    │  └───────┬────────┘  │
                                                    │          │            │
                                                    │  ┌───────▼────────┐  │
                                                    │  │    Grafana     │  │
                                                    │  │   :3000        │  │
                                                    │  └────────────────┘  │
                                                    └──────────────────────┘
```

## Topics MQTT publicados por el root

| Topic | Contenido | QoS |
|-------|-----------|-----|
| `iot/mesh/node/<MAC>/metrics` | JSON con todas las métricas del nodo | 0 |
| `iot/mesh/topology` | Lista de MACs en la malla | 0 |
| `iot/mesh/status` | `{"root":"online"}` / `{"root":"offline"}` (LWT) | 1 |

### Ejemplo de payload de métricas

```json
{
  "seq": 42,
  "mac": "a4:cf:12:34:56:78",
  "rssi_parent": -62,
  "rssi_router": -55,
  "snr_estimate": 33,
  "layer": 2,
  "hops_to_root": 2,
  "connected_subs": 0,
  "tx_count": 42,
  "rx_count": 38,
  "tx_fail": 1,
  "pdr_pct": 47.5,
  "latency_ms": 34,
  "free_heap": 187432,
  "uptime_s": 210
}
```

---

## 1. Configurar el firmware (ESP-IDF)


### Pasos

```bash
# 1. Crea tu settings.h CONFIG_MESH_ROUTER_SSID y CONFIG_MESH_ROUTER_PASS
# 1. Clonar / copiar el proyecto
cd wifi_mesh_tfg

# 2. Ajustar credenciales en sdkconfig.defaults
#    Editar los campos marcados con <--
nano sdkconfig.defaults

# 3. Configurar (opcional, para ajustes adicionales)
idf.py menuconfig

# 4. Compilar
idf.py build

# 5. Flashear los tres nodos (repetir para cada uno con su puerto)
idf.py -p /dev/ttyUSB0 flash monitor
```

> **Nota**: Los tres nodos se flashean con **el mismo binario**. El rol de root
> se negocia automáticamente mediante el algoritmo de elección de ESP-MESH
> (el que tiene mejor conexión al router gana).

---

## 2. Poner en marcha el servidor 

### Prerrequisitos
```bash
sudo apt install docker.io docker-compose-plugin
```

### Pasos
```bash
cd server/

# Crear directorios necesarios
mkdir -p mosquitto/{data,log} telegraf grafana/provisioning

# Arrancar todo 
docker-compose up -d

# Verificar que los contenedores están healthy
docker-compose ps
```

### Verificar que llegan mensajes MQTT

```bash
# Suscribirse a todos los topics del TFG
mosquitto_sub -h localhost -t "iot/mesh/#" -v
```

### Acceder a Grafana
Abrir `http://<IP_UBUNTU>:3000` → usuario `admin` / contraseña `grafana_pass`

**Configurar datasource InfluxDB:**
- URL: `http://influxdb:8086`
- Organization: `tfg`
- Token: `tfg-super-secret-token`
- Default bucket: `mesh_metrics`

---

## 3. Métricas recogidas

| Métrica | Descripción | Unidad |
|---------|-------------|--------|
| `rssi_parent` | Potencia de señal al nodo padre | dBm |
| `rssi_router` | Potencia de señal al router (solo root) | dBm |
| `snr_estimate` | SNR estimado (RSSI − noise floor empírico) | dB |
| `layer` | Capa en el árbol mesh | — |
| `hops_to_root` | Número de saltos hasta el root | — |
| `connected_subs` | Nodos descendientes conocidos | — |
| `tx_count` | Paquetes transmitidos (acumulado) | pkts |
| `rx_count` | Paquetes recibidos (acumulado) | pkts |
| `tx_fail` | Fallos de transmisión (acumulado) | pkts |
| `pdr_pct` | Packet Delivery Ratio | % |
| `latency_ms` | RTT último ping-pong medido | ms |
| `free_heap` | Memoria heap libre | bytes |
| `uptime_s` | Tiempo activo desde reset | s |

---

## 4. Protocolo de mensajes internos mesh

```
┌──────────────────────────────────────────────────────────┐
│  mesh_hdr_t (14 bytes)                                   │
│  version(1) | type(1) | src_mac(6) | seq(4) | ts_ms(4)  │
├──────────────────────────────────────────────────────────┤
│  payload (variable según type)                           │
│  MSG_METRICS → metrics_payload_t (30 bytes)              │
│  MSG_PING    → ping_payload_t    (12 bytes)              │
│  MSG_PONG    → ping_payload_t    (12 bytes)              │
└──────────────────────────────────────────────────────────┘
```

---

## 5. Estructura del proyecto

```
wifi_mesh_tfg/
├── CMakeLists.txt
├── sdkconfig.defaults          ← Ajusta SSID, pass y broker aquí
├── main/
│   ├── CMakeLists.txt
│   ├── main.c                  ← Punto de entrada (app_main)
│   ├── protocol.h              ← Definición del protocolo binario
│   ├── mesh_handler.{h,c}      ← Gestión de ESP-MESH + tareas TX/RX/ping
│   ├── mqtt_handler.{h,c}      ← Cliente MQTT (solo root)
│   └── metrics.{h,c}           ← Recolección y contadores de métricas
└── server/
    ├── docker-compose.yml      ← Stack completo del servidor
    ├── mosquitto/config/
    │   └── mosquitto.conf
    └── telegraf/
        └── telegraf.conf       ← Bridge MQTT → InfluxDB
```


