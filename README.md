# CO_BIEN_MQTT_Dictionnary
Dictionnary for CO-BIEN MQTT data exchange

## Requisitos (MQTT Broker)
Este módulo actúa como cliente. Es **necesario** disponer de un broker MQTT (como Mosquitto) corriendo en la máquina local (`localhost:1883`) para que se puedan intercambiar los mensajes entre el hardware (CAN) y la interfaz de usuario.

**Instalación rápida en Ubuntu/Debian:**
```bash
sudo apt update
sudo apt install mosquitto mosquitto-clients
sudo systemctl enable mosquitto
sudo systemctl start mosquitto
``` 
