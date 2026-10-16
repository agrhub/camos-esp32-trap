#!/usr/bin/env bash
# Provision one ESP32 camera for FlyBot Sentinel (firmware-esp32): device key + certificate, and the NVS image
# with everything the firmware reads at boot. Run from Git Bash / Linux / macOS; needs openssl and an ESP-IDF
# environment (IDF_PATH set, for nvs_partition_gen and esptool).
#
#   bash tools/provision.sh --serial ESP24A1602F3C10 --ssid MyWifi --pass secret \
#        [--mqtt-host <endpoint>] [--api-base <url>] [--token <token>] [--ap-pass <password>]
#        [--apn <apn>] [--tz Asia/Ho_Chi_Minh] [--interval <min>] [--power-mode auto|battery|powered]
#        [--prov-token <token>]   (pair at the first boot instead of giving --token)
#
# The setup portal (docs/PORTAL.md) does the same job without a PC; this script is for provisioning many cameras or
# for the AI-Thinker board without a button.
#
# Step 1 (no --token yet): creates out/<serial>/key.pem + cert.pem (ECDSA P-256, self-signed, CN = serial) and prints
#   the flybot-sentinel command that registers the certificate in AWS IoT Core and issues the Device API token.
# Step 2: run again with --token <token> (and --api-base): writes out/<serial>/nvs.bin, the flash command is printed.
#
# The serial is "ESP" + the 12 hex digits of the Wi-Fi MAC (same shape as AIC54F29F5167FA). Read the MAC with
#   python -m esptool --port COMx read_mac        (or take it from the first boot log line "serial ...")
# and pass it as --serial ESP<MAC without colons, upper case> or as --mac aa:bb:cc:dd:ee:ff.
#
# The private key never leaves out/<serial>/ except into the camera's flash. Keep out/ out of git (.gitignore).
set -euo pipefail
export MSYS_NO_PATHCONV=1   # Git Bash must not rewrite "/CN=..." as a path

HERE="$(cd "$(dirname "$0")" && pwd)"
SERIAL="" SSID="" PASS="" MQTT_HOST="${MQTT_HOST:-}" API_BASE="${API_BASE:-}" TOKEN="${DEVICE_TOKEN:-}" AP_PASS=""
APN="" TZ_NAME="" INTERVAL="" POWER_MODE="" PROV_TOKEN=""
NVS_SIZE=0x8000   # must match the nvs partition in firmware-esp32/partitions-*.csv

while [ $# -gt 0 ]; do
  case "$1" in
    --serial) SERIAL="$2"; shift 2 ;;
    --mac) SERIAL="ESP$(printf '%s' "$2" | tr -d ':-' | tr 'a-f' 'A-F')"; shift 2 ;;
    --ssid) SSID="$2"; shift 2 ;;
    --pass) PASS="$2"; shift 2 ;;
    --mqtt-host) MQTT_HOST="$2"; shift 2 ;;
    --api-base) API_BASE="$2"; shift 2 ;;
    --token) TOKEN="$2"; shift 2 ;;
    --ap-pass) AP_PASS="$2"; shift 2 ;;
    --apn) APN="$2"; shift 2 ;;
    --tz) TZ_NAME="$2"; shift 2 ;;
    --interval) INTERVAL="$2"; shift 2 ;;
    --power-mode) case "$2" in auto) POWER_MODE=0 ;; battery) POWER_MODE=1 ;; powered) POWER_MODE=2 ;; *) echo "bad --power-mode" >&2; exit 2 ;; esac; shift 2 ;;
    --prov-token) PROV_TOKEN="$2"; shift 2 ;;
    -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done
[ -n "$SERIAL" ] || { echo "--serial (or --mac) is required" >&2; exit 2; }
case "$SERIAL" in ESP[0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F]) ;;
  *) echo "note: serial $SERIAL is not ESP + 12 hex digits; the firmware only uses it as given" >&2 ;; esac

OUT="$HERE/out/$SERIAL"
# Native (C:/...) paths for openssl and Python on Windows; MSYS path conversion is off
np() { if command -v cygpath >/dev/null 2>&1; then cygpath -m "$1"; else echo "$1"; fi; }
mkdir -p "$OUT"

# 1. Key and certificate (once)
if [ ! -f "$OUT/key.pem" ]; then
  openssl ecparam -name prime256v1 -genkey -noout -out "$(np "$OUT/key.pem")"
  chmod 600 "$OUT/key.pem" 2>/dev/null || true
  echo "created $OUT/key.pem"
