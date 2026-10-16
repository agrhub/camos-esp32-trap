# Pre-provisioning cameras (factory path)

By default every camera registers itself with FlyBot (it makes its own key, sends a certificate request to
`POST /api/device/register` and waits to be claimed). A farm team that prepares many cameras can instead give each one
its certificate before it leaves the bench. Such a camera never self-registers.

## Factory partition (tools/factory.sh, tools/factory.ps1)

```bash
bash tools/factory.sh COM7                              # read the MAC, make key + certificate, build and flash fctry.bin
bash tools/factory.sh COM7 --cert c.pem --key k.pem     # use an existing certificate and key
bash tools/factory.sh --mac 02:00:00:00:00:01 --no-flash   # build the image only
```

The script derives the serial from the Wi-Fi MAC (`ESP` + 12 hex digits, as the firmware does), makes an EC key and a
self-signed certificate (CN = serial) unless given, builds the read-only `fctry` NVS image (serial, verify code,
certificate, key, thing name, claimed) and flashes it at 0x14000. It prints the command that registers the certificate
and the thing in AWS IoT Core; run that yourself with your own AWS credentials (it creates AWS resources; the script
never touches AWS). Everything it writes stays in `tools/out/` (git-ignored: it holds private keys).

`tools/label.py <serial> <verify code>` draws the label QR (`CAMOS:1;SN:...;VC:...;M:...;;`) that FlyBot's "Add camera"
scans.

## NVS image with Wi-Fi and FlyBot settings (tools/provision.sh)

The alternative for boards without a button (AI-Thinker ESP32-CAM) or for many cameras on the same network: see
[../../tools/README.md](../../tools/README.md). The image goes to 0x9000 and holds the Wi-Fi password: keep it out of git.

## What a factory reset keeps

The console / web console / RPC `factory` reset returns every setting to the build defaults and keeps the identity:
serial, verify code, certificate, key, device token, thing, IoT endpoint, KVS channel and the claim state. The
`-install.bin` release image and `idf.py erase-flash` erase both NVS partitions; the `-update.bin` image keeps them.
