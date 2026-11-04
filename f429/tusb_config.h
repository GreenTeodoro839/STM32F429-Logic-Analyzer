#pragma once

#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be defined (by CMake)
#endif

#define CFG_TUSB_OS    OPT_OS_NONE
#define CFG_TUSB_DEBUG 0

// OTG_FS (PA11/PA12), internal full-speed PHY. VBUS (PA9) is not wired to the
// MCU on this board, so VBUS sensing stays off (B-session valid is forced).
#define CFG_TUD_ENABLED        1
#define CFG_TUD_MAX_SPEED      OPT_MODE_FULL_SPEED
#define CFG_TUSB_RHPORT0_MODE  (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)
#define CFG_TUD_VBUS_DETECT_HW 0

#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN __attribute__((aligned(4)))

#define CFG_TUD_ENDPOINT0_SIZE 64

#define CFG_TUD_CDC    1
#define CFG_TUD_MSC    0
#define CFG_TUD_HID    0
#define CFG_TUD_MIDI   0
#define CFG_TUD_VENDOR 0

#define CFG_TUD_CDC_RX_BUFSIZE 512
#define CFG_TUD_CDC_RX_EPSIZE  64
#define CFG_TUD_CDC_TX_BUFSIZE 8192
#define CFG_TUD_CDC_TX_EPSIZE  1024  // multi-packet IN transfers for upload/streaming throughput
// Never drop upload data, even if the host did not assert DTR
#define CFG_TUD_CDC_TX_OVERWRITABLE_IF_NOT_CONNECTED 0
