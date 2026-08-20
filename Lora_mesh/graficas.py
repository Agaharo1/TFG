import os
import matplotlib.pyplot as plt
from influxdb import InfluxDBClient, DataFrameClient

# --- CONFIGURACIÓN DE LA BASE DE DATOS ---
INFLUX_HOST = '192.168.1.35' 
INFLUX_PORT = 8086
DB_NAME = 'sensores'

GROUP_BY_INTERVAL = '15s'
OUTPUT_DIR = 'resultados_tfg_lora'


campos_rendimiento = {
    'latencia':   'Segundos (s)',
    'throughput': 'Bytes por segundo (B/s)'
}

print(f"Conectando a InfluxDB en {INFLUX_HOST}...")
meta_client = InfluxDBClient(host=INFLUX_HOST, port=INFLUX_PORT, database=DB_NAME)
df_client = DataFrameClient(host=INFLUX_HOST, port=INFLUX_PORT, database=DB_NAME)

def obtener_nodos_lora():
    """Descubre automáticamente qué direcciones 'mac' (Node IDs) hay en los datos."""
    resultado = meta_client.query('SHOW TAG VALUES FROM "mqtt_consumer" WITH KEY = "mac"')
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
            print(f"  [AVISO] Datos vacíos para {nombre_metrica} en {carpeta_destino}.")
            return

        minutos_transcurridos = (df.index - df.index[0]).total_seconds() / 60.0

        plt.figure(figsize=(10, 5))
        # Usamos un color distinto para diferenciar del BLE (ej. Azul LoRa)
        plt.plot(minutos_transcurridos, df[nombre_metrica], color='#1E90FF', linewidth=1.5, marker='o', markersize=4)

        titulo = f'Evolución de {nombre_metrica.upper()}'
        if titulo_extra:
            titulo += f'\n({titulo_extra})'

        plt.title(titulo.replace('\n', ' '), fontsize=14, fontweight='bold')
        plt.xlabel('Tiempo (minutos)', fontsize=11, fontweight='bold')
        plt.ylabel(f'{nombre_metrica}\n({unidad})', fontsize=11, fontweight='bold')

        plt.grid(True, linestyle='--', alpha=0.6)
        plt.xlim(left=0)
        
    
        plt.ylim(bottom=0) 
        
        plt.tight_layout()

        nombre_archivo = f"{nombre_metrica}{sufijo_archivo}"
        ruta_archivo = f'{carpeta_destino}/{nombre_archivo}.png'

        plt.savefig(ruta_archivo, dpi=300)
        plt.close()
        print(f"  [OK] Guardada en: {ruta_archivo}")

    except Exception as e:
        print(f"  [ERROR] Fallo al procesar {nombre_metrica} en {carpeta_destino}: {e}")

# Crear directorio principal
if not os.path.exists(OUTPUT_DIR):
    os.makedirs(OUTPUT_DIR)

nodos = obtener_nodos_lora()

if nodos:
    print(f"\nNodos LoRa detectados: {nodos}")
    
    # Crear subcarpetas por nodo
    for nodo in nodos:
       
        nodo_limpio = nodo.replace('!', '')
        carpeta_nodo = f'{OUTPUT_DIR}/nodo_{nodo_limpio}'
        if not os.path.exists(carpeta_nodo):
            os.makedirs(carpeta_nodo)

    print("\n--- GENERANDO GRÁFICAS DE RENDIMIENTO POR NODO ---")
    for nodo in nodos:
        nodo_limpio = nodo.replace('!', '')
        carpeta_destino = f'{OUTPUT_DIR}/nodo_{nodo_limpio}'
        print(f"\nProcesando Nodo {nodo}:")
        
        for metrica, unidad in campos_rendimiento.items():
            # Construimos la query. Note que ahora usamos "mac" en lugar de "node_addr"
            query = (
                f'SELECT mean("{metrica}") AS "{metrica}" FROM "mqtt_consumer" '
                f'WHERE ("mac"::tag = \'{nodo}\') '
                f'GROUP BY time({GROUP_BY_INTERVAL}) fill(linear)'
            )
            generar_grafica(query, metrica, unidad, carpeta_destino, titulo_extra=f"LoRa Mesh -- Nodo {nodo}")

else:
    print("\n[AVISO] No se encontraron datos de nodos LoRa ('mac') en InfluxDB.")

print("\n🏁 Listo. Gráficas generadas en la carpeta 'resultados_tfg_lora/'.")