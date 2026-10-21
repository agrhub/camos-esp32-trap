/* SPDX-License-Identifier: AGPL-3.0-only */
/* TinyUSB configuration for the native USB port (usbdev.c): device mode, full speed, one UVC camera and one mass
 * storage interface. Only one of them is in the configuration descriptor at a time (USB mode "webcam" or "storage").
 * CFG_TUSB_MCU comes from the espressif/tinyusb component's compiler flags. */
#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be defined
#endif

#define CFG_TUSB_RHPORT0_MODE (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)
#define CFG_TUSB_OS OPT_OS_FREERTOS
#define CFG_TUSB_OS_INC_PATH freertos/
#define CFG_TUSB_DEBUG 0
#define CFG_TUD_ENABLED 1

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif
#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN __attribute__((aligned(4)))
#endif

#define CFG_TUD_ENDPOINT0_SIZE 64

/* webcam: one video control + one video streaming interface, isochronous */
#define CFG_TUD_VIDEO 1
#define CFG_TUD_VIDEO_STREAMING 1
#define CFG_TUD_VIDEO_STREAMING_EP_BUFSIZE 512
#define CFG_TUD_VIDEO_STREAMING_BULK 0

/* storage: SCSI over bulk-only transport; the buffer is a whole number of 512-byte sectors so read10/write10 never
 * see a partial sector */
#define CFG_TUD_MSC 1
#define CFG_TUD_MSC_EP_BUFSIZE 4096

#define CFG_TUD_CDC 0
#define CFG_TUD_HID 0
#define CFG_TUD_MIDI 0
#define CFG_TUD_VENDOR 0

#ifdef __cplusplus
}
#endif

#endif /* _TUSB_CONFIG_H_ */
