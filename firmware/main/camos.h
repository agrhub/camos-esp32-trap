/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * camos-esp32-trap: ESP32 / ESP32-S3 fruit-fly trap camera speaking the CamOS device protocol v1
 * (docs/dev/protocol-v1.md) over AWS IoT Core, with the FlyBot additions (docs/dev/PROTOCOL_ADDITIONS.md).
 * The camera only captures and uploads; the counting is done in the cloud (OpenCV 5).
 * Shared declarations for all modules.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include "cJSON.h"
#include "esp_camera.h"
#include "sdkconfig.h"
#include "power_decide.h"
#include "fleet_policy.h"
#include "logfwd_core.h"

#define CAMOS_PROTOCOL 1
#define CAMOS_NS "camos" /* NVS namespace, see tools/provision.sh and docs/PORTAL.md */

/* ------------------------------------------------------------------ identity and provisioning (store.c) */


typedef struct {
	char serial[24];      /* client id and certificate CN: "ESP" + 12 hex digits of the MAC (15 chars) */
	char verify_code[12]; /* label verify code (protocol 12.1 "VC"), random, generated at the first boot */
	char wifi_ssid[33];
	char wifi_pass[65];
	char apn[64];         /* 4G APN; empty = no cellular */
	char sim_pin[9];
	char api_base[128];   /* FlyBot base URL: https://flybot.example.com (CONTRACTS section 2) */
	char token[130];      /* device token from pairing, sent as X-Device-Token */
	char prov_token[96];  /* provisioning token from the FlyBot QR / portal, used once by POST /api/device/pair */
	char mqtt_host[128];  /* AWS IoT endpoint, optional ":port" (default 8883) */
	char thing_name[64];
	char kvs_channel[64];
	char kvs_region[24];
	char role_alias[64];  /* IoT credential provider role alias (live view credentials) */
	char cred_host[128];  /* IoT credential provider endpoint (xxxx.credentials.iot.<region>.amazonaws.com) */
	char tz[48];          /* IANA zone from the portal; the shadow `timezone` wins once a shadow arrived */
	char claim_url[160];  /* from registration (CONTRACTS 2a), "" = apiBase/claim?sn=<serial> */
	char ui_lang[4];      /* language of the portal and web console: "en" (default) or "vi" */
	int interval_min;     /* capture interval from the portal / defaults; the shadow wins */
	int power_mode;       /* power_mode_t from the portal / defaults; the shadow `power.mode` wins */
	int cellular;         /* 1 = use 4G when a modem answers (APN "" = auto from the SIM) */
	int gps;              /* 1 = take GPS fixes when the module is fitted */
	char gps_systems[12]; /* satellite systems: "auto" (by region, default), "gps", "bds", "gps+bds", "gps+glonass" */
	char net_prefer[8];   /* network: "auto" (Wi-Fi first, 4G when it fails; default), "wifi" (never 4G), "4g" (4G only) */
	int led;              /* 1 = status LED on */
	int batt_mon;         /* 1 = battery / solar dividers are wired (an unwired ADC pin floats and reads noise) */
	int claimed;          /* 0 = self-registered and not claimed in FlyBot yet (CONTRACTS 2a.3) */
	int sd_save;          /* 1 = every capture is also written to the microSD card (sd.c) */
	int sched_from;       /* local capture window (minutes after midnight) when the cloud sets no detections[] */
	int sched_to;         /* inclusive; < sched_from crosses midnight */
	int night_sleep;      /* 1 = deep sleep through the night window, in every power mode (sched.c) */
	int night_from;       /* night window, local minutes after midnight: [night_from, night_to), < crosses midnight */
	int night_to;
	int motion;           /* 1 = frame-difference motion detection (powered mode, motion.c) */
	int motion_sens;      /* 1..100 */
	int motion_cool;      /* seconds between two motion triggers */
	int motion_upload;    /* 1 = a motion trigger also captures and uploads a full photo to FlyBot */
	int motion_sd;        /* 1 = a motion trigger saves the picture to the microSD card */
	char motion_roi[24];  /* "x,y,w,h" in percent of the picture */
	char capture_size[12]; /* photo size: "max" (the sensor's largest) or "WxH" (CAPTURE_SIZES) */
	char usb_mode[8];     /* native USB port at boot: "auto" (a USB drive plugged in: host for it, the photo storage;
						   else drive + webcam for a computer; default), "storage", "webcam" or "off" */
	int ip_mode;          /* 0 DHCP, 1 static (Wi-Fi) */
	/* board wiring: Kconfig gives the defaults, NVS keeps what this board has through an OTA and a factory reset */
	int modem_pwrkey;     /* PWRKEY GPIO, -1 = not wired (the module is always on, AT+CFUN switches the radio) */
	int modem_power;      /* GPIO of the 4G supply switch, -1 = none (a switch that is not fitted is found out) */
	int modem_power_low;  /* 1 = the switch is on when its pin is low (P-MOSFET gate), 0 = high (NPN on the gate) */
	int modem_fast_baud;  /* UART rate after the first AT (0 = keep CAMOS_MODEM_BAUD) */
	int gps_pwr_low;      /* 1 = the GPS switch is on when its pin is low (P-MOSFET), 0 = high (NPN / direct) */
	int batt_div;         /* battery divider ratio x 100: (Rtop + Rbottom) / Rbottom, 200 = 100k/100k */
	int solar_div;        /* solar / charger input divider ratio x 100, 500 = the 30k/7.5k "voltage sensor" module */
	char static_ip[16], static_mask[16], static_gw[16], static_dns[16];
	char ap_pass[65];
	char *cert;           /* device certificate PEM (malloc, lives forever: esp-mqtt keeps the pointer) */
	char *key;            /* device private key PEM */
} cfg_t;

