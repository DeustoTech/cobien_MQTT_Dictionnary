from threading import Thread
import paho.mqtt.client as mqtt
import paho.mqtt.publish as publish
import json
import can
import can.interface
import time


class MQTT_to_CAN(Thread):  # Conversion from MQTT to CAN
    def __init__(self, can_bus, path: str, host: str = 'localhost'):
        super().__init__()
        self.CAN = can_bus
        self.path_conv = path
        self.host = host
        self.disconnect = (False, None)
    
    def on_connect(self, client, userdata, flags, reason_code, properties):
        """Fixed callback signature for API version 2"""
        print(f"Connected with result code {reason_code}")
    
    def on_disconnect(self, client, userdata, reason_code, properties):
        """Fixed callback signature for API version 2"""
        self.disconnect = (True, reason_code)
        print(f"Disconnected with reason code: {reason_code}")
    
    def on_message(self, client, userdata, msg):  # on MQTT message reception
        try:
            # decode the json from the message
            message = str(msg.payload.decode("utf-8", "ignore"))
            message = json.loads(message)
            keys = list(message.keys())  # get all the key from the json
            
            with open(self.path_conv, 'r') as CONV:
                conv = json.load(CONV)
            
            # translation from MQTT to CAN
            topic = msg.topic.split('/')  # extract the key for the translation from the topic
            
            if len(topic) < 2:
                print(f"Invalid topic format: {msg.topic}")
                return
                
            if topic[0] not in conv or topic[1] not in conv[topic[0]]:
                print(f"Topic not found in conversion table: {msg.topic}")
                return
                
            arbitration_id = conv[topic[0]][topic[1]]['arbitration_id']
            payload = []
            
            for i in range(len(keys)):  # Conversion of the different types of data for CAN
                value = message[keys[i]]
                try:
                    if conv[topic[0]][topic[1]]['data'][keys[i]] == 'hex':  # hexadecimal translation for color on 3 bytes
                        payload.append(int(value[1:3], 16))
                        payload.append(int(value[3:5], 16))
                        payload.append(int(value[5:7], 16))
                    elif conv[topic[0]][topic[1]]['data'][keys[i]] == "int":
                        payload.append(value)
                    elif conv[topic[0]][topic[1]]['data'][keys[i]] == "int16":
                        payload.append((value >> 8) & 0xFF)  # octet haut
                        payload.append(value & 0xFF)         # octet bas
                    elif conv[topic[0]][topic[1]]['data'][keys[i]] == "bool":  # for boolean variable : 1=True and 0=False
                        if value:
                            payload.append(1)
                        else:
                            payload.append(0)
                    else:
                        print(f'Unidentified variable type: {keys[i]}')
                except Exception as e:
                    print(f"Unexpected value for {keys[i]}: {e}")
            
            # if the payload < 8 bytes add 0
            if len(payload) < 8:
                for i in range(8-len(payload)):
                    payload.append(0)
            print("Conversion MQTT to CAN successful")
            
            self.can_write(arbitration_id, payload)  # send to CAN
            
        except Exception as e:
            print(f"Error processing MQTT message: {e}")
        
    def can_write(self, topic: int, payload: list):  # Send message on CAN bus
        try:
            msg = can.Message(arbitration_id=topic, data=payload, is_extended_id=False)
            self.CAN.send(msg)
            print(f"CAN message sent: {topic:X}, {payload}")
        except can.CanError as e:
            print(f"CAN message failed: {e}")
        except Exception as e:
            print(f"Unexpected error sending CAN message: {e}")
        
    def run(self):
        client = mqtt.Client(callback_api_version=mqtt.CallbackAPIVersion.VERSION2, protocol=mqtt.MQTTv5)
        
        # defined functions for MQTT
        client.on_connect = self.on_connect
        client.on_disconnect = self.on_disconnect
        client.on_message = self.on_message
        
        try:
            client.connect(self.host)
            
            # subscribe to the different topics with qos
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

            while not self.disconnect[0]:
                client.loop(timeout=1.0)
                
        except Exception as e:
            print(f"MQTT connection error: {e}")
        finally:
            client.disconnect()
            print(f"MQTT thread terminated")


