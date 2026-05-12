# Proyecto TFG: Sistema de Monitorización IoT con ESP32, MQTT e InfluxDB

Este documento detalla los pasos seguidos para la configuración, persistencia y visualización de datos de sensores utilizando Docker.

## 🚀 Arquitectura del Sistema
**ESP32** -> **Mosquitto (MQTT)** -> **Telegraf** -> **InfluxDB** -> **Grafana**

---

## 🛠️ Comandos de Docker Compose (Uso frecuente)

Ejecuta estos comandos dentro de la carpeta `~/Documents/tfg/`:

* **Iniciar todo el sistema:**
    ```bash
    sudo docker-compose up -d
    ```
* **Detener y borrar los contenedores:**
    ```bash
    sudo docker-compose down
    ```
* **Reiniciar los servicios (aplicar cambios):**
    ```bash
    sudo docker-compose restart
    ```
* **Ver el estado de los contenedores:**
    ```bash
    sudo docker-compose ps
    ```
* **Ver logs de un servicio específico (ej. Telegraf):**
    ```bash
    sudo docker-compose logs -f telegraf
    ```

---

## 📊 Paso 4: Visualización en Grafana

Para que las gráficas funcionen y no se pierdan, sigue este procedimiento:

1.  **Acceso:** Entra en `http://localhost:3000` (o la IP de tu servidor).
2.  **Configurar Data Source:** * Ve a *Connections* > *Data Sources* > *Add data source*.
    * Selecciona **InfluxDB**.
    * URL: `http://influxdb:8086`.
    * Database: `sensores`.
    * Pulsa **Save & Test**.
3.  **Configurar el Panel de Temperatura:**
    * **FROM:** `default` `mqtt_consumer`.
    * **WHERE:** Haz clic en `+` > `topic` > `=` > `casa/temperatura`.
    * **SELECT:** `field(value)` `mean()`.
    * **FORMAT AS:** `Time series`.
4.  **Ajuste de Tiempo:** Cambia el selector superior derecho de "Last 6 hours" a **"Last 5 minutes"** o **"Last 30 minutes"**.
5.  **Persistencia del Dashboard:** Haz clic en el icono del **disquete (Save)** arriba a la derecha y asígnale un nombre (ej. "Monitorización TFG").

---

## 📂 Paso 5: Gestión y Almacenamiento de Datos

Los datos se guardan físicamente en tu máquina para que no se borren al apagar el PC.

1.  **Ubicación de los datos:**
    * Ruta: `/home/agaharo/Documents/tfg/influx_data/`
    * Los archivos reales son de tipo `.tsm` y están dentro de la subcarpeta `data/`.
2.  **Permisos de lectura:**
    * Como Docker usa el usuario root, para ver las carpetas usa: `sudo ls -R ~/Documents/tfg/influx_data/data`.
3.  **Exportación a CSV (Para la memoria del TFG):**
    * Usa este comando para generar un archivo Excel/CSV con todas las lecturas de temperatura:
    ```bash
    sudo docker exec -it influxdb influx -database 'sensores' -execute 'SELECT * FROM mqtt_consumer' -format csv > datos_exportados.csv
    ```

