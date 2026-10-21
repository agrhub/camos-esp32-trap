/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2019 Ha Thach (tinyusb.org)
 * Copyright (c) 2023 Espressif
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
/* Adapted from espressif/usb_device_uvc 1.3.1 (tusb/usb_descriptors.c) for the CamOS trap firmware: two device
 * personalities on the native USB port, chosen at boot (usbMode, usbdev.c): a UVC webcam, a mass storage drive (the
 * microSD card), or both in one composite device (usbMode auto, the default). Each has its own product id so the host
 * keeps their drivers apart. */
#include <string.h>
#include "tusb.h"
#include "usb_descriptors.h"
#include "usbdev_desc.h"

#define USB_VID 0x303A          /* Espressif */
#define USB_PID_WEBCAM 0x8D21   /* development ids under the Espressif VID; a product needs its own allocation */
#define USB_PID_STORAGE 0x8D22
#define USB_PID_BOTH 0x8D23

/* -------------------------------------------------------------------------------------------- device descriptors */

static const tusb_desc_device_t desc_device_webcam = {
	.bLength = sizeof(tusb_desc_device_t),
	.bDescriptorType = TUSB_DESC_DEVICE,
	.bcdUSB = 0x0200,
	/* Interface Association Descriptor for video: the device class must be "miscellaneous / common / IAD" */
	.bDeviceClass = TUSB_CLASS_MISC,
	.bDeviceSubClass = MISC_SUBCLASS_COMMON,
	.bDeviceProtocol = MISC_PROTOCOL_IAD,
	.bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
	.idVendor = USB_VID,
	.idProduct = USB_PID_WEBCAM,
	.bcdDevice = 0x0100,
	.iManufacturer = 0x01,
	.iProduct = 0x02,
	.iSerialNumber = 0x03,
	.bNumConfigurations = 0x01,
};

static const tusb_desc_device_t desc_device_storage = {
	.bLength = sizeof(tusb_desc_device_t),
	.bDescriptorType = TUSB_DESC_DEVICE,
	.bcdUSB = 0x0200,
	.bDeviceClass = 0x00, /* defined per interface */
	.bDeviceSubClass = 0x00,
	.bDeviceProtocol = 0x00,
	.bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
	.idVendor = USB_VID,
	.idProduct = USB_PID_STORAGE,
	.bcdDevice = 0x0100,
	.iManufacturer = 0x01,
	.iProduct = 0x02,
	.iSerialNumber = 0x03,
	.bNumConfigurations = 0x01,
};

static const tusb_desc_device_t desc_device_both = {
	.bLength = sizeof(tusb_desc_device_t),
	.bDescriptorType = TUSB_DESC_DEVICE,
	.bcdUSB = 0x0200,
	.bDeviceClass = TUSB_CLASS_MISC, /* IAD for the video function */
	.bDeviceSubClass = MISC_SUBCLASS_COMMON,
	.bDeviceProtocol = MISC_PROTOCOL_IAD,
	.bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
	.idVendor = USB_VID,
	.idProduct = USB_PID_BOTH,
	.bcdDevice = 0x0100,
	.iManufacturer = 0x01,
	.iProduct = 0x02,
	.iSerialNumber = 0x03,
	.bNumConfigurations = 0x01,
};

uint8_t const *tud_descriptor_device_cb(void)
{
	switch (usbdev_desc_kind()) {
	case USB_DESC_STORAGE: return (uint8_t const *)&desc_device_storage;
	case USB_DESC_BOTH: return (uint8_t const *)&desc_device_both;
	default: return (uint8_t const *)&desc_device_webcam;
	}
}

/* ------------------------------------------------------------------------------------ configuration descriptors */

#define WEBCAM_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_VIDEO_CAPTURE_DESC_MULTI_MJPEG_LEN(UVC_FRAME_NUM))
static const uint8_t desc_cfg_webcam[] = {
	TUD_CONFIG_DESCRIPTOR(1, USB_ITF_WEBCAM_TOTAL, 0, WEBCAM_TOTAL_LEN, 0, 500),
	TUD_VIDEO_CAPTURE_DESCRIPTOR_MULTI_MJPEG(4, USB_ITF_VIDEO_CONTROL, USB_EP_VIDEO_IN, CFG_TUD_VIDEO_STREAMING_EP_BUFSIZE),
};

#define STORAGE_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)
static const uint8_t desc_cfg_storage[] = {
	TUD_CONFIG_DESCRIPTOR(1, USB_ITF_STORAGE_TOTAL, 0, STORAGE_TOTAL_LEN, 0, 500),
	TUD_MSC_DESCRIPTOR(USB_ITF_MSC, 5, USB_EP_MSC_OUT, USB_EP_MSC_IN, 64),
};

#define BOTH_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_VIDEO_CAPTURE_DESC_MULTI_MJPEG_LEN(UVC_FRAME_NUM) + TUD_MSC_DESC_LEN)
static const uint8_t desc_cfg_both[] = {
	TUD_CONFIG_DESCRIPTOR(1, USB_ITF_BOTH_TOTAL, 0, BOTH_TOTAL_LEN, 0, 500),
	TUD_VIDEO_CAPTURE_DESCRIPTOR_MULTI_MJPEG(4, USB_ITF_BOTH_VIDEO_CONTROL, USB_EP_VIDEO_IN, CFG_TUD_VIDEO_STREAMING_EP_BUFSIZE),
	TUD_MSC_DESCRIPTOR(USB_ITF_BOTH_MSC, 5, USB_EP_BOTH_MSC_OUT, USB_EP_BOTH_MSC_IN, 64),
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
	(void)index;
	switch (usbdev_desc_kind()) {
	case USB_DESC_STORAGE: return desc_cfg_storage;
	case USB_DESC_BOTH: return desc_cfg_both;
	default: return desc_cfg_webcam;
	}
}

/* ------------------------------------------------------------------------------------------- string descriptors */

static uint16_t desc_str[40];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
	(void)langid;
	const char *str;
	switch (index) {
	case 0:
		desc_str[1] = 0x0409; /* English */
		desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | 4);
		return desc_str;
	case 1: str = "AgriHub"; break;
	case 2: str = usbdev_desc_kind() == USB_DESC_STORAGE ? "FlyBot trap camera card" : "FlyBot trap camera"; break;
	case 3: str = usbdev_desc_serial(); break;
	case 4: str = "FlyBot trap camera"; break;
	case 5: str = "microSD card"; break;
	default: return NULL; /* includes 0xEE, the Microsoft OS 1.0 string */
	}
	size_t n = strlen(str);
	if (n > 39) {
		n = 39;
	}
	for (size_t i = 0; i < n; i++) {
		desc_str[1 + i] = (uint8_t)str[i];
	}
	desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * n + 2));
	return desc_str;
}
