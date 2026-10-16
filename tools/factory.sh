#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
# Factory provisioning (the owner's path, CONTRACTS 2a.7): give one camera its own identity in the read-only "fctry"
# NVS partition (0x14000, 0x6000), so it boots straight into IoT Core without self-registration.
#
#   bash tools/factory.sh COM7                         read the MAC, make key + certificate, build and flash fctry.bin
#   bash tools/factory.sh COM7 --cert c.pem --key k.pem  use an existing certificate / key (e.g. flybot-aws data/secrets/)
#   bash tools/factory.sh --mac 02:00:00:00:00:01 --no-flash   build the image only (no board needed)
#   options: --chip esp32s3|esp32 (default esp32s3)  --thing <name> (default = serial)  --iot-host <endpoint>
#            --vcode <6 chars> (default: random, Crockford base32)  --out <dir> (default tools/out/<serial>)
#
# Steps: 1. serial = "ESP" + Wi-Fi MAC (as the firmware does)  2. key + self-signed certificate (CN = serial) unless given
# 3. prints the flybot-aws command that registers the certificate and the thing in AWS IoT Core: run it yourself with the
#    owner's AWS profile (this script never touches AWS)  4. fctry.csv -> fctry.bin (ESP-IDF nvs_partition_gen)
# 5. esptool write_flash 0x14000 fctry.bin  6. the label QR text for tools/label.py.
# Nothing here goes into git: tools/out/ is ignored (it holds the private key).
set -euo pipefail
export MSYS_NO_PATHCONV=1

HERE="$(cd "$(dirname "$0")" && pwd)"
PORT="" MAC="" CERT="" KEY="" CHIP="esp32s3" THING="" IOT_HOST="" VCODE="" OUT="" FLASH=1
FCTRY_OFFSET=0x14000 FCTRY_SIZE=0x6000   # must match firmware/partitions-*.csv
while [ $# -gt 0 ]; do
  case "$1" in
    --mac) MAC="$2"; shift 2 ;;
    --cert) CERT="$2"; shift 2 ;;
    --key) KEY="$2"; shift 2 ;;
    --chip) CHIP="$2"; shift 2 ;;
    --thing) THING="$2"; shift 2 ;;
    --iot-host) IOT_HOST="$2"; shift 2 ;;
    --vcode) VCODE="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    --no-flash) FLASH=0; shift ;;
    -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
    -*) echo "unknown option $1" >&2; exit 2 ;;
    *) PORT="$1"; shift ;;
  esac
done
np() { if command -v cygpath >/dev/null 2>&1; then cygpath -m "$1"; else echo "$1"; fi; }

IDF_PATH="${IDF_PATH:-C:/dev/esp-idf}"
case "$IDF_PATH" in *esp-idf-v[1-4]*) IDF_PATH="C:/dev/esp-idf" ;; esac   # an old install in the environment
PY="${PYTHON:-}"
if [ -z "$PY" ]; then
  for c in "C:/dev/espressif/python_env/idf5.4_py3.12_env/Scripts/python.exe" python3 python; do
    if command -v "$c" >/dev/null 2>&1 || [ -x "$c" ]; then PY="$c"; break; fi
  done
fi
NVSGEN="$IDF_PATH/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py"
[ -f "$NVSGEN" ] || { echo "nvs_partition_gen.py not found under IDF_PATH=$IDF_PATH" >&2; exit 2; }

# 1. serial from the MAC
if [ -z "$MAC" ]; then
  [ -n "$PORT" ] || { echo "give the serial port (COM7) or --mac" >&2; exit 2; }
  MAC="$("$PY" -m esptool --chip "$CHIP" --port "$PORT" read_mac 2>/dev/null | sed -n 's/^MAC: *//p' | head -1)"
  [ -n "$MAC" ] || { echo "could not read the MAC on $PORT" >&2; exit 1; }
