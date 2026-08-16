

import os
import pandas as pd
import matplotlib.pyplot as plt
from influxdb import InfluxDBClient, DataFrameClient

INFLUX_HOST = '192.168.1.35'
INFLUX_PORT = 8086
DB_NAME = 'sensores'


GROUP_BY_INTERVAL = '15s'
OUTPUT_DIR = 'resultados_tfg'


campos_ping = {
    'received':             'Pings recibidos',
    'expected':             'Pings esperados',
    'loss_pct':             'Porcentaje (%)',
    'rssi_min_dbm':         'Decibelios (dBm)',
    'rssi_avg_dbm':         'Decibelios (dBm)',
    'rssi_max_dbm':         'Decibelios (dBm)',
    'snr_estimate_avg_db':  'Decibelios (dB) - ESTIMADO, no medido',
}


campos_transfer = {
    'total_bytes_observed': 'Bytes',
    'total_chunks':         'Numero de chunks',
    'received_chunks':      'Chunks recibidos',
    'crc_fail_count':       'Chunks con CRC invalido',
    'loss_pct':             'Porcentaje (%)',
    'rssi_min_dbm':         'Decibelios (dBm)',
    'rssi_avg_dbm':         'Decibelios (dBm)',
    'rssi_max_dbm':         'Decibelios (dBm)',
    'snr_estimate_avg_db':  'Decibelios (dB) - ESTIMADO, no medido',
}


campos_power = {
    'volts_avg': 'Voltaje Medio (V)',
    'amps_avg':  'Corriente Media (A)',
    'watts_avg': 'Potencia Media (W)',
    'watt_hour': 'Energia Acumulada (Wh)'
}

print(f"Conectando a InfluxDB en {INFLUX_HOST}...")
meta_client = InfluxDBClient(host=INFLUX_HOST, port=INFLUX_PORT, database=DB_NAME)
df_client = DataFrameClient(host=INFLUX_HOST, port=INFLUX_PORT, database=DB_NAME)


def obtener_nodos():
    """Descubre automaticamente que direcciones node_addr hay en los datos."""
    resultado = meta_client.query('SHOW TAG VALUES FROM "mqtt_consumer" WITH KEY = "node_addr"')
    puntos = list(resultado.get_points())
    nodos = sorted(set(p['value'] for p in puntos))
    return nodos


def generar_grafica(query, nombre_metrica, unidad, carpeta_destino, titulo_extra="", sufijo_archivo=""):
    try:
        resultados = df_client.query(query)

        if 'mqtt_consumer' not in resultados:
            print(f"  [AVISO] Sin datos para {nombre_metrica} en {carpeta_destino}.")
            return

        df = resultados['mqtt_consumer']

        if df.empty or df[nombre_metrica].isna().all():
            print(f"  [AVISO] Sin datos para {nombre_metrica} en {carpeta_destino}.")
            return

        minutos_transcurridos = (df.index - df.index[0]).total_seconds() / 60.0

        plt.figure(figsize=(10, 5))
        plt.plot(minutos_transcurridos, df[nombre_metrica], color='#32CD32', linewidth=1.5)

        titulo = f'Evolucion de {nombre_metrica.upper()}'
        if titulo_extra:
            titulo += f'\n({titulo_extra})'

        plt.title(titulo.replace('\n', ' '), fontsize=14, fontweight='bold')
        plt.xlabel('Tiempo (minutos)', fontsize=11, fontweight='bold')
        plt.ylabel(f'{nombre_metrica}\n({unidad})', fontsize=11, fontweight='bold')

        plt.grid(True, linestyle='--', alpha=0.6)
        plt.xlim(left=0)
        plt.tight_layout()

        nombre_archivo = f"{nombre_metrica}{sufijo_archivo}"
        ruta_archivo = f'{carpeta_destino}/{nombre_archivo}.png'

        plt.savefig(ruta_archivo, dpi=300)
        plt.close()
        print(f"  [OK] Guardada en: {ruta_archivo}")

    except Exception as e:
        print(f"  [ERROR] Fallo al procesar {nombre_metrica} en {carpeta_destino}: {e}")


if not os.path.exists(OUTPUT_DIR):
    os.makedirs(OUTPUT_DIR)


nodos = obtener_nodos()

if nodos:
    print(f"\nNodos detectados: {nodos}")
    

    for nodo in nodos:
        carpeta_base = f'{OUTPUT_DIR}/node_{nodo.replace("0x", "")}'
        for subcarpeta in ('ping', 'transferencia'):
            ruta = f'{carpeta_base}/{subcarpeta}'
            if not os.path.exists(ruta):
                os.makedirs(ruta)

    print("\n--- GENERANDO METRICAS DE PING POR NODO ---")
    for nodo in nodos:
        carpeta_destino = f'{OUTPUT_DIR}/node_{nodo.replace("0x", "")}/ping'
        print(f"\nNodo {nodo}:")
        for metrica, unidad in campos_ping.items():
            query = (
                f'SELECT mean("{metrica}") AS "{metrica}" FROM "mqtt_consumer" '
                f'WHERE ("node_addr"::tag = \'{nodo}\') AND ("topic"::tag =~ /ping$/) '
                f'GROUP BY time({GROUP_BY_INTERVAL}) fill(linear)'
            )
            generar_grafica(query, metrica, unidad, carpeta_destino, titulo_extra=f"Ping -- nodo {nodo}")

    print("\n--- GENERANDO METRICAS DE TRANSFERENCIA POR NODO ---")
    for nodo in nodos:
        carpeta_destino = f'{OUTPUT_DIR}/node_{nodo.replace("0x", "")}/transferencia'
        print(f"\nNodo {nodo}:")
        for metrica, unidad in campos_transfer.items():
            query = (
                f'SELECT mean("{metrica}") AS "{metrica}" FROM "mqtt_consumer" '
                f'WHERE ("node_addr"::tag = \'{nodo}\') AND ("topic"::tag =~ /transfer_/) '
                f'GROUP BY time({GROUP_BY_INTERVAL}) fill(linear)'
            )
            generar_grafica(query, metrica, unidad, carpeta_destino, titulo_extra=f"Transferencia -- nodo {nodo}")
else:
    print("\n[AVISO] No se encontraron datos de nodos BLE Mesh ('node_addr') en InfluxDB.")


print("\n--- GENERANDO METRICAS DE CONSUMO (POWER) ---")
carpeta_power = f'{OUTPUT_DIR}/power_gateway'
if not os.path.exists(carpeta_power):
    os.makedirs(carpeta_power)

for metrica, unidad in campos_power.items():
    
    query = (
        f'SELECT mean("{metrica}") AS "{metrica}" FROM "mqtt_consumer" '
        f'WHERE ("topic"::tag = \'iot/power/gateway\') '
        f'GROUP BY time({GROUP_BY_INTERVAL}) fill(linear)'
    )
    generar_grafica(query, metrica, unidad, carpeta_power, titulo_extra="Consumo Gateway")

print("\nListo. Graficas guardadas en la carpeta 'resultados_tfg/'.")