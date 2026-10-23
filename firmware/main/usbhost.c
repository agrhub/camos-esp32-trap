/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * The native USB port as a host for a USB stick (usbMode "auto", usbdev.c): the stick is mounted at /usb (FAT) and the
 * photo storage (sd.c) uses it instead of the microSD card while it is there. The stick needs 5 V on VBUS from outside
 * (an OTG cable with a power input, or a USB-A socket fed by the 5 V rail): the board's USB-C port does not supply it.
 *
 * Which role the port takes is read from the D+ line while no USB driver has the port (usbhost_sense): a device (the
 * stick) pulls D+ up through 1.5 kOhm, a host (a computer) pulls it down through 15 kOhm. The camera never runs as a
 * host with a computer on the cable: the computer shuts its port down when it sees a host driving the bus.
 * Built on espressif/usb_host_msc and the ESP-IDF USB host library.
 */
#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_rom_sys.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "camos.h"

static const char *TAG = "usb-host";

#if CONFIG_SOC_USB_OTG_SUPPORTED && CONFIG_CAMOS_USB_OTG

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_adc/adc_oneshot.h"
#include "soc/io_mux_reg.h"
#include "soc/usb_serial_jtag_reg.h"
#include "hal/usb_wrap_ll.h"
#include "hal/usb_serial_jtag_ll.h"
#include "usb/usb_host.h"
#include "usb/msc_host.h"
#include "usb/msc_host_vfs.h"

#define STICK_MOUNT "/usb"
#define DP_GPIO 20           /* the native port's D+ (ADC2 channel 9) */
#define DP_ADC ADC_CHANNEL_9
#define DM_GPIO 19           /* D- (ADC2 channel 8) */
#define DM_ADC ADC_CHANNEL_8
#define COMPUTER_MAX_MV 2000 /* a line with the pin's pull-up: about 1 V against a computer's 15 kOhm, 3.3 V open */

/* ------------------------------------------------------------------------------- who is on the other end */

/* D+ with the pin's own pull-down (about 45 kOhm): high only when a device pulls it up */
static bool dp_high_pulled_down(void)
{
	const gpio_config_t io = { .pin_bit_mask = BIT64(DP_GPIO), .mode = GPIO_MODE_INPUT, .pull_down_en = GPIO_PULLDOWN_ENABLE };
	gpio_config(&io); /* also takes the pin from the USB serial/JTAG pad and drops its D+ pull-up */
	esp_rom_delay_us(1000);
	int high = 0;
	for (int i = 0; i < 5; i++) {
		high += gpio_get_level(DP_GPIO);
		esp_rom_delay_us(200);
	}
	return high == 5;
}

/* A data line in mV with the pin's own pull-up, -1 when the ADC is busy (ADC2 is shared with Wi-Fi). The lowest of
 * 8 samples: bus traffic drives the line high part of the time, a computer's pull-down shows between packets. */
static int line_mv_pulled_up(int gpio, adc_channel_t chan)
{
	adc_oneshot_unit_handle_t adc;
	const adc_oneshot_unit_init_cfg_t uc = { .unit_id = ADC_UNIT_2 };
	if (adc_oneshot_new_unit(&uc, &adc) != ESP_OK) {
		return -1;
	}
	const adc_oneshot_chan_cfg_t cc = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12 };
	int low = 4095, n = 0;
	if (adc_oneshot_config_channel(adc, chan, &cc) == ESP_OK) {
		rtc_gpio_pulldown_dis(gpio);
		rtc_gpio_pullup_en(gpio);
		esp_rom_delay_us(1000);
		for (int i = 0; i < 16 && n < 8; i++) {
			int raw;
			if (adc_oneshot_read(adc, chan, &raw) == ESP_OK) {
				low = raw < low ? raw : low;
				n++;
			}
		}
		rtc_gpio_pullup_dis(gpio);
		rtc_gpio_deinit(gpio);
	}
	adc_oneshot_del_unit(adc);
	return n ? low * 3100 / 4095 : -1; /* uncalibrated 12 dB scale: plenty for 1 V against 3.3 V */
}

/* Call only while no USB driver has the port (before usbdev/usbhost start it, or after the host stopped). The pad is
 * put back exactly as it was. */
usb_peer_t usbhost_sense(void)
{
	const uint32_t mux = REG_READ(IO_MUX_GPIO20_REG), usj = REG_READ(USB_SERIAL_JTAG_CONF0_REG);
	usb_peer_t peer;
	int mv = -1;
	if (dp_high_pulled_down()) {
		peer = USB_PEER_STICK;
	} else {
		mv = line_mv_pulled_up(DP_GPIO, DP_ADC);
		peer = mv < 0 ? USB_PEER_UNKNOWN : mv < COMPUTER_MAX_MV ? USB_PEER_COMPUTER : USB_PEER_NONE;
	}
	REG_WRITE(IO_MUX_GPIO20_REG, mux);
	REG_WRITE(USB_SERIAL_JTAG_CONF0_REG, usj);
	ESP_LOGD(TAG, "D+ sense: peer %d (%d mV pulled up)", peer, mv);
	return peer;
}