class CAN_to_MQTT(Thread):  # Conversion from CAN to MQTT
    def __init__(self, can_bus, path: str, host: str = 'localhost'):
        super().__init__()
        self.can = can_bus
        self.path_conv = path
        self.host = host
        self.running = True

    def stop(self):
        self.running = False
        
    def run(self):
        try:
            # created listener for CAN bus
            listener = CAN_Listener(self.path_conv, self.host)  # use CAN_Listener class as can listener
            notifier = can.Notifier(self.can, [listener])
            
            while self.running:
                time.sleep(1)
                
        except KeyboardInterrupt:
            print("CAN to MQTT thread interrupted")
        except Exception as e:
            print(f"CAN to MQTT error: {e}")
        finally:
            if 'notifier' in locals():
                notifier.stop()
            print("CAN to MQTT thread terminated")


class CAN_Listener(can.Listener):
    
    def __init__(self, path, host: str = 'localhost'):
        super().__init__()
        self.host = host
        self.path_conv = path
    
    def on_message_received(self, msg):  # function executed on can message reception
        try:
            # extract id and payload
            arbitration_id = msg.arbitration_id
            message = msg.data
            
            # find MQTT corresponding topic
            with open(self.path_conv, 'r') as CONV:  # open conversion file
                conv = json.load(CONV)
            
            path = self.find_path(conv, arbitration_id)
            if not path:
                print(f"No conversion found for CAN ID: {arbitration_id:X}")
                return
                
            topic = conv[path[0]][path[1]]['topic']
            
            # payload translation
            payload = {}
            n = 0
            
            for field, value in conv[path[0]][path[1]]["data"].items():
                if n >= len(message):
                    break
                    
                if value == 'int':
                    payload[field] = message[n]
                    n += 1
                elif value == 'int16':
                    if n + 1 < len(message):
                        payload[field] = (message[n] << 8) | message[n+1]
                        n += 2
                    else:
                        break
                elif value == 'bool':  # for boolean : 1=True and 0=False
                    payload[field] = message[n] == 1
                    n += 1
                elif value == 'hex':  # decimal to hexadecimal conversion format #0F4A6E for RGB
                    if n + 2 < len(message):
                        hexa = '#'
                        for i in range(3):
                            num = hex(message[n]).split('x')[-1].upper()  # get hexadecimal value
                            if len(num) == 1:  # add 0 in front of the value if <16, value must be 6 characters long
                                num = f'0{num}'
                            hexa = f"{hexa}{num}"
                            n += 1
                        payload[field] = hexa
                    else:
                        break
                elif isinstance(value, dict):
                    data = self.find_path(conv, 1, (path + ["data"] + [field]))
                    if data:
                        payload[field] = data[-1]
            
            payload_json = json.dumps(payload)
            self.publish(topic, payload_json)  # publish to MQTT
            
        except Exception as e:
            print(f"Error processing CAN message: {e}")
                 
    def find_path(self, data, target, path=None):
        # return path from dictionaries
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
            print(f"MQTT publish failed: {e}")
    
    def rtr(self):
        pass


if __name__ == '__main__':
    from pathlib import Path
    import os
    
    # For software testing - use virtual CAN interface
    print("Initializing virtual CAN interface for software testing...")
    
    # Use virtual CAN interface for software testing
    bus = can.interface.Bus(interface='virtual')
    print("Virtual CAN interface initialized successfully")

    # Update path to your actual conversion file
    path = Path.cwd() / '/home/iris/Desktop/CoBien/CO_BIEN_MQTT_Dictionnary/conversion.json'  # MQTT/CAN translation json file
    

    if not path.exists():
        print(f"Conversion file not found at: {path}")
        print("Please update the path to your conversion.json file")
        exit(1)
    
    r_mqtt = MQTT_to_CAN(bus, path, "localhost")
    r_can = CAN_to_MQTT(bus, path, "localhost")
    
    try:
        r_mqtt.start()
        r_can.start()
        
        print("MQTT-CAN interface started. Press Ctrl+C to stop...")
        
        r_mqtt.join()
        r_can.join()
        
    except KeyboardInterrupt:
        print("\nShutting down...")
        r_can.stop()
    except Exception as e:
        print(f"Error: {e}")
    finally:
        if bus:
            bus.shutdown()
