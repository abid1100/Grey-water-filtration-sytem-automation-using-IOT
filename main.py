import subprocess
import time
from pymongo import MongoClient

SENSOR_SCRIPT = "sensor_update.py"  # Name of your sensor update file
IMAGE_SCRIPT = "image_update.py"   # Name of your image update file
MONGO_URI = "mongodb+srv://AdminClint0001:uTYZ4fPph7whTpXC@cluster0.eifjnhd.mongodb.net/"
DB_NAME = "Sensor"
COLLECTION_NAME = "Data"


def mark_sensor_offline():
    """
    Connect to the MongoDB and update the sensor status to 'Offline'.
    """
    try:
        client = MongoClient(MONGO_URI)
        db = client[DB_NAME]
        collection = db[COLLECTION_NAME]
        # Update any document as appropriate – here we update the first one we find
        collection.update_one({}, {"$set": {"Sensor": "Offline"}}, upsert=True)
        print("MongoDB: Sensor marked Offline.")
    except Exception as e:
        print("Error marking sensor offline in MongoDB:", e)
    finally:
        client.close()


def run_sensor(duration=300):
    """
    Start the sensor update script and let it run for the specified duration (in seconds).
    After the duration expires, terminate the script.
    """
    print("Starting sensor update...")
    sensor_proc = subprocess.Popen(["python", SENSOR_SCRIPT])
    try:
        time.sleep(duration)  # Let the sensor script run for 5 minutes (300 seconds)
    except KeyboardInterrupt:
        print("Interrupted during sensor runtime.")
    finally:
        # Terminate the sensor process gracefully.
        sensor_proc.terminate()
        try:
            sensor_proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            sensor_proc.kill()
        mark_sensor_offline()
        print("Sensor update stopped.")

def run_image():
    """
    Run the image update script (this is a one-time execution).
    """
    print("Running image update...")
    try:
        subprocess.run(["python", IMAGE_SCRIPT], check=True)
        print("Image update completed.")
    except subprocess.CalledProcessError as e:
        print("Error executing image update:", e)

def main():
    while True:
        # Run sensor update for 5 minutes.
        run_sensor(duration=30)
        
        # Run image update one time.
        run_image()
        
        # Wait 10 minutes before repeating the cycle.
        print("Waiting 10 minutes before the next cycle...")
        time.sleep(600)

if __name__ == "__main__":
    main()