extern cfg_t g_cfg;
extern char g_wifi_error[33];                  /* SSID that could not be joined after the portal, "" none */
extern char g_reg_error[64];                   /* last registration error ("serial_taken" ...), "" none */
bool store_set_key(const char *key_pem);       /* the device key alone (registration generates it first) */
void store_claim_url(char *out, size_t n);     /* registration's claimUrl, else <apiBase>/claim?sn=<serial> */
const char *portal_ssid(void);                 /* "CAM-xxxx" */

void store_load(void);
void store_set_str(const char *key, const char *value); /* NVS string, "" erases */
void store_clear_admin(void);                  /* the setup button: the web console asks for a new password */
void store_set_wifi(const char *ssid, const char *pass);
void store_set_flag(const char *name, int value);
int store_get_flag(const char *name);
int store_get_flag_default(const char *name, int dflt); /* dflt when the key is absent */
bool store_get_blob(const char *name, void *out, size_t len); /* exactly len bytes */
bool store_get_blob_upto(const char *name, void *out, size_t len, size_t *got); /* a blob up to len bytes; got = its size */
void store_set_blob(const char *name, const void *data, size_t len);
bool store_set_pem(const char *cert, const char *key);  /* both PEM, validated (BEGIN/END lines) */
void store_erase_all(void);                             /* factory reset: keeps serial and verify code */
bool store_paired(void);                                /* token and IoT host present */
bool store_drop_registration(void);                      /* removed in FlyBot: forget certificate, token, thing */
/* Apply one portal / console key ("ssid", "password", "apn", "apiBase", ...). Returns false for an unknown key or a
 * bad value. Updates g_cfg and NVS. */
bool store_apply_kv(const char *key, const char *value);
void store_config_json(cJSON *out, bool secrets); /* the portal / console view of the configuration */

/* ------------------------------------------------------------------ small helpers (util.c) */

uint64_t now_ms(void);           /* Unix ms when the clock is synced, else uptime ms */
bool clock_synced(void);
void new_ulid(char out[27]);     /* protocol 3: ULID, 26 Crockford base32 characters */
const char *boot_reason_name(void);
const char *chip_reset_name(void);              /* reset reason of this boot: power_on, crash, task_watchdog, brownout ... */
void last_reset_json(cJSON *parent);            /* lastReset {reason, detail, at, uptimeS} (util.c) */
void remember_reboot_reason(const char *reason); /* "rpc" survives the restart in RTC memory */
uint32_t reboot_count(void);
int jpeg_dims(const uint8_t *jpg, size_t len, int *w, int *h);
void iso8601(time_t t, char out[25]);              /* "2026-10-16T08:00:00Z" */
time_t parse_iso8601(const char *s);               /* 0 when unparsable; accepts Z or +hh:mm */
const char *fw_version(void);

/* IANA name from the shadow `timezone` (protocol 8) -> POSIX TZ string; NULL when unknown (tz.c) */
const char *tz_lookup(const char *name);

/* ------------------------------------------------------------------ network (net.c, wifi.c, modem.c) */

typedef enum { NET_NONE = 0, NET_WIFI, NET_4G } net_type_t;

void net_start(void);              /* Wi-Fi first when one is set, else (or when it gives no IP) 4G; asynchronous */
void net_ensure(void);             /* start the network if it is not running yet (battery mode keeps it off until a capture) */
bool net_started(void);
bool net_up(void);                 /* has an IP */
net_type_t net_type(void);         /* the network in use (or chosen) */
const char *net_type_name(void);   /* "wifi" | "4g" | "none" */
int net_rssi(void);                /* dBm (Wi-Fi) or CSQ-derived dBm (4G); 0 unknown */
void net_ip(char *out, size_t n);
void net_cell_json(cJSON *net);    /* adds imei, iccid, operator, csq, registered (4G only) */
bool net_wait_up(int timeout_s);   /* wait for IP and a valid clock */
void net_sleep_prepare(void);      /* modem off (PWRKEY), Wi-Fi stop, before deep sleep */
void net_set_up_cb(void (*cb)(void)); /* called on every GOT_IP (Wi-Fi or PPP) */
void time_sync_start(void);        /* SNTP, with GET /api/device/time as the fallback */
void net_4g_restarted(void);       /* modem.c recovered 4G in place: MQTT and the clock follow */

