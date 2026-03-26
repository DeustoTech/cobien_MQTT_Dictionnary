from threading import Event, Thread
import paho.mqtt.client as mqtt
import paho.mqtt.publish as publish
import json
import os
from pathlib import Path
import can
import can.interface
import time


DEFAULT_CONVERSION_CANDIDATES = (
    Path(__file__).resolve().parent / "Interface_MQTT_CAN_c" / "conversion.json",
    Path("/home/iris/Desktop/CoBien/CO_BIEN_MQTT_Dictionnary/conversion.json"),
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


class MQTT_to_CAN (Thread): # Conversion from MQTT to CAN
    def __init__(self, can, path: str, host: str='localhost'):
        super().__init__()
        self.CAN = can
        self.path_conv = path
        self.host = host
        self.disconnect = (False, None)
        self._stop_event = Event()
   
    # CORRECTION 1: Ajout du paramètre 'properties' pour API v2
    def on_connect (self, client, userdata, flags, reason_code, properties):
        print(f"Connected with result code {reason_code}")
   
    # CORRECTION 2: Signature correcte pour on_disconnect API v2
    def on_disconnect (self, client, userdata, disconnect_flags, reason_code, properties): # Return disconnection reasons
        self.disconnect = (True, reason_code)
        self._stop_event.set()
   
    def on_message (self, client, userdata, msg): # on MQTT message reception
       
        # decode the json from the message
        message = str(msg.payload.decode("utf-8", "ignore"))
        message = json.loads(message)
        keys = list(message.keys()) # get all the key from the json
        with open(self.path_conv, 'r') as CONV:
            conv = json.load(CONV)
       
        # translation from MQTT to CAN
        topic = msg.topic.split('/') # extract the key for the translation from the topic
        arbitration_id = conv[topic[0]][topic[1]]['arbitration_id']
        payload = []
        for i in range (len(keys)): # Conversion of the differtes types of data for CAN
            value = message[keys[i]]
            try :
                if conv[topic[0]][topic[1]]['data'][keys[i]] == 'hex': # hexadecimal translation for color on 3 bytes
                    payload.append(int(value[1:3], 16))
                    payload.append(int(value[3:5], 16))
                    payload.append(int(value[5:7], 16))
                elif conv[topic[0]][topic[1]]['data'][keys[i]] == "int" :
                    payload.append(value)
                elif conv[topic[0]][topic[1]]['data'][keys[i]] == "bool": # for boolean variable : 1=True and 0=False
                    if value :
                        payload.append(1)
                    else :
                        payload.append(0)
                # CORRECTION: Gérer les autres types non reconnus
                elif conv[topic[0]][topic[1]]['data'][keys[i]] == "mode":
                    payload.append(value)  # Traiter comme un int
                else :
                    print(f'Unidentified variable type : {keys[i]} = {conv[topic[0]][topic[1]]["data"][keys[i]]}')
            except:
                print(f"Unexpected value : {keys[i]}")
       
        # if the payload < 8 bytes add 0             
        if len(payload) < 8:
            for i in range (8-len(payload)):
                payload.append(0)
        print ("Conversion MQTT to CAN successful")
       
        self.can_write (arbitration_id, payload) # send to CAN
       
    def can_write (self, topic: str, payload: list): # Send message on CAN bus
        msg = can.Message(arbitration_id=topic, data=payload, is_extended_id=False)
        try:
            self.CAN.send(msg)
            print (f"CAN message sent : {topic},{payload}")
        except can.CanError:
            print("CAN message failed")    
       
    def run (self):
        client = mqtt.Client(callback_api_version=mqtt.CallbackAPIVersion.VERSION2, protocol=mqtt.MQTTv5)
       
        # difined function for MQTT        
        client.on_connect = self.on_connect
        client.on_disconnect = self.on_disconnect
        client.on_message = self.on_message
       
        client.connect(self.host)
       
        # subscribe to the differents topics with qos
        client.subscribe("rfid/init", qos=1)
        client.subscribe("rfid/read", qos=1)
        client.subscribe("sensors/init", qos=1)
        client.subscribe("sensors/update", qos=1)
        client.subscribe("led/config", qos=1)
        client.subscribe("proximity/config", qos=1)
        client.subscribe("proximity/update", qos=1)
        client.subscribe("imu/config", qos=1)
        client.subscribe("imu/update", qos=1)

        # CORRECTION 3: Utiliser self.disconnect[0] au lieu de [1]
        while not self._stop_event.is_set():
            client.loop(timeout=1.0)
       
        print(f"Disconnected: {self.disconnect[1]}")
       
   
class CAN_to_MQTT (Thread): # Conversion from CAN to MQTT
    def __init__(self, can, path: str, host: str='localhost'):
        super().__init__()
        self.can = can
        self.path_conv = path
        self.host = host
        self._stop_event = Event()

    def stop(self):
        self._stop_event.set()
   
       
    def run (self):
        # created listener for CAN bus
        # CORRECTION 4: Passer 'host' au listener
        listener = CAN_Listener(self.path_conv, self.host) # use CAN_Listener class as can listener
        notifier = can.Notifier(self.can, [listener])
        try:
            while not self._stop_event.wait(timeout=1.0):
                pass
        except KeyboardInterrupt:
            print("Keyboard interrupt")
            self.stop()
        finally:
            notifier.stop()


class CAN_Listener (can.Listener):
   
    def __init__ (self, path, host: str = 'localhost'):
        super().__init__()
        self.host = host
        self.path_conv = path
   
    def on_message_received(self, msg): # function executed on can message reception
        try:
            # extract id and payload
            arbitration_id = msg.arbitration_id
            message = msg.data
           
            # find MQTT coresponding topic
            with open(self.path_conv, 'r') as CONV: # open conversion file
                conv = json.load(CONV)
            
            path = self.find_path(conv, arbitration_id)
            
            # CORRECTION: Vérifier si path existe
            if path is None:
                print(f"Warning: No conversion found for CAN ID 0x{arbitration_id:X}")
                return
            
            topic = conv[path[0]][path[1]]['topic']

            # payload translation
            payload = {}
            n = 0
            for field, value in conv[path[0]][path[1]]["data"].items():
                if value == 'int':
                    payload[field] = message[n]
                    n += 1
                elif value == 'int16':
                    payload[field] = (message[n] << 8) | message[n + 1]
                    n += 2
                elif value == 'bool': # for boolean : 1=True and 0=False
                    payload[field] = message[n] == 1
                    n += 1
                elif value == 'hex': # decimal to hexadecimal conversion format #0F4A6E for RGB
                    hexa = '#'
                    for i in range(3):
                        num = hex(message[n]).split('x')[-1].upper() # get hexadecimal value
                        if len(num) == 1: # add 0 in front of the value if <16, value must be 6 caractére long
                            num = f'0{num}'
                        hexa = f"{hexa}{num}"
                        n += 1
                    print(hexa)
                    payload[field] = hexa
                elif isinstance(value, dict):
                    data = self.find_path(conv, 1, (path + ["data"] + [field]))
                    if data:
                        payload[field] = data[-1]

            self.publish(topic, json.dumps(payload)) # publish to MQTT
        except Exception as e:
            print(f"Error in CAN message processing: {e}")
                
    def find_path(self, data, target, path=None):
        # return path from ditionnaries
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
   
    def publish (self, topic, payload):
        try :
            # CORRECTION 5: Ajout de 'protocol' pour cohérence
            publish.single(topic=topic, payload=payload, qos=1, hostname=self.host, protocol=mqtt.MQTTv5)
            print (f"MQTT message sent to {topic}: /n{payload}")
        except :
            print ("MQTT publish failed")
   
    def rtr (self):
        pass


if __name__ == '__main__':
    # can periferal initialisation
    os.system(f'sudo ip link set {DEFAULT_CAN_INTERFACE} down') # sudo ifconfig can0 down
    os.system(f'sudo ip link set {DEFAULT_CAN_INTERFACE} type can bitrate {DEFAULT_CAN_BITRATE}') # sudo ip link set can0 type can bitrate 1000000
    os.system(f'sudo ip link set {DEFAULT_CAN_INTERFACE} up') # sudo ifconfig can0 up

    bus = can.interface.Bus(interface='socketcan', channel=DEFAULT_CAN_INTERFACE, bitrate=DEFAULT_CAN_BITRATE)
   
    path = resolve_conversion_path()
   
    r_mqtt = MQTT_to_CAN(bus, path, DEFAULT_MQTT_HOST)
    r_can = CAN_to_MQTT(bus, path, DEFAULT_MQTT_HOST)
   
    r_mqtt.start()
    r_can.start()
   
    r_mqtt.join()
    r_can.join()
