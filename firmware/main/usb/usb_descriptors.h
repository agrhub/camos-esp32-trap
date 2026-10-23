/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2020 Jerzy Kasenbreg
 * Copyright (c) 2021 Koji KITAYAMA
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
 *
 */
/* Trimmed from espressif/usb_device_uvc 1.3.1 (tusb/usb_descriptors.h) for the CamOS trap firmware: one MJPEG camera,
 * isochronous, six frame sizes (usb_frames.h; UVC_FRAME_NUM must match the frame lines of the macro below).
 * Interface and endpoint numbers per USB mode are in usb/usbdev_desc.h. */
#pragma once
#include "usb_frames.h"

/* Time stamp base clock. It is a deprecated parameter. */
#define UVC_CLOCK_FREQUENCY    27000000
/* video capture path */
#define UVC_ENTITY_CAP_INPUT_TERMINAL  0x01
#define UVC_ENTITY_CAP_OUTPUT_TERMINAL 0x02

#define TUD_VIDEO_CAPTURE_DESC_MULTI_MJPEG_LEN(n) (\
    TUD_VIDEO_DESC_IAD_LEN\
    /* control */\
    + TUD_VIDEO_DESC_STD_VC_LEN\
    + (TUD_VIDEO_DESC_CS_VC_LEN + 1/*bInCollection*/)\
    + TUD_VIDEO_DESC_CAMERA_TERM_LEN\
    + TUD_VIDEO_DESC_OUTPUT_TERM_LEN\
    /* Interface 1, Alternate 0 */\
    + TUD_VIDEO_DESC_STD_VS_LEN\
    + (TUD_VIDEO_DESC_CS_VS_IN_LEN + 1/*bNumFormats x bControlSize*/)\
    + TUD_VIDEO_DESC_CS_VS_FMT_MJPEG_LEN\
    + (n*TUD_VIDEO_DESC_CS_VS_FRM_MJPEG_CONT_LEN)\
    + TUD_VIDEO_DESC_CS_VS_COLOR_MATCHING_LEN\
    /* Interface 1, Alternate 1 */\
    + TUD_VIDEO_DESC_STD_VS_LEN\
    + 7/* Endpoint */\
  )

#define TUD_VIDEO_DESC_CS_VS_FRM_MJPEG_CONT_TEMPLATE(bCamIndex ,bFrameIndex) \
        TUD_VIDEO_DESC_CS_VS_FRM_MJPEG_CONT(bFrameIndex, 0, UVC_FRAMES_INFO[bCamIndex][bFrameIndex-1].width, UVC_FRAMES_INFO[bCamIndex][bFrameIndex-1].height, \
            UVC_FRAMES_INFO[bCamIndex][bFrameIndex-1].width * UVC_FRAMES_INFO[bCamIndex][bFrameIndex-1].height * 16, UVC_FRAMES_INFO[bCamIndex][bFrameIndex-1].width * UVC_FRAMES_INFO[bCamIndex][bFrameIndex-1].height * 16 * UVC_FRAMES_INFO[bCamIndex][bFrameIndex-1].rate, \
            UVC_FRAMES_INFO[bCamIndex][bFrameIndex-1].width * UVC_FRAMES_INFO[bCamIndex][bFrameIndex-1].height * 16 / 8, \
            (10000000/UVC_FRAMES_INFO[bCamIndex][bFrameIndex-1].rate), (10000000/UVC_FRAMES_INFO[bCamIndex][bFrameIndex-1].rate), (10000000/UVC_FRAMES_INFO[bCamIndex][bFrameIndex-1].rate), (10000000/UVC_FRAMES_INFO[bCamIndex][bFrameIndex-1].rate))

#define TUD_VIDEO_CAPTURE_DESCRIPTOR_MULTI_MJPEG(_stridx, _itf, _epin, _epsize) \
  TUD_VIDEO_DESC_IAD(_itf, /* 2 Interfaces */ 0x02, _stridx), \
  /* Video control 0 */ \
  TUD_VIDEO_DESC_STD_VC(_itf, 0, _stridx), \
    TUD_VIDEO_DESC_CS_VC( /* UVC 1.5*/ 0x0150, \
         /* wTotalLength - bLength */ \
         TUD_VIDEO_DESC_CAMERA_TERM_LEN + TUD_VIDEO_DESC_OUTPUT_TERM_LEN, \
         UVC_CLOCK_FREQUENCY, _itf + 1), \
      TUD_VIDEO_DESC_CAMERA_TERM(UVC_ENTITY_CAP_INPUT_TERMINAL, 0, 0,\
                                 /*wObjectiveFocalLengthMin*/0, /*wObjectiveFocalLengthMax*/0,\
                                 /*wObjectiveFocalLength*/0, /*bmControls*/0), \
      TUD_VIDEO_DESC_OUTPUT_TERM(UVC_ENTITY_CAP_OUTPUT_TERMINAL, VIDEO_TT_STREAMING, 0, 1, 0), \
  /* Video stream alt. 0 */ \
  TUD_VIDEO_DESC_STD_VS(_itf + 1, 0, 0, _stridx), \
    /* Video stream header for without still image capture */ \
    TUD_VIDEO_DESC_CS_VS_INPUT( /*bNumFormats*/1, \
        /*wTotalLength - bLength */\
        TUD_VIDEO_DESC_CS_VS_FMT_MJPEG_LEN\
        + (UVC_FRAME_NUM*TUD_VIDEO_DESC_CS_VS_FRM_MJPEG_CONT_LEN)\
        + TUD_VIDEO_DESC_CS_VS_COLOR_MATCHING_LEN,\
        _epin, /*bmInfo*/0, /*bTerminalLink*/UVC_ENTITY_CAP_OUTPUT_TERMINAL, \
        /*bStillCaptureMethod*/0, /*bTriggerSupport*/0, /*bTriggerUsage*/0, \
        /*bmaControls(1)*/0), \
      /* Video stream format */ \
      TUD_VIDEO_DESC_CS_VS_FMT_MJPEG(/*bFormatIndex*/1, /*bNumFrameDescriptors*/UVC_FRAME_NUM, \
        /*bmFlags*/0, /*bDefaultFrameIndex*/1, 0, 0, 0, /*bCopyProtect*/0), \
        /* Video stream frame format */ \
        TUD_VIDEO_DESC_CS_VS_FRM_MJPEG_CONT_TEMPLATE(_itf/2, 1), \
        TUD_VIDEO_DESC_CS_VS_FRM_MJPEG_CONT_TEMPLATE(_itf/2, 2), \
        TUD_VIDEO_DESC_CS_VS_FRM_MJPEG_CONT_TEMPLATE(_itf/2, 3), \
        TUD_VIDEO_DESC_CS_VS_FRM_MJPEG_CONT_TEMPLATE(_itf/2, 4), \
        TUD_VIDEO_DESC_CS_VS_FRM_MJPEG_CONT_TEMPLATE(_itf/2, 5), \
        TUD_VIDEO_DESC_CS_VS_FRM_MJPEG_CONT_TEMPLATE(_itf/2, 6), \
        TUD_VIDEO_DESC_CS_VS_COLOR_MATCHING(VIDEO_COLOR_PRIMARIES_BT709, VIDEO_COLOR_XFER_CH_BT709, VIDEO_COLOR_COEF_SMPTE170M), \
  /* VS alt 1 */\
  TUD_VIDEO_DESC_STD_VS(_itf + 1, 1, 1, _stridx), \
    /* EP */ \
    TUD_VIDEO_DESC_EP_ISO(_epin, _epsize, 1)