void wifi_start_sta(void);
bool wifi_started(void);
bool wifi_up(void);
int wifi_rssi(void);
void wifi_ip(char *out, size_t n);
void wifi_set_up_cb(void (*cb)(void));
void wifi_stop(void);
int wifi_failures(void);           /* failed join attempts since the start or the last IP */
int wifi_last_reason(const char **name); /* why the last join failed: the driver's reason (0 none), named or "" */
bool wifi_probe(int *rssi);        /* after wifi_stop (4G in use): one scan for the configured network */
void wifi_release(void);           /* 4G took over: stop and free the driver (wifi_probe starts it for its scan) */

/* hostname.c: flybot-<last 6 of the serial>, the DHCP host name and <name>.local over mDNS */
const char *local_hostname(void);              /* "flybot-a1b2c3" */
void local_hostname_url(char *out, size_t n);  /* "http://flybot-a1b2c3.local/" */
struct esp_netif_obj;
void local_hostname_apply(struct esp_netif_obj *sta); /* before DHCP starts */
void local_mdns_start(void);                   /* once; answers <name>.local and announces the console */

bool modem_available(void);        /* built with a modem, 4G on, the module not found missing before */
typedef enum { MODEM_OK = 0, MODEM_NOT_FITTED, MODEM_NO_SIM, MODEM_NO_NETWORK, MODEM_NO_DATA } modem_fail_t;
modem_fail_t modem_last_failure(void);
const char *modem_apn(void);       /* the APN in use (the user's or auto from the IMSI) */
bool modem_start(void);            /* power on, AT sync, PPP; returns false when no modem answers or no SIM */
void modem_stop(void);             /* PPP down, AT+CPOF, PWRKEY, power rail off */
void modem_park(void);             /* always-on module unused (Wi-Fi): its radio off, AT+CFUN=0 */
void modem_radio_pause(void);      /* 4G radio off for a while (a GPS fix): modem_stop, the watchdog waits */
bool modem_radio_resume(void);     /* and back: modem_start, PPP and MQTT again */
bool modem_up(void);
bool modem_cell_location(double *lat, double *lng, int *acc_m); /* AT+CLBS: the 4G cell's position */
int modem_rssi(void);
void modem_cell_json(cJSON *net);
/* The modem's IMEI / ICCID from the last time it answered (kept in NVS), empty when never read */
void modem_saved_ids(char *imei, size_t in, char *iccid, size_t cn);
int modem_at(const char *cmd, char *out, size_t n, int timeout_ms); /* console `at` command; -1 when not in command mode */
void modem_ip(char *out, size_t n);

/* ------------------------------------------------------------------ portal (portal.c) */

void portal_run(void) __attribute__((noreturn)); /* SoftAP CAM-xxxx + captive portal, restarts when done */
bool portal_active(void);                       /* the setup portal runs (this boot) */
esp_err_t portal_try(const char *ssid, const char *pass); /* join check with the AP up; stored once it gets an IP */
/* BOOT button: released after 3-10 s = setup portal (settings kept), held 10 s = factory reset (identity kept) */
#define BUTTON_PORTAL_MS 3000
#define BUTTON_FACTORY_MS 10000
int button_hold_ms(int max_ms);  /* at start: how long the button stays pressed, up to max_ms */
void button_task_start(void);

/* ------------------------------------------------------------------ settings from the shadow (shadow.c) */

#define MAX_SCHED 4
typedef struct {
	char id[24];       /* detection id */
	int every_min;     /* capture period in minutes */
	int from_min;      /* window start, minutes after local midnight */
	int to_min;        /* window end (inclusive); < from_min means across midnight */
} sched_t;

typedef struct {
	int version;
	char tz[64];          /* POSIX TZ string */
	int jpeg_quality;     /* protocol 1..100 */
	int brightness;       /* protocol -64..64 */
	int contrast;         /* protocol 0..100 */
	int saturation;       /* protocol 0..100 */
	int sharpness;        /* protocol 0..100 */
	int orientation;      /* protocol 0..3: 0 normal, 1 mirror, 2 flip, 3 both (on top of the Kconfig flip) */
	/* ESP32 additions under video.image (docs/dev/PROTOCOL_ADDITIONS.md) */
	int wb_mode;          /* 0 auto, 1 sunny, 2 cloudy, 3 office, 4 home */
	int effect;           /* 0 none, 1 negative, 2 grayscale, 3 red, 4 green, 5 blue, 6 sepia */
	int ae_level;         /* -5..5 (OV5640), -2..2 others */
	int exposure_auto;    /* 1 auto exposure */
	int exposure;         /* 0..1200 when manual */
	int gain_auto;        /* 1 auto gain */
	int gain;             /* 0..30 when manual */
	int gain_ceiling;     /* 0..6 = 2x..128x */
	int denoise;          /* 0..8 (OV5640) */
	sched_t sched[MAX_SCHED];
	int n_sched;
	/* v1 additions (docs/dev/PROTOCOL_ADDITIONS.md) */
	int interval_min;     /* capture.intervalMin, 0 = not set by the shadow */
	int power_mode;       /* power.mode, -1 = not set by the shadow */
	int power_high_mv;    /* power.highMv / power.lowMv: auto mode hysteresis, 0 = Kconfig default */
	int power_low_mv;
	bool live_requested;  /* live.requested */
	bool tz_set;          /* `timezone` came from the shadow (else automatic, CONTRACTS 2a.6) */
	char tz_iana[48];
	int claimed;          /* shadow `claimed`: -1 not said, 0/1 */
	/* New fields go last: a shorter blob saved by an older release still loads, the new ones at their defaults */
	int night_mode;       /* video.image.nightMode: streams may stretch a frame up to 4x in low light (OV5640) */
	int disabled;         /* `disabled`: an administrator paused the camera (no captures, uploads or live view) */
} settings_t;

