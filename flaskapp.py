import json
from flask import Flask, request, jsonify
from flask_cors import CORS
from pymongo import MongoClient
import paho.mqtt.client as mqtt
from datetime import datetime
import os

flaskapp = Flask(__name__)
CORS(flaskapp)

flaskapp.config['SECRET_KEY'] = os.getenv('SECRET_KEY', 'defaultsecret')

@flaskapp.route('/')
def home():
    return f"Hello from Flask! (ENV={os.getenv('FLASK_ENV')})"

# Config
MQTT_BROKER = os.getenv("MQTT_BROKER", "localhost")
MQTT_PORT = int(os.getenv("MQTT_PORT", 1883))
MONGO_URI = os.getenv("MONGO_URI", "mongodb://localhost:27017/")
DB_NAME = "hydroponics"

mongo = MongoClient(MONGO_URI)
db = mongo[DB_NAME]
readings = db.readings
events = db.events

mqttc = mqtt.Client()
mqttc.connect(MQTT_BROKER, MQTT_PORT, 60)
mqttc.loop_start()

@flaskapp.route("/api/readings/latest", methods=["GET"])
def latest_readings():
    pipeline = [
        {"$sort": {"received_at": -1}},
        {"$group": {"_id": "$device_id", "doc": {"$first": "$$ROOT"}}},
        {"$replaceRoot": {"newRoot": "$doc"}}
    ]
    docs = list(readings.aggregate(pipeline))
    # convert ObjectIds and datetimes
    for d in docs:
        d["_id"] = str(d.get("_id"))
        if isinstance(d.get("received_at"), datetime):
            d["received_at"] = d["received_at"].isoformat()
    return jsonify(docs)

@flaskapp.route("/api/readings", methods=["GET"])
def get_readings():
    device_id = request.args.get("device_id")
    limit = int(request.args.get("limit", 100))
    q = {}
    if device_id:
        q["device_id"] = device_id
    cursor = readings.find(q).sort("received_at", -1).limit(limit)
    out = []
    for d in cursor:
        d["_id"] = str(d.get("_id"))
        if isinstance(d.get("received_at"), datetime):
            d["received_at"] = d["received_at"].isoformat()
        out.append(d)
    return jsonify(out)

@flaskapp.route("/api/command", methods=["POST"])
def send_command():
    payload = request.json
    # expected: { "device_id": "esp32-hydro-01", "cmd": "filter", "action": "on", "meta": {...} }
    device_id = payload.get("device_id")
    if not device_id:
        return jsonify({"error": "device_id required"}), 400
    topic = f"hydro/{device_id}/commands"
    mqtt_payload = {}
    mqtt_payload["cmd"] = payload.get("cmd")
    mqtt_payload["action"] = payload.get("action")
    if "duration" in payload:
        mqtt_payload["duration"] = payload["duration"]
    # publish
    mqttc.publish(topic, json.dumps(mqtt_payload))
    # log event
    event = {
        "device_id": device_id,
        "command": mqtt_payload,
        "user": payload.get("user", "api"),
        "ts": datetime.utcnow()
    }
    events.insert_one(event)
    return jsonify({"status": "ok", "topic": topic, "payload": mqtt_payload})

if __name__ == "__main__":
    flaskapp.run(host="0.0.0.0", port=5000, debug=False)

