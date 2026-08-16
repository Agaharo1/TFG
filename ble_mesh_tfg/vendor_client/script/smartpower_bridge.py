

import json
import socket
import time
from datetime import datetime, timezone

import paho.mqtt.client as mqtt


SMARTPOWER_HOST = "192.168.4.1"   
SMARTPOWER_PORT = 23              

MQTT_HOST = "192.168.1.35"        
MQTT_PORT = 1883
MQTT_TOPIC = "iot/power/gateway"  


SAMPLES_PER_PUBLISH = 10


RECONNECT_DELAY_SEC = 3


def parse_line(line: str):

    parts = line.strip().split(",")
    if len(parts) != 4:
        return None
    try:
        return {
            "volts":     float(parts[0]),
            "amps":      float(parts[1]),
            "watts":     float(parts[2]),
            "watt_hour": float(parts[3]),
        }
    except ValueError:
        return None


def aggregate(samples: list) -> dict:
  
    n = len(samples)
    return {
        "volts_avg":     sum(s["volts"]     for s in samples) / n,
        "amps_avg":      sum(s["amps"]      for s in samples) / n,
        "watts_avg":     sum(s["watts"]     for s in samples) / n,
        "watts_min":     min(s["watts"]     for s in samples),
        "watts_max":     max(s["watts"]     for s in samples),
        "watt_hour":     samples[-1]["watt_hour"],
        "samples_agg":   n,
        "timestamp_utc": datetime.now(timezone.utc).isoformat(),
    }


def main():
    print(f"Conectando a MQTT {MQTT_HOST}:{MQTT_PORT}...")
    mqtt_client = mqtt.Client()
    mqtt_client.connect(MQTT_HOST, MQTT_PORT, keepalive=30)
    mqtt_client.loop_start()

    while True:
        try:
            print(f"Conectando a SmartPower2 telnet {SMARTPOWER_HOST}:{SMARTPOWER_PORT}...")
            with socket.create_connection((SMARTPOWER_HOST, SMARTPOWER_PORT), timeout=10) as sock:
                sock.settimeout(5.0)
                buffer = ""
                pending = []

                print(f"Leyendo muestras (publica cada {SAMPLES_PER_PUBLISH} muestras en '{MQTT_TOPIC}')...")
                while True:
                    chunk = sock.recv(4096)
                    if not chunk:
                        raise ConnectionError("Telnet cerrado por el otro extremo")

                    buffer += chunk.decode("ascii", errors="ignore")
                    while "\n" in buffer:
                        line, buffer = buffer.split("\n", 1)
                        sample = parse_line(line)
                        if sample is None:
                            continue
                        pending.append(sample)

                        if len(pending) >= SAMPLES_PER_PUBLISH:
                            payload = aggregate(pending)
                            mqtt_client.publish(MQTT_TOPIC, json.dumps(payload), qos=0)
                            print(f"  publicado: watts_avg={payload['watts_avg']:.3f} "
                                  f"volts_avg={payload['volts_avg']:.3f} "
                                  f"amps_avg={payload['amps_avg']:.3f}")
                            pending.clear()

        except (socket.timeout, ConnectionError, OSError) as e:
            print(f"[WARN] Conexion perdida ({e}). Reintentando en {RECONNECT_DELAY_SEC}s...")
            time.sleep(RECONNECT_DELAY_SEC)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nInterrumpido por el usuario.")