/* Device mode, suspended (no SOF for a while): is the computer still on the cable? While the device is connected D+
 * is ours (pulled up), so D- tells: a computer holds it down through 15 kOhm, without one only the pin's pull-up is
 * there. Nothing is disconnected; D- at about 1 V with D+ high is not a resume (K) state for the computer. */
bool usbhost_computer_on_line(void)
{
	const uint32_t mux = REG_READ(IO_MUX_GPIO19_REG);
	const int mv = line_mv_pulled_up(DM_GPIO, DM_ADC);
	REG_WRITE(IO_MUX_GPIO19_REG, mux);
	ESP_LOGD(TAG, "D- sense: %d mV pulled up", mv);
	return mv < 0 || mv < COMPUTER_MAX_MV; /* ADC busy: assume it is there, read again later */
}

/* Device mode with the controller disconnected (tud_disconnect, no D+ pull-up of ours): D+ against the PHY's own
 * pull-down is high only when a stick pulls it up */
bool usbhost_stick_on_line(void)
{
	const uint32_t conf = USB_WRAP.otg_conf.val, mux = REG_READ(IO_MUX_GPIO20_REG);
	const usb_wrap_pull_override_vals_t pd = { .dp_pd = true, .dm_pd = true };
	usb_wrap_ll_phy_enable_pull_override(&USB_WRAP, &pd);
	PIN_INPUT_ENABLE(IO_MUX_GPIO20_REG);
	esp_rom_delay_us(1000);
	int high = 0;
	for (int i = 0; i < 5; i++) {
		high += gpio_get_level(DP_GPIO);
		esp_rom_delay_us(200);
	}
	REG_WRITE(IO_MUX_GPIO20_REG, mux);
	USB_WRAP.otg_conf.val = conf;
	return high == 5;
}

/* --------------------------------------------------------------------------------------------- the host */

typedef struct {
	bool connected;
	uint8_t address;
	msc_host_device_handle_t handle;
} stick_evt_t;

static QueueHandle_t events;
static SemaphoreHandle_t lib_done;
static volatile bool lib_stop;
static msc_host_device_handle_t stick;
static msc_host_vfs_handle_t stick_vfs;
static bool installed;
static uint64_t stick_bytes;

/* The USB host library's event loop; on stop it frees the devices and uninstalls the library */
static void lib_task(void *arg)
{
	bool freeing = false;
	int waited = 0;
	for (;;) {
		uint32_t flags = 0;
		usb_host_lib_handle_events(pdMS_TO_TICKS(100), &flags);
		if (lib_stop && !freeing) {
			usb_host_device_free_all();
			freeing = true;
		}
		/* uninstall only once every device is freed (NO_CLIENTS alone comes too early); give up after 3 s */
		if (freeing && ((flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) || ++waited > 30)) {
			break;
		}
	}
	esp_err_t e = usb_host_uninstall();
	ESP_LOGI(TAG, "USB host stopped (%s)", esp_err_to_name(e));
	xSemaphoreGive(lib_done);
	vTaskDelete(NULL);
}

static void msc_cb(const msc_host_event_t *e, void *arg)
{
	stick_evt_t ev = { 0 };
	if (e->event == MSC_DEVICE_CONNECTED) {
		ev.connected = true;
		ev.address = e->device.address;
	} else if (e->event == MSC_DEVICE_DISCONNECTED) {
		ev.connected = false;
		ev.handle = e->device.handle;
	} else {
		return;
	}
	xQueueSend(events, &ev, 0);
}

static bool mount_stick(uint8_t address)
{
	if (msc_host_install_device(address, &stick) != ESP_OK) {
		ESP_LOGW(TAG, "USB drive at address %u: not usable", address);
		stick = NULL;
		return false;
	}
	msc_host_device_info_t info;
	if (msc_host_get_device_info(stick, &info) == ESP_OK) {
		stick_bytes = (uint64_t)info.sector_count * info.sector_size;
	}
	const esp_vfs_fat_mount_config_t mc = { .format_if_mount_failed = false, .max_files = 4, .allocation_unit_size = 16 * 1024 };
	if (msc_host_vfs_register(stick, STICK_MOUNT, &mc, &stick_vfs) != ESP_OK) {
		ESP_LOGW(TAG, "USB drive: no FAT file system (format it as FAT32 on a computer)");
		msc_host_uninstall_device(stick);
		stick = NULL;
		return false;
	}
	ESP_LOGI(TAG, "USB drive mounted at " STICK_MOUNT " (%llu MB)", (unsigned long long)(stick_bytes >> 20));
	sd_stick_mounted(STICK_MOUNT);
	events_note("usb", "USB drive mounted: photos are saved there");
	return true;
}

