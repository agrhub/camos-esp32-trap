/* SPDX-License-Identifier: AGPL-3.0-only */
/* Interface and endpoint numbers of the three USB personalities (webcam, drive, both), and what the descriptors need from usbdev.c.
 * This header is compiled into the TinyUSB library (usb_descriptors.c) as well as into main. */
#pragma once
#include <stdbool.h>

enum { USB_ITF_VIDEO_CONTROL = 0, USB_ITF_VIDEO_STREAMING, USB_ITF_WEBCAM_TOTAL };
enum { USB_ITF_MSC = 0, USB_ITF_STORAGE_TOTAL };
enum { USB_ITF_BOTH_VIDEO_CONTROL = 0, USB_ITF_BOTH_VIDEO_STREAMING, USB_ITF_BOTH_MSC, USB_ITF_BOTH_TOTAL };

#define USB_EP_VIDEO_IN 0x81
#define USB_EP_MSC_OUT 0x01
#define USB_EP_MSC_IN 0x81
#define USB_EP_BOTH_MSC_OUT 0x02 /* the composite device: the webcam keeps 0x81 */
#define USB_EP_BOTH_MSC_IN 0x82

typedef enum { USB_DESC_WEBCAM = 0, USB_DESC_STORAGE, USB_DESC_BOTH } usb_desc_kind_t;
usb_desc_kind_t usbdev_desc_kind(void); /* which personality the port shows (usbMode at boot) */
const char *usbdev_desc_serial(void);  /* the camera serial, the USB serial number string */
