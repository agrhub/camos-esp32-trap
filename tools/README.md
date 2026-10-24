# tools

| File | Purpose |
|---|---|
| `build.ps1` | Windows build / flash / monitor helper (`tools\build.ps1 freenove build`, see the top-level README) |
| `provision.sh` | per-camera key + self-signed certificate (CN = serial), the AWS IoT registration command, and an NVS image with Wi-Fi / APN / FlyBot URL / token (the alternative to the portal for many cameras or the button-less AI-Thinker board) |
| `label.py` | the label QR `CAMOS:1;SN:...;VC:...;M:...;;` for FlyBot's "add camera" scan |
| `factory.sh` / `factory.ps1` | factory provisioning: the read-only `fctry` NVS image (serial, verify code, certificate, key, thing) flashed at 0x14000; prints the AWS IoT registration command (docs/dev/FACTORY.md) |
| `serial_log.py` | non-interactive serial capture and console driver: `python tools/serial_log.py COM7 --reset --seconds 60`, `--cmd info --cmd scan` |
| `mock/server.js` | mock camera serving the setup page and the web console with fake data (docs/dev/BUILD.md) |
| `diagrams/wiring.py` | draws the illustration of the mounted trap in docs/img |

## Provisioning with provision.sh

1. Read the serial from the first boot log (`serial ESP..., verify code ...`) or from the MAC:
   `python -m esptool --port COM7 read_mac` -> `ESP` + the 12 hex digits.
2. `bash tools/provision.sh --serial ESP000000000002` creates `tools/out/<serial>/key.pem` + `cert.pem` and prints the
   command that registers the certificate in AWS IoT Core (run it with your own AWS credentials).
3. Either pair through FlyBot (`--prov-token <token from "add camera">`), or give the token and endpoint directly:
   ```bash
   bash tools/provision.sh --serial ESP000000000002 --ssid Vuon-A --pass '...' --api-base https://flybot.agrhub.com \
     --prov-token <token> --tz Asia/Ho_Chi_Minh --interval 30 --power-mode auto [--apn v-internet]
   ```
   writes `tools/out/<serial>/nvs.bin` (git-ignored: it holds the private key and the Wi-Fi password).
4. Flash it after the firmware: `python -m esptool --chip esp32s3 --port COM7 write_flash 0x9000 tools/out/<serial>/nvs.bin`
   (`--chip esp32` for the AI-Thinker board). `idf.py flash` leaves the NVS partition alone afterwards.

The certificate and key can also be pasted into the portal page (`POST /api/cert`) or the console (`cert`, `key`).
NVS keys: docs/dev/CONFIG.md section 5.

## AI-Thinker ESP32-CAM: serial adapter

5 V and GND from a USB-serial adapter, adapter TX -> U0R, adapter RX -> U0T, GPIO0 to GND while powering up to enter
download mode (remove the jumper afterwards and press reset). Portal button: a push button between GPIO13 and GND.
Healthy boot log: `serial ... cert yes key yes`, `got IP`, `clock ...`, `connected to mqtts://...`.