void shadow_init(void);                         /* load the persisted desired document */
void shadow_get(settings_t *out);               /* consistent copy */
/* Apply a `desired` document. Returns the `reported` JSON (caller frees) and persists the document. */
char *shadow_apply(const char *json, int len, bool persist);
char *shadow_reported_json(void);               /* retained shadow/reported, caller frees */
void shadow_image_json(cJSON *parent);          /* adds video.image and capture (also used by state, protocol 8) */
void shadow_additions_json(cJSON *parent);      /* power, live, kvs, location, net, battery (reported and state) */
int shadow_version(void);
int shadow_interval_min(void);                  /* effective capture interval: shadow > portal > Kconfig */
power_mode_t shadow_power_mode(void);           /* configured mode (not the effective one, see power.c) */
void shadow_clear_live_request(void);           /* live.requested handled */
bool shadow_take_capture_request(int64_t *ms);  /* capture.requestNow not handled yet (taken once) */
void shadow_capture_request_done(int64_t ms);   /* remembered, and reported as capture.requestNow */
void shadow_reload(void);                       /* back to the saved settings (the console's image preview) */
bool shadow_claimed(void);                      /* CONTRACTS 2a: false = unclaimed mode (1 snapshot per nextWakeAt) */
/* Paused by a FlyBot administrator: the shadow's `disabled`, or a snapshot upload answered 403 "disabled" (kept in
 * RAM until the next desired document says otherwise). No captures, uploads, motion photos or live view; OTA works. */
bool shadow_disabled(void);
void shadow_note_disabled(bool on);

/* ------------------------------------------------------------------ camera (cam.c) */

/* Powers the sensor up, applies settings, discards warm-up frames, returns a JPEG at maximum sensor size. */
esp_err_t cam_capture(const settings_t *s, camera_fb_t **fb);
void cam_release(camera_fb_t *fb);              /* returns the frame and powers the sensor down */
void cam_sleep_prepare(void);                   /* hold PWDN / camera power off through deep sleep */
void cam_max_size(int *w, int *h);              /* maximum JPEG size of the detected sensor, 0 before the first capture */
void cam_photo_size(int *w, int *h);            /* what a capture gives: captureSize within that maximum */
int cam_sizes(bool live, int out[][2], int max); /* photo sizes (live false) or live JPEG sizes of this sensor */
bool cam_size_ok(bool live, int w, int h);      /* one of them (any known size while the sensor is unknown) */
void cam_info_json(cJSON *o);                   /* reported `camera`: sensor, maxW, maxH, photoSizes, liveSizes */
void cam_apply_settings(const settings_t *s);   /* the image settings to a running sensor (call with the camera lock) */
const char *cam_sensor_name(void);              /* "OV5640" ..., "" before the first power-up */
void cam_set_night_override(int v);             /* console diagnosis: -1 = default, 0 = night mode off, 1 = on */
void cam_print_regs(void);                      /* console diagnosis: the OV5640's exposure, gain, AWB, clock registers */
int cam_reg(int addr, int value);               /* console diagnosis: read (value < 0) or write a sensor register */
/* Live view: the sensor at a small size, streaming JPEG frames (ESP32-S3). Returns ESP_OK and keeps the sensor up. */
esp_err_t cam_stream_start(int width, int height, int quality);
esp_err_t cam_stream_start_yuv(int width, int height); /* YUV422 frames for the live view encoder */
esp_err_t cam_stream_start_live_jpeg(int width, int height, int quality); /* JPEG for the live view data channel */
void cam_stream_stop(void);
void cam_dma_reserve_init(void);                 /* at boot: the static 16 KB DMA buffer to the camera driver (cam.c) */

/* ------------------------------------------------------------------ FlyBot device API (flybot.c, CONTRACTS section 2) */

typedef struct {
	char key[80];        /* snapshotId (also used as the legacy image.key) */
	int w, h;
	size_t bytes;
	time_t next_wake_at; /* nextWakeAt hint, 0 when absent */
	char error[64];
	char sd_file[32];    /* "YYYYMMDD/HHMMSSx.JPG" when the photo was also saved to the microSD card */
} upload_t;

/* POST /api/device/snapshot: multipart image + meta. trigger: "schedule" | "rpc" | "boot". Retries with back-off. */
esp_err_t flybot_snapshot(const camera_fb_t *fb, const char *trigger, upload_t *out);
/* POST /api/device/pair with the stored serial, verify code and provisioning token; stores the result. */
esp_err_t flybot_pair(char *err, size_t errn);
/* GET /api/device/time -> sets the clock. */
esp_err_t flybot_time(bool set_clock);          /* also stores timezone / POSIX / tzSource / publicIp */
#include "esp_http_client.h"
int flybot_request(const char *path, esp_http_client_method_t method, const char *body, char *resp, size_t respn,
				   int timeout_ms);               /* JSON to {apiBase}{path}; HTTP status or -1 */