static void unmount_stick(void)
{
	if (!stick) {
		return;
	}
	sd_stick_gone();
	if (stick_vfs) {
		msc_host_vfs_unregister(stick_vfs);
		stick_vfs = NULL;
	}
	msc_host_uninstall_device(stick);
	stick = NULL;
	stick_bytes = 0;
	ESP_LOGI(TAG, "USB drive removed");
	events_note("usb", "USB drive removed");
}

static esp_err_t host_up(void)
{
	if (!events) {
		events = xQueueCreate(4, sizeof(stick_evt_t));
		lib_done = xSemaphoreCreateBinary();
	}
	xQueueReset(events);
	const usb_host_config_t hc = { .skip_phy_setup = false, .intr_flags = ESP_INTR_FLAG_LEVEL1 };
	esp_err_t e = usb_host_install(&hc);
	if (e != ESP_OK) {
		return e;
	}
	lib_stop = false;
	xTaskCreate(lib_task, "usb-lib", 4096, NULL, 5, NULL);
	const msc_host_driver_config_t mc = { .create_backround_task = true, .task_priority = 5, .stack_size = 4096,
										  .core_id = tskNO_AFFINITY, .callback = msc_cb };
	e = msc_host_install(&mc);
	if (e != ESP_OK) {
		lib_stop = true;
		xSemaphoreTake(lib_done, portMAX_DELAY);
		return e;
	}
	installed = true;
	return ESP_OK;
}

static void host_down(void)
{
	if (!installed) {
		return;
	}
	unmount_stick();
	msc_host_uninstall();
	lib_stop = true;
	if (xSemaphoreTake(lib_done, pdMS_TO_TICKS(5000)) != pdTRUE) {
		ESP_LOGW(TAG, "USB host did not stop");
	}
	installed = false;
	/* The stopped host only gates the controller's clock: the PHY stays with it and keeps the host's 15 kOhm
	 * pull-downs, which read like a computer on the line. Give the PHY back to the USB serial/JTAG controller, with
	 * its D+ pull-up off, as at boot; the next host_up takes it again. */
	usb_serial_jtag_ll_phy_enable_external(false);
	SET_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_PAD_PULL_OVERRIDE);
	CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_DP_PULLUP);
}

/* The port while the camera is a host. A stick pulled out: the host stops and D+ is read every second, so the stick
 * plugged back in starts the host again, and a computer plugged in restarts the camera as a drive + webcam for it.
 * A stick that is not mounted 10 s after the host started (some drives fail their first enumeration right after
 * power-up, "Dev 0 EP 0 Error", and the host library does not try them again): the host stops and starts again. */
#define MOUNT_WAIT_MS 10000

static void watch_task(void *arg)
{
	TickType_t started = xTaskGetTickCount();
	stick_evt_t ev;
	for (;;) {
		if (!installed) {
			usb_peer_t peer = usbhost_sense();
			if (peer == USB_PEER_STICK) {
				if (host_up() != ESP_OK) {
					ESP_LOGW(TAG, "USB host start failed");
					vTaskDelay(pdMS_TO_TICKS(5000));
				}
				started = xTaskGetTickCount();
			} else if (peer == USB_PEER_COMPUTER) {
				ESP_LOGI(TAG, "a computer on the native USB port: restarting as a drive + webcam for it");
				events_note("usb", "Computer on the USB port: restarting as a drive + webcam");
				vTaskDelay(pdMS_TO_TICKS(500));
				esp_restart();
			} else {
				vTaskDelay(pdMS_TO_TICKS(1000));
			}
			continue;
		}
		if (xQueueReceive(events, &ev, pdMS_TO_TICKS(1000)) == pdTRUE) {
			if (ev.connected && !stick) {
				mount_stick(ev.address);
			} else if (!ev.connected && stick) {
				host_down(); /* unmounts it */
			}
			continue;
		}
		if (!stick && xTaskGetTickCount() - started >= pdMS_TO_TICKS(MOUNT_WAIT_MS)) {
			host_down();
		}
	}
}

/* A stick was sensed on the line (usbdev_start): the port becomes its host from now on */
void usbhost_start(void)
{
	if (host_up() != ESP_OK) {
		ESP_LOGW(TAG, "USB host start failed");
	}
	/* its stack in PSRAM: the host and its drive leave little internal RAM for the camera's DMA block */
	xTaskCreateWithCaps(watch_task, "usb-stick", 4096, NULL, 4, NULL, MALLOC_CAP_SPIRAM);
}

bool usbhost_stick_present(void)
{
	return stick != NULL;
}

uint64_t usbhost_stick_bytes(void)
{
	return stick_bytes;
}

#else

usb_peer_t usbhost_sense(void)
{
	return USB_PEER_UNKNOWN;
}

bool usbhost_stick_on_line(void)
{
	return false;
}

bool usbhost_computer_on_line(void)
{
	return true;
}

void usbhost_start(void)
{
}

bool usbhost_stick_present(void)
{
	return false;
}

uint64_t usbhost_stick_bytes(void)
{
	return 0;
}

#endif
