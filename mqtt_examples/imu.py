import subprocess
import json
import os
from pathlib import Path

EXAMPLES_DIR = Path(__file__).resolve().parent
PATH_EXAMPLE = Path(os.getenv("COBIEN_EXAMPLE_IMU_PATH", EXAMPLES_DIR / "imu_changes.json"))
MQTT_HOST = os.getenv("COBIEN_MQTT_HOST", "localhost")

def main():
    # Charger ton JSON
    with PATH_EXAMPLE.open("r", encoding="utf-8") as f:
        data = json.load(f)

    # Modifier une valeur
    data["time"]["config"]["data"]["IMMOBILE_TIME_MS"] = 5000
    data["threshold"]["config"]["data"]["MOTION_THRESHOLD"] = 1

    # Lancer mosquitto_pub
    subprocess.run([
        "mosquitto_pub",
        "-h", MQTT_HOST,
        "-t", "time/config",
        "-m", json.dumps(data["time"]["config"]["data"])
    ])

    # Publier MOTION_THRESHOLD sur threshold/config
    subprocess.run([
        "mosquitto_pub",
        "-h", MQTT_HOST,
        "-t", "threshold/config",
        "-m", json.dumps(data["threshold"]["config"]["data"])
    ])


if __name__ == "__main__":
    main()
