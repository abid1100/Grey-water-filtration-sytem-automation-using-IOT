import json
from flask import Flask, request, jsonify
from flask_cors import CORS
from pymongo import MongoClient
import paho.mqtt.client as mqtt
from datetime import datetime, timezone
from pymongo.errors import PyMongoError
import secrets
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
# Command authentication: no default password or key.
# If unset, the command endpoint remains disabled.
COMMAND_API_KEY = os.getenv("COMMAND_API_KEY", "")

# Must match DEVICE_ID in esp32.ino.
ALLOWED_DEVICE_ID = "esp32-hydro-01"

# Must match the firmware's current duration ceiling.
MAX_INJECT_SECONDS = 5

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

    # Default to 100 readings; reject invalid limits.
    try:
        limit = int(request.args.get("limit", "100"))
    except ValueError:
        return jsonify({
            "error": "limit must be an integer from 1 to 500"
        }), 400

    if not 1 <= limit <= 500:
        return jsonify({
            "error": "limit must be an integer from 1 to 500"
        }), 400

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
    # 1. Require a configured API key.
    if not COMMAND_API_KEY:
        return jsonify({
            "error": "Command API is disabled until COMMAND_API_KEY is configured"
        }), 503

    supplied_key = request.headers.get("X-API-Key", "")

    if not secrets.compare_digest(
        supplied_key.encode("utf-8"),
        COMMAND_API_KEY.encode("utf-8")
    ):
        return jsonify({"error": "Unauthorized"}), 401

    # 2. Require a valid JSON object.
    payload = request.get_json(silent=True)

    if not isinstance(payload, dict):
        return jsonify({
            "error": "Body must be a valid JSON object"
        }), 400

    if set(payload) - {"device_id", "cmd", "action", "duration"}:
        return jsonify({"error": "Unknown command fields"}), 400

    # 3. Validate the target and supported command.
    device_id = payload.get("device_id")

    if device_id != ALLOWED_DEVICE_ID:
        return jsonify({"error": "Unknown device_id"}), 400

    if payload.get("cmd") != "injector":
        return jsonify({"error": "cmd must be injector"}), 400

    action = payload.get("action")

    if action not in ("on", "off"):
        return jsonify({"error": "action must be on or off"}), 400

    # 4. ON requires whole seconds; OFF must omit duration.
    if action == "on":
        duration = payload.get("duration")

        if type(duration) is not int or not 1 <= duration <= MAX_INJECT_SECONDS:
            return jsonify({
                "error": "duration must be an integer from 1 to 5 seconds"
            }), 400

    elif "duration" in payload:
        return jsonify({
            "error": "Omit duration for off commands"
        }), 400

    # 5. Construct the firmware message from validated fields.
    topic = f"hydro/{device_id}/commands"

    mqtt_payload = {
        "cmd": "injector",
        "action": action
    }

    if action == "on":
        mqtt_payload["duration"] = duration

    if not mqttc.is_connected():
        return jsonify({
            "error": "MQTT is disconnected",
            "status": "not_published"
        }), 503

    # 6. Check the MQTT client's publish result.
    # Do not retain actuator commands.
    try:
        result = mqttc.publish(
            topic,
            json.dumps(mqtt_payload),
            qos=0,
            retain=False
        )
    except (OSError, ValueError, RuntimeError):
        flaskapp.logger.error("MQTT publish raised an exception")

        return jsonify({
            "error": "Publish outcome is unknown; do not automatically retry",
            "status": "publish_outcome_unknown"
        }), 503

    if result.rc != mqtt.MQTT_ERR_SUCCESS:
        return jsonify({
            "error": "MQTT publish reported failure; delivery is unconfirmed",
            "status": "publish_failed"
        }), 503

    # 7. Log acceptance, not successful physical execution.
    event = {
        "device_id": device_id,
        "command": mqtt_payload,
        "user": "api_key_client",
        "status": "accepted_for_publish",
        "ts": datetime.now(timezone.utc)
    }

    event_logged = True

    try:
        events.insert_one(event)
    except PyMongoError:
        event_logged = False
        flaskapp.logger.error(
            "Command accepted for publish but event logging failed"
        )

    # A database failure must not imply the command was never sent.
    return jsonify({
        "status": "accepted_for_publish",
        "topic": topic,
        "payload": mqtt_payload,
        "event_logged": event_logged,
        "message": "Device execution is not confirmed. Do not automatically retry."
    }), 202
    
if __name__ == "__main__":
    flaskapp.run(host="0.0.0.0", port=5000, debug=False)

