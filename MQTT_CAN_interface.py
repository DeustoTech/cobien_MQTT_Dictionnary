from threading import Event, Thread
import paho.mqtt.client as mqtt
import paho.mqtt.publish as publish
import json
import os
import subprocess
from pathlib import Path
import can
import can.interface


DEFAULT_CONVERSION_CANDIDATES = (
    Path(__file__).resolve().parent / "Interface_MQTT_CAN_c" / "conversion.json",
)
DEFAULT_MQTT_HOST = os.getenv("COBIEN_MQTT_HOST", "localhost")
DEFAULT_CAN_INTERFACE = os.getenv("COBIEN_CAN_INTERFACE", "can0")
DEFAULT_CAN_BITRATE = int(os.getenv("COBIEN_CAN_BITRATE", "500000"))


def resolve_conversion_path():
    env_path = os.getenv("COBIEN_CONVERSION_PATH")
    if env_path:
        return env_path

    for candidate in DEFAULT_CONVERSION_CANDIDATES:
        if candidate.exists():
            return str(candidate)

    return str(DEFAULT_CONVERSION_CANDIDATES[0])


def _load_conversion(path: str) -> dict:
    with open(path, 'r') as f:
        return json.load(f)


class MQTT_to_CAN(Thread):
    def __init__(self, can_bus, path: str, host: str = 'localhost'):
        super().__init__()
        self.CAN = can_bus
        self.path_conv = path
        self.host = host
        self.disconnect = (False, None)
        self._stop_event = Event()
        self.conv = _load_conversion(path)

    def on_connect(self, client, userdata, flags, reason_code, properties):
        print(f"Connected with result code {reason_code}")

    def on_disconnect(self, client, userdata, reason_code, properties):
        self.disconnect = (True, reason_code)
        self._stop_event.set()
        print(f"Disconnected with reason code: {reason_code}")

    def on_message(self, client, userdata, msg):
        try:
            message = json.loads(msg.payload.decode("utf-8", "ignore"))
            keys = list(message.keys())

            topic = msg.topic.split('/')
            if len(topic) < 2:
                print(f"Invalid topic format: {msg.topic}")
                return

            if topic[0] not in self.conv or topic[1] not in self.conv[topic[0]]:
                print(f"Topic not found in conversion table: {msg.topic}")
                return

            entry = self.conv[topic[0]][topic[1]]
            arbitration_id = entry['arbitration_id']
            payload = []

            for key in keys:
                value = message[key]
                field_type = entry['data'].get(key)
                if field_type is None:
                    print(f"Field not in conversion table: {key}")
                    continue
                try:
                    if field_type == 'hex':
                        payload.append(int(value[1:3], 16))
                        payload.append(int(value[3:5], 16))
                        payload.append(int(value[5:7], 16))
                    elif field_type == 'int':
                        payload.append(int(value) & 0xFF)
                    elif field_type == 'int16':
                        v = int(value)
                        payload.append((v >> 8) & 0xFF)
                        payload.append(v & 0xFF)
                    elif field_type == 'bool':
                        payload.append(1 if value else 0)
                    elif isinstance(field_type, dict):
                        if value in field_type:
                            payload.append(int(field_type[value]))
                        else:
                            print(f"Enum value not found: {value} for field {key}")
                    else:
                        print(f"Unidentified variable type: {key} = {field_type}")
                except Exception as e:
                    print(f"Error converting field {key}: {e}")

            while len(payload) < 8:
                payload.append(0)

            print(f"Conversion MQTT to CAN successful: {msg.topic}")
            self.can_write(arbitration_id, payload)

        except json.JSONDecodeError as e:
            print(f"Invalid JSON in MQTT message on {msg.topic}: {e}")
        except Exception as e:
            print(f"Error processing MQTT message on {msg.topic}: {e}")

    def can_write(self, arbitration_id: int, payload: list):
        try:
            msg = can.Message(arbitration_id=arbitration_id, data=payload, is_extended_id=False)
            self.CAN.send(msg)
            print(f"CAN message sent: ID=0x{arbitration_id:X}, data={payload}")
        except can.CanError as e:
            print(f"CAN message failed: {e}")
        except Exception as e:
            print(f"Unexpected error sending CAN message: {e}")

    def run(self):
        client = mqtt.Client(callback_api_version=mqtt.CallbackAPIVersion.VERSION2, protocol=mqtt.MQTTv5)
        client.on_connect = self.on_connect
        client.on_disconnect = self.on_disconnect
        client.on_message = self.on_message

        try:
            client.connect(self.host)

            client.subscribe("rfid/init", qos=1)
            client.subscribe("rfid/read", qos=1)
            client.subscribe("sensors/init", qos=1)
            client.subscribe("sensors/update", qos=1)
            client.subscribe("led/config", qos=1)
            client.subscribe("proximity/config", qos=1)
            client.subscribe("proximity/update", qos=1)
            client.subscribe("imu/config", qos=1)
            client.subscribe("imu/update", qos=1)
            client.subscribe("threshold/config", qos=1)
            client.subscribe("threshold/update", qos=1)
            client.subscribe("time/config", qos=1)
            client.subscribe("time/update", qos=1)
            client.subscribe("button/config", qos=1)

            while not self._stop_event.is_set():
                client.loop(timeout=1.0)

        except Exception as e:
            print(f"MQTT connection error: {e}")
        finally:
            client.disconnect()
            print("MQTT thread terminated")