fi
if [ ! -f "$OUT/cert.pem" ]; then
  # AWS IoT registers it without a CA (provision-camera.sh), so a self-signed certificate is enough.
  openssl req -new -x509 -key "$(np "$OUT/key.pem")" -sha256 -days 3650 -subj "/CN=$SERIAL" -out "$(np "$OUT/cert.pem")"
  echo "created $OUT/cert.pem (CN=$SERIAL, valid 10 years)"
fi

# Where flybot-sentinel lives (sibling repo), for the registration command
SENTINEL="$HERE/../../flybot-aws"
REG="bash $( [ -d "$SENTINEL" ] && (cd "$SENTINEL" && pwd) || echo '<flybot-sentinel>')/infra/provision-camera.sh $SERIAL $OUT/cert.pem"

if { [ -z "$TOKEN" ] && [ -z "$PROV_TOKEN" ]; } || { [ -z "$SSID" ] && [ -z "$APN" ]; }; then
  echo
  echo "Next: register the certificate in AWS IoT Core and get the Device API token (printed once):"
  echo "  $REG"
  echo "Then run this script again with --ssid/--pass (or --apn), --api-base <url>, and either --token <token> --mqtt-host <endpoint>"
  echo "or --prov-token <token> (the camera pairs by itself at the first boot)."
  exit 0
fi

# 2. MQTT endpoint: argument, or ask AWS
if [ -z "$MQTT_HOST" ] && command -v aws >/dev/null 2>&1; then
  MQTT_HOST="$(aws --profile "${AWS_PROFILE:-sentinel}" --region "${AWS_REGION:-ap-southeast-2}" iot describe-endpoint \
    --endpoint-type iot:Data-ATS --query endpointAddress --output text 2>/dev/null || true)"
fi
[ -n "$MQTT_HOST" ] || [ -n "$PROV_TOKEN" ] || { echo "--mqtt-host <xxxx-ats.iot.<region>.amazonaws.com> is required (or --prov-token)" >&2; exit 2; }
[ -n "$API_BASE" ] || { echo "--api-base <https://<id>.execute-api.<region>.amazonaws.com[/stage]> is required" >&2; exit 2; }

# 3. NVS CSV. Values are quoted for CSV (a Wi-Fi password may contain commas or quotes).
q() { printf '"%s"' "$(printf '%s' "$1" | sed 's/"/""/g')"; }
CSV="$OUT/nvs.csv"
{
  echo "key,type,encoding,value"
  echo "camos,namespace,,"
  echo "serial,data,string,$(q "$SERIAL")"
  echo "wifi_ssid,data,string,$(q "$SSID")"
  echo "wifi_pass,data,string,$(q "$PASS")"
  [ -n "$MQTT_HOST" ] && echo "mqtt_host,data,string,$(q "$MQTT_HOST")"
  echo "api_base,data,string,$(q "$API_BASE")"
  [ -n "$TOKEN" ] && echo "token,data,string,$(q "$TOKEN")"
  [ -n "$PROV_TOKEN" ] && echo "prov_token,data,string,$(q "$PROV_TOKEN")"
  [ -n "$AP_PASS" ] && echo "ap_pass,data,string,$(q "$AP_PASS")"
  [ -n "$APN" ] && echo "apn,data,string,$(q "$APN")"
  [ -n "$TZ_NAME" ] && echo "tz,data,string,$(q "$TZ_NAME")"
  [ -n "$INTERVAL" ] && echo "interval,data,i32,$INTERVAL"
  [ -n "$POWER_MODE" ] && echo "power_mode,data,i32,$POWER_MODE"
  echo "cert,file,string,$(np "$OUT/cert.pem")"
  echo "key,file,string,$(np "$OUT/key.pem")"
} > "$CSV"
chmod 600 "$CSV" 2>/dev/null || true

# 4. nvs.bin
PY="${PYTHON:-python3}"; command -v "$PY" >/dev/null 2>&1 || PY=python
if [ -n "${IDF_PATH:-}" ] && [ -f "$IDF_PATH/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py" ]; then
  "$PY" "$(np "$IDF_PATH")/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py" generate "$(np "$CSV")" "$(np "$OUT/nvs.bin")" "$NVS_SIZE"
else
  "$PY" -m esp_idf_nvs_partition_gen generate "$(np "$CSV")" "$(np "$OUT/nvs.bin")" "$NVS_SIZE"
fi
echo
echo "wrote $OUT/nvs.bin ($NVS_SIZE). It holds the private key and Wi-Fi password: do not share it."
echo
echo "Flash it (ESP32-CAM: --chip esp32, ESP32-S3: --chip esp32s3), after the firmware itself:"
echo "  python -m esptool --chip esp32 --port COM5 write_flash 0x9000 $OUT/nvs.bin"
echo "Register the certificate (once per camera; skip if already done):"
echo "  $REG"
