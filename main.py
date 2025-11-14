
import json
from datetime import datetime
from pymongo import MongoClient
import paho.mqtt.client as mqtt

MQTT_BROKER = "localhost"
MQTT_TOPIC = "hydro/+/sensors"

MONGO_URI = "mongodb://localhost:27017/"
DB_NAME = "hydroponics"

mongo = MongoClient(MONGO_URI)
db = mongo[DB_NAME]
readings = db.readings

def on_connect(client, userdata, flags, rc):
    print("Connected to MQTT broker:", rc)
    client.subscribe(MQTT_TOPIC)

def on_message(client, userdata, msg):
    try:
        data = json.loads(msg.payload.decode())
        doc = {
            "device_id": data.get("device_id"),
            "sensors": data.get("sensors"),
            "received_at": datetime.utcnow(),
            "topic": msg.topic
        }
        readings.insert_one(doc)
        print("Inserted:", doc)
    except Exception as e:
        print("Error:", e)

client = mqtt.Client()
client.on_connect = on_connect
client.on_message = on_message

client.connect(MQTT_BROKER, 1883, 60)
client.loop_forever()