fi
SERIAL="ESP$(printf '%s' "$MAC" | tr -d ':-' | tr 'a-f' 'A-F')"
case "$SERIAL" in ESP[0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F]) ;;
  *) echo "bad MAC $MAC" >&2; exit 2 ;; esac
THING="${THING:-$SERIAL}"
OUT="${OUT:-$HERE/out/$SERIAL}"
mkdir -p "$OUT"
echo "serial $SERIAL (MAC $MAC), thing $THING, output $OUT"

# 2. key + certificate
if [ -z "$CERT" ] || [ -z "$KEY" ]; then
  KEY="$OUT/key.pem"; CERT="$OUT/cert.pem"
  if [ ! -f "$KEY" ]; then
    openssl ecparam -name prime256v1 -genkey -noout -out "$(np "$KEY")"
    chmod 600 "$KEY" 2>/dev/null || true
    echo "created $KEY (ECDSA P-256)"
  fi
  if [ ! -f "$CERT" ]; then
    openssl req -new -x509 -key "$(np "$KEY")" -sha256 -days 3650 -subj "/CN=$SERIAL" -out "$(np "$CERT")"
    echo "created $CERT (self-signed, CN=$SERIAL, 10 years)"
  fi
fi
grep -q "BEGIN CERTIFICATE" "$CERT" || { echo "$CERT is not a certificate PEM" >&2; exit 2; }
grep -q "PRIVATE KEY" "$KEY" || { echo "$KEY is not a private key PEM" >&2; exit 2; }

# 3. verify code (CONTRACTS 2a.1: 6 characters, Crockford base32), kept in the image so the label can be printed now
if [ -z "$VCODE" ]; then
  VCODE="$("$PY" -c 'import secrets;a="0123456789ABCDEFGHJKMNPQRSTVWXYZ";print("".join(secrets.choice(a) for _ in range(6)))')"
fi

# 4. NVS image (namespace camos; the firmware reads it read-only, the runtime NVS overrides it)
q() { printf '"%s"' "$(printf '%s' "$1" | sed 's/"/""/g')"; }
CSV="$OUT/fctry.csv"
{
  echo "key,type,encoding,value"
  echo "camos,namespace,,"
  echo "serial,data,string,$(q "$SERIAL")"
  echo "vcode,data,string,$(q "$VCODE")"
  echo "thing,data,string,$(q "$THING")"
  [ -n "$IOT_HOST" ] && echo "mqtt_host,data,string,$(q "$IOT_HOST")"
  echo "claimed,data,i32,1"
  echo "cert,file,string,$(np "$CERT")"
  echo "key,file,string,$(np "$KEY")"
} > "$CSV"
chmod 600 "$CSV" 2>/dev/null || true
"$PY" "$(np "$NVSGEN")" generate "$(np "$CSV")" "$(np "$OUT/fctry.bin")" "$FCTRY_SIZE" >/dev/null
echo "wrote $OUT/fctry.bin ($FCTRY_SIZE bytes at $FCTRY_OFFSET). It holds the private key: do not share it."

# 5. flash
if [ "$FLASH" = 1 ]; then
  [ -n "$PORT" ] || { echo "no port: skipping the flash (--no-flash implied)"; FLASH=0; }
fi
if [ "$FLASH" = 1 ]; then
  "$PY" -m esptool --chip "$CHIP" --port "$PORT" write_flash "$FCTRY_OFFSET" "$(np "$OUT/fctry.bin")"
  echo "flashed fctry.bin to $PORT"
fi

# 6. what the human does next
FLYBOT_AWS="$(cd "$HERE/../../flybot-aws" 2>/dev/null && pwd || echo '<flybot-aws>')"
echo
echo "Register this certificate and thing in AWS IoT Core (owner's AWS profile; creates resources):"
echo "  bash $FLYBOT_AWS/infra/provision-camera.sh $SERIAL $(np "$CERT")"
echo "Label QR (print with: python tools/label.py $SERIAL $VCODE):"
echo "  CAMOS:1;SN:$SERIAL;VC:$VCODE;M:<model>;;"