extern char g_public_ip[48];                    /* publicIp from /api/device/time, "" unknown */

/* ------------------------------------------------------------------ time and timezone (tz.c, net.c) */

/* Effective timezone: manual override (portal/console) > shadow `timezone` > /api/device/time > build default */
void tz_auto_set(const char *iana, const char *posix, const char *source); /* from /api/device/time, persisted */
void tz_apply(void);                            /* recompute + setenv("TZ") + tzset(); logs the choice */
const char *tz_effective_iana(void);
const char *tz_effective_source(void);          /* "manual" | "shadow" | "farm" | "gps" | "ip" | "default" | "build" */
const char *clock_source(void);                 /* "sntp" | "modem" | "gps" | "flybot" | "rtc" | "none" */
void clock_set_source(const char *src);
bool store_overridden(const char *key);         /* set in the runtime NVS (not a default) */
bool roi_parse(const char *s, int out[4]);      /* "x,y,w,h" percent (motionRoi) */
bool capture_size_parse(const char *s, int *w, int *h); /* "max" (0x0) or "WxH" of a photo size (cam_size_ok) */
void nvs_tz_read(char *iana, size_t in, char *posix, size_t pn, char *src, size_t sn); /* store.c */
void nvs_tz_write(const char *iana, const char *posix, const char *src);
bool shadow_tz(char *iana, size_t in, char *posix, size_t pn); /* the shadow set `timezone` (shadow.c) */
/* GET /api/device/ota -> fw, url, sha256; false when 204 / no update */
bool flybot_ota_check(char *fw, size_t fwn, char *url, size_t urln, char *sha256, size_t shan);
/* the `meta` JSON of a snapshot (also shown by the console) */
cJSON *flybot_meta_json(const char *trigger, time_t captured_at);

/* quality.c: the photo's quality, measured before the upload (FlyBot's imageQuality.js numbers) */
typedef struct {
	float brightness, contrast, sharpness, edge_ratio, clipped;
	int change;    /* per mille of the thumbnail changed since the last photo, -1 = no earlier photo */
	int ms, decode_ms;
} photo_quality_t;
bool photo_quality(const uint8_t *jpg, size_t len, int w, photo_quality_t *q);
void photo_quality_json(cJSON *parent, const photo_quality_t *q); /* adds quality {...} */

/* ------------------------------------------------------------------ OTA (ota.c) */

/* Downloads url into the other OTA slot, checks the SHA-256 (hex) before it is made bootable. Restarts on success. */
typedef void (*ota_progress_cb)(int pct, void *ctx); /* 25, 50, 75 while the image downloads */
esp_err_t ota_apply(const char *url, const char *sha256_hex, char *err, size_t errn, ota_progress_cb progress, void *ctx);
bool ota_running(void);            /* an image is downloading (motion detection pauses, no live view) */

/* ------------------------------------------------------------------ MQTT agent (proto.c) */

void proto_start(void);
void proto_fast_retry(int seconds);             /* short reconnect back-off for a while (a fresh certificate) */
bool proto_connected(void);
void proto_publish_event(cJSON *evt);           /* takes ownership */
void proto_publish_low_power(int sleep_s);      /* low_power: voltage, percent, charging, critical (+ sleepS before a protective sleep) */
void proto_publish_state(bool retained_online);
void proto_publish_reported(const char *json);
bool proto_publish_log(const char *json);       /* camos/<serial>/log, QoS 0, only while connected */
void proto_publish_reported_now(void);          /* after a local change (location, power mode, pairing) */
bool proto_flush(int timeout_ms);               /* wait until the outbox is empty */
void proto_goodbye(uint64_t wake_at_ms);        /* retained {"v":1,"online":false,"sleeping":true,"wakeAt"}, before deep sleep */
bool proto_provisioned(void);
int proto_connected_for_s(void);                /* seconds since CONNECTED, -1 when not connected */
bool proto_idle(void);                          /* no RPC queued or running */
cJSON *evt_new(const char *type, char id_out[27]); /* event with v, id, ts, clock, type */

/* ------------------------------------------------------------------ capture and schedule (sched.c) */

/* Capture + upload under the camera lock. trigger as for flybot_snapshot. */
bool capture_busy(void);                        /* a capture + upload runs (it fills the uplink) */
esp_err_t capture_now(const char *trigger, upload_t *out);
void sched_start(void);
void sched_init_lock(void);                     /* the camera lock alone (portal mode console) */
void sched_changed(void);                       /* settings changed: recompute the next capture */
bool sched_run(void (*fn)(void));              /* run fn on the scheduler's task (12 KB stack) soon; false if busy */
bool sched_job_pending(void);                  /* a sched_run request waits for the scheduler's task */
bool sched_checkin(void);                      /* this wake is a check-in (battery mode on Wi-Fi): requests only, no 4G */
void sched_keep_awake(int seconds);             /* live view / console: no deep sleep for this long */
time_t sched_next_capture(void);                /* 0 when none */
void sched_wake_hint(time_t at);                /* nextWakeAt from the FlyBot snapshot response */
cJSON *sched_json(int n_next);                  /* {source, items[], next[]} for the console's Schedule page */