class CAN_to_MQTT(Thread):
    def __init__(self, can_bus, path: str, host: str = 'localhost'):
        super().__init__()
        self.can = can_bus
        self.path_conv = path
        self.host = host
        self._stop_event = Event()

    def stop(self):
        self._stop_event.set()

    def run(self):
        notifier = None
        try:
            listener = CAN_Listener(self.path_conv, self.host)
            notifier = can.Notifier(self.can, [listener])

            while not self._stop_event.wait(timeout=1.0):
                pass

        except KeyboardInterrupt:
            print("CAN to MQTT thread interrupted")
            self.stop()
        except Exception as e:
            print(f"CAN to MQTT error: {e}")
        finally:
            if notifier is not None:
                notifier.stop()
            print("CAN to MQTT thread terminated")


class CAN_Listener(can.Listener):

    def __init__(self, path, host: str = 'localhost'):
        super().__init__()
        self.host = host
        self.path_conv = path
        self.conv = _load_conversion(path)

    def on_message_received(self, msg):
        try:
            arbitration_id = msg.arbitration_id
            message = msg.data

            path = self.find_path(self.conv, arbitration_id)
            if path is None:
                print(f"Warning: No conversion found for CAN ID 0x{arbitration_id:X}")
                return

            topic = self.conv[path[0]][path[1]]['topic']
            payload = {}
            n = 0

            for field, value in self.conv[path[0]][path[1]]["data"].items():
                if n >= len(message):
                    break

                if value == 'int':
                    payload[field] = message[n]
                    n += 1
                elif value == 'int16':
                    if n + 1 < len(message):
                        payload[field] = (message[n] << 8) | message[n + 1]
                        n += 2
                    else:
                        break
                elif value == 'bool':
                    payload[field] = message[n] == 1
                    n += 1
                elif value == 'hex':
                    if n + 2 < len(message):
                        hexa = '#'
                        for i in range(3):
                            num = hex(message[n]).split('x')[-1].upper()
                            if len(num) == 1:
                                num = f'0{num}'
                            hexa = f"{hexa}{num}"
                            n += 1
                        payload[field] = hexa
                    else:
                        break
                elif isinstance(value, dict):
                    inv = {v: k for k, v in value.items()}
                    payload[field] = inv.get(message[n], message[n])
                    n += 1

            self.publish(topic, json.dumps(payload))

        except Exception as e:
            print(f"Error processing CAN message: {e}")

    def find_path(self, data, target, path=None):
        if path is None:
            path = []

        for key, value in data.items():
            current_path = path + [key]

            if isinstance(value, dict):
                found = self.find_path(value, target, current_path)
                if found:
                    return found
            else:
                if value == target:
                    return current_path
        return None

    def publish(self, topic, payload):
        try:
            publish.single(topic=topic, payload=payload, qos=1, hostname=self.host, protocol=mqtt.MQTTv5)
            print(f"MQTT message sent to {topic}:\n{payload}")
        except Exception as e:
            print(f"MQTT publish failed on {topic}: {e}")

    def rtr(self):
        pass


def _setup_can_interface():
    cmds = [
        ['sudo', 'ip', 'link', 'set', DEFAULT_CAN_INTERFACE, 'down'],
        ['sudo', 'ip', 'link', 'set', DEFAULT_CAN_INTERFACE, 'type', 'can', 'bitrate', str(DEFAULT_CAN_BITRATE)],
        ['sudo', 'ip', 'link', 'set', DEFAULT_CAN_INTERFACE, 'up'],
    ]
    for cmd in cmds:
        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode != 0:
            print(f"Warning: '{' '.join(cmd)}' returned {result.returncode}: {result.stderr.strip()}")


if __name__ == '__main__':
    _setup_can_interface()

    bus = can.interface.Bus(interface='socketcan', channel=DEFAULT_CAN_INTERFACE, bitrate=DEFAULT_CAN_BITRATE)

    path = resolve_conversion_path()

    r_mqtt = MQTT_to_CAN(bus, path, DEFAULT_MQTT_HOST)
    r_can = CAN_to_MQTT(bus, path, DEFAULT_MQTT_HOST)

    try:
        r_mqtt.start()
        r_can.start()

        r_mqtt.join(timeout=None)
        r_can.join(timeout=None)

    except KeyboardInterrupt:
        print("\nShutting down...")
        r_can.stop()
    except Exception as e:
        print(f"Fatal error: {e}")
    finally:
        r_mqtt.join(timeout=5)
        r_can.join(timeout=5)
        bus.shutdown()
        print("Shutdown complete")
