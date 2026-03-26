import paho.mqtt.client as mqtt
import paho.mqtt.publish as publish
import json
import os
from pathlib import Path

EXAMPLES_DIR = Path(__file__).resolve().parent
PATH_EXAMPLE = Path(os.getenv("COBIEN_EXAMPLE_LED_PATH", EXAMPLES_DIR / "led.json"))
MQTT_HOST = os.getenv("COBIEN_MQTT_HOST", "localhost")

def main():
    # Create client with callback API version 2 (current version)
    client = mqtt.Client(callback_api_version=mqtt.CallbackAPIVersion.VERSION2, protocol=mqtt.MQTTv5)

    # Connect to broker
    client.connect(MQTT_HOST)

    # Read and publish JSON data
    with open(PATH_EXAMPLE, 'r', encoding='utf-8') as msg:
        data = json.load(msg)

    payload = json.dumps(data)

    # Publish message
    publish.single('led/config', payload, qos=1, hostname=MQTT_HOST, protocol=mqtt.MQTTv5)


if __name__ == "__main__":
    main()
