#!/usr/bin/env bash
# Diagnóstico rápido para el bridge MQTT<->CAN
# Úsalo en el mueble (máquina remota). No modifica el sistema.

set -u

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BRIDGE_DIR="$ROOT_DIR/Interface_MQTT_CAN_c"
BRIDGE_BIN="$BRIDGE_DIR/cobien_bridge"
CONV_JSON="$BRIDGE_DIR/conversion.json"
SIMULATOR="$ROOT_DIR/Simu_MQTT_CAN_interface.py"
LOG_DIRS=("$HOME/.local/state/cobien/logs" "/var/log/cobien" "$ROOT_DIR/logs")

echo "== COBIEN bridge diagnóstico =="
date
echo
echo "Sistema: $(uname -a)"
echo "Working dir: $PWD"
echo

cmd_exists(){ command -v "$1" >/dev/null 2>&1; }

echo "-- Herramientas disponibles --"
for c in gcc make python3 nc ss timeout mosquitto_sub mosquitto_pub; do
  if cmd_exists "$c"; then echo "  $c: OK"; else echo "  $c: MISSING"; fi
done
echo

echo "-- Estado del broker (systemd) --"
if cmd_exists systemctl; then
  systemctl status mosquitto --no-pager || true
else
  echo " systemctl no disponible"
fi
echo

echo "-- Listeners relevantes (puertos 1883, 9856) --"
if cmd_exists ss; then
  ss -ltnp | egrep 'LISTEN|:1883|:9856' || true
else
  if cmd_exists netstat; then netstat -ltnp | egrep 'LISTEN|:1883|:9856' || true; fi
fi
echo

echo "-- Buscar procesos relacionados --"
ps aux | egrep '(cobien_bridge|candump|cobien-launcher|mosquitto|mqtt|can0)' | grep -v egrep || true
echo

echo "-- Comprobar binario cobien_bridge --"
if [ -x "$BRIDGE_BIN" ]; then
  ls -lh "$BRIDGE_BIN"
  echo "Encontrado: $BRIDGE_BIN"
else
  echo "No existe ejecutable en $BRIDGE_BIN"
fi
echo

echo "-- Comprobar conversion.json --"
if [ -f "$CONV_JSON" ]; then
  echo "conversion.json presente: $CONV_JSON"
else
  echo "No se encontró $CONV_JSON"
fi
echo

echo "-- Revisar logs recientes de mosquitto (últimas 200 líneas) --"
if cmd_exists journalctl; then
  sudo journalctl -u mosquitto -n 200 --no-pager || true
else
  echo "journalctl no disponible"
fi
echo

echo "-- Revisar posibles logs del proyecto --"
for d in "${LOG_DIRS[@]}"; do
  if [ -d "$d" ]; then
    echo "Logs en $d (últimas 120 líneas):"
    tail -n 120 "$d"/* 2>/dev/null || ls -la "$d"
  fi
done
echo

usage(){
  cat <<EOF
Uso: $0 [--simulate] [--run-bridge] [--tail-logs]

Opciones:
  --simulate   : lanzar el simulador Python (if present) en background
  --run-bridge : ejecutar el binario cobien_bridge en primer plano (si existe) y guardar bridge_run.log
  --tail-logs  : mostrar tail -f de bridge_run.log si existe

Ejemplo:
  $0 --run-bridge
EOF
}

if [ "$#" -eq 0 ]; then
  usage
  exit 0
fi

while [ "$#" -gt 0 ]; do
  case "$1" in
    --simulate)
      if cmd_exists python3 && [ -f "$SIMULATOR" ]; then
        echo "Lanzando simulador en background (Simu_MQTT_CAN_interface.py)"
        nohup python3 "$SIMULATOR" >/tmp/simu_bridge.log 2>&1 &
        echo "Simulador PID: $! -> /tmp/simu_bridge.log"
      else
        echo "Simulador no disponible (python3 o archivo faltante)"
      fi
      shift
      ;;
    --run-bridge)
      if [ -x "$BRIDGE_BIN" ]; then
        echo "Ejecutando $BRIDGE_BIN $CONV_JSON (timeout 60s si disponible)"
        if cmd_exists timeout; then
          timeout 60s "$BRIDGE_BIN" "$CONV_JSON" 2>&1 | tee bridge_run.log || true
        else
          "$BRIDGE_BIN" "$CONV_JSON" 2>&1 | tee bridge_run.log || true
        fi
        echo "Salida guardada en bridge_run.log (últimas 200 líneas):"
        tail -n 200 bridge_run.log || true
      else
        echo "No hay ejecutable en $BRIDGE_BIN. Intenta compilar en la máquina de desarrollo.";
      fi
      shift
      ;;
    --tail-logs)
      if [ -f bridge_run.log ]; then
        tail -n 200 bridge_run.log
      else
        echo "No existe bridge_run.log en el directorio actual"
      fi
      shift
      ;;
    -h|--help)
      usage; exit 0
      ;;
    *)
      echo "Opción desconocida: $1"; usage; exit 2
      ;;
  esac
done

echo "== Fin diagnóstico =="