/* ------------------------------------------------------------------ battery and power (battery.c, power.c) */

bool battery_sample(void);                      /* read the ADCs now (ESP32 ADC2: only while Wi-Fi is off) */
int battery_mv(void);                           /* last cell voltage in mV, -1 unknown or no monitor */
int battery_pct(void);
int solar_mv(void);                             /* solar / charger input in mV, -1 unknown */
bool battery_charging(void);                    /* charger status pin or solar input present */
bool battery_charging_fast(void);               /* charging, the input at full voltage (not a sagging panel) */
const char *battery_charge_name(void);          /* "yes", "slow", "no" or "unknown" */
bool battery_low_due(void);                     /* true once per 6 h while below CAMOS_BATT_LOW_MV */
bool battery_critical(void);
bool battery_protect(void);                     /* critical and not charging: no photos, sleep an hour at a time */
bool battery_protect_notice_due(void);          /* once per spell of battery_protect: the warning before its first sleep */
void battery_json(cJSON *parent);               /* adds battery {voltage, percent, mv, pct, charging, charge, solarMv} */

power_mode_t power_effective(void);             /* auto resolved by battery voltage with hysteresis */
const char *power_mode_name(power_mode_t m);
bool power_mode_parse(const char *s, power_mode_t *out);
bool power_may_deep_sleep(void);                /* effective mode is battery (and deep sleep is built in) */
bool power_eco(void);                           /* online on a cell not full and not charging at full input */
const char *power_level_name(void);             /* "full", "eco" (power_eco) or "saver" (battery mode) */
void power_json(cJSON *parent);                 /* power {mode, effective, level, highMv, lowMv, nightSleep, nightWindow} */

/* ------------------------------------------------------------------ GPS (gps.c) */

typedef struct {
	double lat, lng;
	int sats;
	time_t fix_at;      /* 0 = no fix yet */
	int acc_m;          /* accuracy radius in metres, 0 unknown */
	bool cell;          /* from the 4G cell (AT+CLBS), not the GPS: a few hundred metres to a few km */
} gps_fix_t;

bool gps_available(void);
bool gps_fix(int timeout_s, gps_fix_t *out);    /* power the module, wait for RMC valid (+GGA), power it down
                                                 * (no fix: kept on for 10 min, the next try starts warm) */
void gps_sleep_prepare(void);                   /* before deep sleep: the module off */
bool gps_fix_yielding(int timeout_s);           /* gps_fix that stops when a sched_run request comes (the daily fix) */
bool gps_quiet(void);                           /* console `gps quiet`: camera, Wi-Fi and 4G stay off for the fix */
void gps_set_quiet(bool on);
bool gps_last(gps_fix_t *out);                  /* last fix (RTC memory / NVS) */
bool gps_fix_due(void);                         /* no fix in CAMOS_GPS_FIX_INTERVAL_H */
void gps_json(cJSON *parent);                   /* location {lat, lng, fixAt} (CONTRACTS) */
void gps_state_json(cJSON *parent);             /* gps {lat, lon, fix, ts} (protocol 4) */

/* ------------------------------------------------------------------ live view (live.c, ESP32-S3 + CAMOS_LIVE) */

void live_init(void);                           /* boot: the KVS task (powered mode joins the channel on its own) */
bool live_supported(void);                      /* built in, paired (kvs channel + region) and with a credential source */
esp_err_t live_start(int minutes, char *err, size_t errn);
void live_stop(void);
void live_stack_lend(void);                     /* the encoder's idle 16 KB stack to the camera's JPEG DMA buffer */
void live_stack_reserve(void);                  /* and back, once the camera stopped */
bool live_active(void);                         /* a live.start / live.requested window is open */
bool live_streaming(void);                      /* a VIEWER is connected: the camera belongs to the encoder */
void peer_udp_set_wait(int ms);                 /* how long esp_peer's UDP sends wait for a full Wi-Fi TX queue; 0: its own 40 ms */
int live_pause(int timeout_ms);                 /* a capture needs the camera: 1 paused, 0 no live stream, -1 timeout */
void live_resume(void);                         /* after live_pause: the stream starts again in its mode */
int live_remaining_s(void);
void live_status_json(cJSON *o);                /* console / web console: state, viewer, fps, bitrate, errors */

/* ------------------------------------------------------------------ status LED (led.c, docs/LED.md) */

