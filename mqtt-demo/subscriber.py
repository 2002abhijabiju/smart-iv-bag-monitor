"""Print IV bag status messages published by the device."""
import json
import sys

import paho.mqtt.client as mqtt

BROKER = "localhost"
PORT = 1883
PATIENT_ID = sys.argv[1] if len(sys.argv) > 1 else "patient001"
TOPIC = f"public/ivbag/{PATIENT_ID}"


def on_connect(client, userdata, flags, reason_code, properties=None):
    print(f"Connected ({reason_code}), subscribing to {TOPIC}")
    client.subscribe(TOPIC)


def on_message(client, userdata, msg):
    data = json.loads(msg.payload)
    t = data.get("timeLeft", {})
    print(f"level={data['level']}%  rate={data['rate']} mL/hr  "
          f"time left={t.get('hour', 0)}h {t.get('minute', 0)}m")


client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
client.on_connect = on_connect
client.on_message = on_message
client.connect(BROKER, PORT)
client.loop_forever()
