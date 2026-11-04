#include <string.h>

#include "board.h"
#include "tusb.h"

// pid.codes test VID/PID (for private / development use only)
#define USB_VID 0x1209
#define USB_PID 0x0001

static const tusb_desc_device_t desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,  // IAD for CDC
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USB_VID,
    .idProduct = USB_PID,
    .bcdDevice = 0x0001,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

const uint8_t *tud_descriptor_device_cb(void) { return (const uint8_t *)&desc_device; }

enum { ITF_NUM_CDC = 0, ITF_NUM_CDC_DATA, ITF_NUM_TOTAL };

#define EPNUM_CDC_NOTIF  0x81
#define EPNUM_CDC_OUT    0x02
#define EPNUM_CDC_IN     0x82
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN)

static const uint8_t desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 4, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
};

const uint8_t *tud_descriptor_configuration_cb(uint8_t index) {
  (void)index;
  return desc_configuration;
}

static const char *const string_desc[] = {
    NULL,                      // 0: language (handled below)
    "LA-F103",                 // 1: manufacturer
    "LA-F103 Logic Analyzer",  // 2: product
    NULL,                      // 3: serial number (chip UID)
    "SUMP",                    // 4: CDC interface
};

static uint16_t s_str[32 + 1];

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
  (void)langid;
  size_t n = 0;
  if (index == 0) {
    s_str[1] = 0x0409;
    n = 1;
  } else if (index == 3) {
    static const char hex[] = "0123456789ABCDEF";
    const uint8_t *uid = (const uint8_t *)UID_BASE;  // 96-bit unique ID
    for (unsigned i = 0; i < 12; i++) {
      s_str[1 + n++] = (uint16_t)hex[uid[i] >> 4];
      s_str[1 + n++] = (uint16_t)hex[uid[i] & 0xF];
    }
  } else {
    if (index >= sizeof string_desc / sizeof string_desc[0]) return NULL;
    const char *s = string_desc[index];
    n = strlen(s);
    if (n > 32) n = 32;
    for (size_t i = 0; i < n; i++) s_str[1 + i] = (uint16_t)s[i];
  }
  s_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2u * n + 2u));
  return s_str;
}