typedef enum { LED_OFF = 0, LED_PORTAL, LED_CONNECTING, LED_ONLINE, LED_ERROR, LED_OTA } led_state_t;
typedef enum { LED_FLASH_NONE = 0, LED_FLASH_CAPTURE, LED_FLASH_ERROR, LED_FLASH_LOWBATT } led_flash_t;
void led_init(void);
void led_set(led_state_t s);                    /* base state, logged on change */
led_state_t led_get(void);
const char *led_state_name(led_state_t s);
void led_flash(led_flash_t f);                  /* one-shot overlay, then back to the base state */
void led_enable(bool on);                       /* persisted in NVS "led" */
bool led_enabled(void);
void led_sleep_prepare(bool low_battery);       /* blocking low-battery blink when asked, then dark + pin held */

/* ------------------------------------------------------------------ Wi-Fi scan (wifi.c) */

typedef struct {
	char ssid[33];
	int8_t rssi;
	uint8_t channel;
	bool secure;
} wifi_net_t;
#define WIFI_SCAN_MAX 24
/* Active scan that keeps a SoftAP usable (short per-channel dwell, back on the home channel between channels).
 * Deduplicated by SSID (strongest kept), hidden networks dropped, strongest first. Returns the count, -1 on error. */
int wifi_scan(wifi_net_t *out, int max);
cJSON *wifi_scan_json(const wifi_net_t *nets, int n); /* [{ssid, rssi, secure, channel}] */

/* ------------------------------------------------------------------ web (web.c): shared HTTP helpers, local console */

#include "esp_http_server.h"
esp_err_t web_send_json(httpd_req_t *req, cJSON *o);         /* takes ownership */
esp_err_t web_send_ok(httpd_req_t *req);
esp_err_t web_logo_get(httpd_req_t *req);                   /* GET /logo.png (console and portal) */
esp_err_t web_send_error(httpd_req_t *req, const char *status, const char *code, const char *message);
cJSON *web_read_json(httpd_req_t *req, size_t max);
esp_err_t web_config_post(httpd_req_t *req);                  /* POST /api/config (portal and console) */
esp_err_t web_cert_post(httpd_req_t *req);                    /* POST /api/cert */
esp_err_t web_scan_get(httpd_req_t *req);                     /* GET /api/wifi/scan[?refresh=1] */
void web_scan_cache_set(const wifi_net_t *nets, int n);
void web_start(void);                                         /* station-mode console (CAMOS_WEB_CONSOLE) */
void web_stop(void);                                          /* 4G took over: the console server goes */
esp_err_t web_qr_get(httpd_req_t *req);                       /* GET /api/qr.svg?what=claim|label (qr.c) */
char *qr_svg(const char *text);                               /* caller frees */
void qr_print(const char *text);                              /* to the serial console */

/* ------------------------------------------------------------------ self-registration (register.c, CONTRACTS 2a) */

void register_start(void);              /* no certificate yet: key + CSR + POST /api/device/register, with back-off */
bool register_pending(void);
void register_check_identity(const char *why); /* a 401 or refused IoT logins: is the camera still known to FlyBot? */
char *register_csr_pem(void);           /* the CSR it would send (console `csr`), caller frees */
/* Parses a register response (201/200 JSON) into the configuration; pure apart from store_*; false + err on a bad one */
bool register_apply_response(const char *json, char *err, size_t errn);
void log_capture_start(void);                                 /* ring buffer of the log for /api/logs */
uint32_t log_ring_total(void);                                /* bytes ever written to the log ring */
/* Copy from position since (at most outn bytes); *lost = bytes overwritten before they were read (the copy then
 * starts at since + *lost). Returns the bytes copied. */
uint32_t log_ring_copy(uint32_t since, char *out, size_t outn, uint32_t *lost);
const char *log_crash_tail(void);                             /* the previous boot's last log lines (RTC), or NULL */
void log_crash_tail_free(void);

/* ------------------------------------------------------------------ remote log (logfwd.c) */

void logfwd_init(void);                         /* task; the crash report of the previous boot */
esp_err_t logfwd_start(int level, int minutes, bool backlog, char *err, size_t errn); /* level 1..5, minutes 1..60 */
void logfwd_stop(const char *why);
bool logfwd_on(void);
void logfwd_connected(void);                    /* MQTT is up: a pending crash report goes out */
void logfwd_desired(const cJSON *item, cJSON *errors); /* shadow log {level, until} */
void logfwd_json(cJSON *parent);                /* reported log {on, level, until, sent, dropped} */
int64_t logfwd_until_epoch(void);               /* end of the session (epoch s), 0 when off */

/* ------------------------------------------------------------------ camera preview (sched.c, shares the camera lock) */

esp_err_t preview_frame(int w, int h, int quality, camera_fb_t **fb);
void preview_release(camera_fb_t *fb);
void preview_stop(void);
bool preview_active(void);
esp_err_t preview_frame_any(camera_fb_t **fb, int *w, int *h); /* the running preview size, else QVGA (motion) */
void camera_settings_changed(void);             /* re-apply the image settings when the camera is running */
bool camera_take(int timeout_ms);               /* the camera lock for the live view encoder (stops the preview) */
void camera_give(void);

/* ------------------------------------------------------------------ MJPEG live view (stream.c) */

typedef struct {
	int w, h, quality;
	uint32_t frames, avg_bytes;
	uint64_t bytes;
	float fps;
} stream_stats_t;
esp_err_t web_stream_get(httpd_req_t *req);       /* GET /api/stream (after the session check) */
void stream_get_stats(stream_stats_t *out);
void stream_stop(void);
bool stream_active(void);
bool frame_size_ok(int w, int h);

