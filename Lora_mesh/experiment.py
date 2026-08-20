import time
import json
import paho.mqtt.client as mqtt
from pubsub import pub
import meshtastic.serial_interface

DESTINATION_NODE = "!1a2b3c4d" 
PAYLOAD_SIZE = 200 
MAX_PACKETS = 50 


MQTT_BROKER = "192.168.1.35" 
MQTT_PORT = 1883
MQTT_TOPIC = f"iot/mesh/node/{DESTINATION_NODE.strip('!')}/metrics/performance"

start_time = 0
waiting_for_ack = False
packets_sent = 0


mqtt_client = mqtt.Client()

def on_receive(packet, interface):
    global start_time, waiting_for_ack
    
    if waiting_for_ack and packet.get('decoded', {}).get('portnum') == 'ROUTING_APP':
        end_time = time.time()
        
        latencia = (end_time - start_time) / 2
        throughput = PAYLOAD_SIZE / latencia
        
        print(f"✅ ¡ACK Recibido! Latencia: {latencia:.2f}s | Throughput: {throughput:.2f} B/s")
        
        mqtt_payload = {
            "mac": DESTINATION_NODE,
            "latencia": latencia,
            "throughput": throughput,
            "payload_size": PAYLOAD_SIZE
        }
        
 
        mqtt_client.publish(MQTT_TOPIC, json.dumps(mqtt_payload))
        print(f"☁️  Datos publicados en MQTT -> {MQTT_TOPIC}\n")
        
        waiting_for_ack = False

def main():
    global start_time, waiting_for_ack, packets_sent
    
    print("Conectando al broker MQTT local...")
    try:
        mqtt_client.connect(MQTT_BROKER, MQTT_PORT, 60)
        mqtt_client.loop_start()
    except Exception as e:
        print(f"❌ Error conectando a MQTT: {e}")
        return

    print("Conectando al Nodo Base por USB...")
    interface = meshtastic.serial_interface.SerialInterface()
    pub.subscribe(on_receive, "meshtastic.receive")
    
    payload_data = "X" * PAYLOAD_SIZE
    
    print(f"\nIniciando inyección de {MAX_PACKETS} paquetes hacia {DESTINATION_NODE}...\n")
    
    try:
        while packets_sent < MAX_PACKETS:
            if not waiting_for_ack:
                packets_sent += 1
                print(f"📦 Enviando paquete {packets_sent}/{MAX_PACKETS}...")
                start_time = time.time()
                waiting_for_ack = True
                
                interface.sendData(payload_data.encode('utf-8'), 
                                 destinationId=DESTINATION_NODE, 
                                 portNum=256,
                                 wantAck=True)
                
            time.sleep(15)
            
            if waiting_for_ack:
                print("❌ Tiempo de espera agotado (Packet Loss). Reintentando...\n")
                waiting_for_ack = False

    except KeyboardInterrupt:
        print("\nPrueba finalizada por el usuario.")
    
    print("\n🏁 Experimento completado.")
    interface.close()
    mqtt_client.loop_stop()
    mqtt_client.disconnect()

if __name__ == "__main__":
    main()