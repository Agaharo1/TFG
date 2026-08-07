import pandas as pd
import matplotlib.pyplot as plt
from influxdb import DataFrameClient
import os


INFLUX_HOST = '192.168.1.37'
INFLUX_PORT = 8086
DB_NAME = 'sensores'
MAC = '5c:01:3b:67:14:24'


metricas_generales = {
    'latency_ms': 'Milisegundos (ms)',
    'rssi_parent': 'Decibelios (dBm)',
    'rssi_router': 'Decibelios (dBm)',
    'snr_estimate': 'Decibelios (dB)',
    'layer': 'Nivel de profundidad',
    'hops_to_root': 'Número de saltos',
    'connected_subs': 'Nodos hijos conectados',
    'tx_count': 'Total de paquetes',
    'rx_count': 'Total de paquetes',
    'tx_fail': 'Paquetes fallidos',
    'pdr_pct': 'Porcentaje (%)',
    'ping_lost_count': 'Pings perdidos',
    'free_heap': 'Bytes libres',
    'uptime_s': 'Segundos (s)',
    'power_json': 'Milivatios (mW)'
}

metricas_separadas = {
    'duration_ms': 'Milisegundos (ms)',
    'power_idle_mw': 'Milivatios (mW)',
    'power_active_mw': 'Milivatios (mW)'
}

tamanos_payload = [1, 10, 100] # Tamaños en KB


ps_modes = [0, 1, 2] 
etiquetas_psmode = {
    0: "Max Rendimiento",
    1: "Ahorro Activado",
    2: "Modo 2" 
}

print(f"Conectando a InfluxDB en {INFLUX_HOST}...")
client = DataFrameClient(host=INFLUX_HOST, port=INFLUX_PORT, database=DB_NAME)


if not os.path.exists('resultados_tfg'):
    os.makedirs('resultados_tfg')

for mode in ps_modes:
    carpeta_modo = f'resultados_tfg/ps_mode_{mode}'
    if not os.path.exists(carpeta_modo):
        os.makedirs(carpeta_modo)


def generar_grafica(query, nombre_metrica, unidad, carpeta_destino, titulo_extra=""):
    try:
        resultados = client.query(query)
        
        if 'mqtt_consumer' in resultados:
            df = resultados['mqtt_consumer']
            
            if df.empty or df[nombre_metrica].isna().all():
                print(f" [AVISO] Sin datos para {nombre_metrica} en {carpeta_destino}.")
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
            
         
            nombre_archivo = titulo.replace('\n', ' ').replace('(', '').replace(')', '').replace('|', '').replace(':', '').replace(' - ', '_').replace(' ', '_').lower()
            
     
            ruta_archivo = f'{carpeta_destino}/{nombre_archivo}.png'
            
            plt.savefig(ruta_archivo, dpi=300)
            plt.close()
            print(f" [OK] Guardada en: {ruta_archivo}")
        else:
            print(f" [AVISO] No se encontraron datos para {nombre_metrica} en {carpeta_destino}.")
            
    except Exception as e:
        print(f" [ERROR] Fallo al procesar {nombre_metrica} en {carpeta_destino}: {e}")


print("\n--- GENERANDO MÉTRICAS GENERALES POR PS_MODE ---")
for metrica, unidad in metricas_generales.items():
    for mode in ps_modes:
        etiqueta_modo = etiquetas_psmode[mode]
        carpeta_destino = f'resultados_tfg/ps_mode_{mode}'
        
        query = f"SELECT mean(\"{metrica}\") AS \"{metrica}\" FROM \"mqtt_consumer\" WHERE (\"mac\"::tag = '{MAC}') AND \"ps_mode\" = {mode} GROUP BY time(10s) fill(linear)"
        generar_grafica(query, metrica, unidad, carpeta_destino, titulo_extra=f"PS_MODE: {mode} - {etiqueta_modo}")


print("\n--- GENERANDO MÉTRICAS CRUZADAS (PAYLOAD + PS_MODE) ---")
for metrica, unidad in metricas_separadas.items():
    for kb in tamanos_payload:
        for mode in ps_modes:
            etiqueta_modo = etiquetas_psmode[mode]
            carpeta_destino = f'resultados_tfg/ps_mode_{mode}'
            
            query = f"SELECT mean(\"{metrica}\") AS \"{metrica}\" FROM \"mqtt_consumer\" WHERE (\"mac\"::tag = '{MAC}') AND \"payload_kb\" = {kb} AND \"ps_mode\" = {mode} GROUP BY time(10s) fill(linear)"
            
            titulo_ext = f"Payload {kb}KB | PS_MODE {mode} ({etiqueta_modo})"
            
            generar_grafica(query, metrica, unidad, carpeta_destino, titulo_extra=titulo_ext)