/* ------------------------------------------------------------------ microSD card (sd.c) */

bool sd_mount(void);                            /* at boot; false = no card (or no SD support in this build) */
void sd_unmount(void);
bool sd_present(void);
const char *sd_card_name(void);
bool sd_usage(uint64_t *total, uint64_t *free_bytes);
/* /sdcard/DCIM/YYYYMMDD/HHMMSSx.JPG; name_out = "YYYYMMDD/HHMMSSx.JPG" (the caller checks sdSave / motionSd) */
esp_err_t sd_save_jpeg(const uint8_t *buf, size_t len, const char *trigger, char *name_out, size_t name_n);
cJSON *sd_list_days(void);                      /* [{day, files}] */
cJSON *sd_list_files(const char *day);          /* [{name, bytes, trigger}] */
esp_err_t sd_serve(httpd_req_t *req, const char *day, const char *file, bool download);
esp_err_t sd_delete(const char *day, const char *file); /* file NULL or "": the whole day */
esp_err_t sd_format(void);
void *sd_usb_lend(void);                        /* the card (sdmmc_card_t *) for USB mass storage, unmounted here */
void sd_usb_return(void);                       /* the computer let go: mount it again */
void sd_stick_mounted(const char *mount);       /* a USB stick replaces the card as the photo storage */
void sd_stick_gone(void);                       /* back to the card */

/* ------------------------------------------------------------------ native USB port (usbdev.c) */

void usbdev_start(void);                        /* at boot, per usbMode */
void usbdev_json(cJSON *parent);                /* status "usb": {mode, setting, connected, cardLent, streaming} */
bool usb_mode_valid(const char *s);             /* "auto", "storage", "webcam" or "off" */
/* usbhost.c: what is on the other end of the native port, read from the D+ line while no USB driver has the port */
typedef enum { USB_PEER_UNKNOWN = -1, USB_PEER_NONE, USB_PEER_STICK, USB_PEER_COMPUTER } usb_peer_t;
usb_peer_t usbhost_sense(void);
bool usbhost_stick_on_line(void);               /* device mode, controller disconnected: a stick pulls D+ up */
bool usbhost_computer_on_line(void);            /* device mode, suspended: a computer still holds D- down */
void usbhost_start(void);                       /* the port becomes a USB host for the stick (photo storage) */
bool usbhost_stick_present(void);
uint64_t usbhost_stick_bytes(void);

/* ------------------------------------------------------------------ motion detection (motion.c) */

void motion_start(void);                        /* task; runs while enabled and the effective mode is powered */
void motion_config_changed(void);
void motion_json(cJSON *parent);                /* motion {enabled, running, active, lastAt, count, level} */

/* ------------------------------------------------------------------ recent events for the console (events.c) */

void events_note(const char *type, const char *text); /* also called by proto_publish_event */
cJSON *events_json(void);                       /* newest first */

/* ------------------------------------------------------------------ serial console (console.c) */

void console_start(void);

/* Kconfig bools are undefined when off: give C code 0/1 values */
#ifdef CONFIG_CAMOS_DEEP_SLEEP
#define CAMOS_DEEP_SLEEP 1
#else
#define CAMOS_DEEP_SLEEP 0
#endif
#ifdef CONFIG_CAMOS_CAM_VFLIP
#define CAMOS_VFLIP 1
#else
#define CAMOS_VFLIP 0
#endif
#ifdef CONFIG_CAMOS_CAM_HMIRROR
#define CAMOS_HMIRROR 1
#else
#define CAMOS_HMIRROR 0
#endif
#ifdef CONFIG_CAMOS_CAM_POWER_ACTIVE_LOW
#define CAMOS_POWER_ACTIVE_LOW 1
#else
#define CAMOS_POWER_ACTIVE_LOW 0
#endif
#ifdef CONFIG_CAMOS_MODEM
#define CAMOS_MODEM 1
#else
#define CAMOS_MODEM 0
#endif
#ifdef CONFIG_CAMOS_GPS
#define CAMOS_GPS 1
#else
#define CAMOS_GPS 0
#endif
#ifdef CONFIG_CAMOS_LIVE
#define CAMOS_LIVE 1
#else
#define CAMOS_LIVE 0
#endif

/* Options that only exist when their feature is built: give the rest of the code sane values */
#ifndef CONFIG_CAMOS_GPS_FIX_TIMEOUT_S
#define CONFIG_CAMOS_GPS_FIX_TIMEOUT_S 120
#endif
#ifndef CONFIG_CAMOS_GPS_FIX_INTERVAL_H
#define CONFIG_CAMOS_GPS_FIX_INTERVAL_H 24
#endif
#ifndef CONFIG_CAMOS_MODEM_TX_GPIO
#define CONFIG_CAMOS_MODEM_TX_GPIO -1
#endif
#ifndef CONFIG_CAMOS_MODEM_RX_GPIO
#define CONFIG_CAMOS_MODEM_RX_GPIO -1
#endif
