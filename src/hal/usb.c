// USB CDC console via TinyUSB (no SoftDevice). The USB peripheral and HFXO only run while
// VBUS is present, so on battery the whole USB stack costs nothing.
#include "usb.h"
#include "system.h"
#include "tusb.h"
#include "nrf.h"
#include <string.h>

// ---------------------------------------------------------------------------
// descriptors
#define USB_VID  0x239A
#define USB_PID  0x8029

static const tusb_desc_device_t desc_device = {
  .bLength            = sizeof(tusb_desc_device_t),
  .bDescriptorType    = TUSB_DESC_DEVICE,
  .bcdUSB             = 0x0200,
  .bDeviceClass       = TUSB_CLASS_MISC,
  .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
  .bDeviceProtocol    = MISC_PROTOCOL_IAD,
  .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
  .idVendor           = USB_VID,
  .idProduct          = USB_PID,
  .bcdDevice          = 0x0100,
  .iManufacturer      = 1,
  .iProduct           = 2,
  .iSerialNumber      = 3,
  .bNumConfigurations = 1
};

uint8_t const* tud_descriptor_device_cb(void) { return (uint8_t const*)&desc_device; }

enum { ITF_CDC = 0, ITF_CDC_DATA, ITF_TOTAL };
#define EPNUM_CDC_NOTIF 0x81
#define EPNUM_CDC_OUT   0x02
#define EPNUM_CDC_IN    0x82
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN)

static const uint8_t desc_config[] = {
  TUD_CONFIG_DESCRIPTOR(1, ITF_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),
  TUD_CDC_DESCRIPTOR(ITF_CDC, 4, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
};

uint8_t const* tud_descriptor_configuration_cb(uint8_t index) { (void)index; return desc_config; }

static uint16_t desc_str[33];

uint16_t const* tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
  (void)langid;
  char buf[33];
  const char* s;
  switch (index) {
    case 0: desc_str[1] = 0x0409; desc_str[0] = (TUSB_DESC_STRING << 8) | 4; return desc_str;
    case 1: s = "MeshCore-LP"; break;
    case 2: s = "Faketec LP Repeater"; break;
    case 3: {
      uint32_t a = NRF_FICR->DEVICEID[1], b = NRF_FICR->DEVICEID[0];
      static const char hx[] = "0123456789ABCDEF";
      for (int i = 0; i < 8; i++) { buf[i] = hx[(a >> (28 - 4 * i)) & 15]; buf[8 + i] = hx[(b >> (28 - 4 * i)) & 15]; }
      buf[16] = 0;
      s = buf;
      break;
    }
    case 4: s = "MeshCore-LP CLI"; break;
    default: return NULL;
  }
  size_t n = strlen(s);
  if (n > 32) n = 32;
  for (size_t i = 0; i < n; i++) desc_str[1 + i] = (uint8_t)s[i];
  desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * n + 2));
  return desc_str;
}

// ---------------------------------------------------------------------------
// 1200 baud "touch" -> Adafruit bootloader DFU (same convention as Arduino / pio upload)
static uint32_t g_dtr_since;

void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts) {
  (void)rts;
  uint32_t t = sys_millis();
  g_dtr_since = dtr ? (t ? t : 1) : 0;
  if (!dtr) {
    cdc_line_coding_t lc;
    tud_cdc_n_get_line_coding(itf, &lc);
    if (lc.bit_rate == 1200) sys_reset_to_bootloader();
  }
}

void tud_cdc_line_coding_cb(uint8_t itf, cdc_line_coding_t const* lc) {
  if (lc->bit_rate == 1200 && !tud_cdc_n_connected(itf)) sys_reset_to_bootloader();
}

// ---------------------------------------------------------------------------
// VBUS power events (normally nrfx_power's job)
extern void tusb_hal_nrf_power_event(uint32_t event);

void POWER_CLOCK_IRQHandler(void) {
  if (NRF_POWER->EVENTS_USBDETECTED) {
    NRF_POWER->EVENTS_USBDETECTED = 0; (void)NRF_POWER->EVENTS_USBDETECTED;
    tusb_hal_nrf_power_event(0);
  }
  if (NRF_POWER->EVENTS_USBPWRRDY) {
    NRF_POWER->EVENTS_USBPWRRDY = 0; (void)NRF_POWER->EVENTS_USBPWRRDY;
    tusb_hal_nrf_power_event(2);
  }
  if (NRF_POWER->EVENTS_USBREMOVED) {
    NRF_POWER->EVENTS_USBREMOVED = 0; (void)NRF_POWER->EVENTS_USBREMOVED;
    tusb_hal_nrf_power_event(1);
  }
  sys_wake();
}

void USBD_IRQHandler(void) {
  tud_int_handler(0);
  sys_wake();
}

void usb_init(void) {
  tusb_rhport_init_t dev_init = { .role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_AUTO };
  tusb_init(0, &dev_init);

  NRF_POWER->EVENTS_USBDETECTED = 0;
  NRF_POWER->EVENTS_USBREMOVED = 0;
  NRF_POWER->EVENTS_USBPWRRDY = 0;
  NRF_POWER->INTENSET = POWER_INTENSET_USBDETECTED_Msk | POWER_INTENSET_USBREMOVED_Msk | POWER_INTENSET_USBPWRRDY_Msk;
  NVIC_SetPriority(POWER_CLOCK_IRQn, 7);
  NVIC_EnableIRQ(POWER_CLOCK_IRQn);

  uint32_t st = NRF_POWER->USBREGSTATUS;
  if (st & POWER_USBREGSTATUS_VBUSDETECT_Msk) tusb_hal_nrf_power_event(0);
  if (st & POWER_USBREGSTATUS_OUTPUTRDY_Msk)  tusb_hal_nrf_power_event(2);
}

bool usb_vbus(void) {
  return (NRF_POWER->USBREGSTATUS & POWER_USBREGSTATUS_VBUSDETECT_Msk) != 0;
}

void usb_task(void) {
  if (NRF_USBD->ENABLE) tud_task();
}

bool usb_connected(void) {
  return NRF_USBD->ENABLE && tud_cdc_connected();
}

int usb_read(void) {
  if (!usb_connected() || !tud_cdc_available()) return -1;
  return tud_cdc_read_char();
}

size_t usb_write(const void* data, size_t len) {
  // Never block the radio work: whatever does not fit into the 2 KB TX FIFO is dropped.
  if (!usb_connected()) return len;   // nobody listening: drop silently
  const uint8_t* p = (const uint8_t*)data;
  uint32_t n = tud_cdc_write(p, len);
  if (n < len) {
    tud_cdc_write_flush();
    tud_task();
    tud_cdc_write(p + n, len - n);
  }
  tud_cdc_write_flush();
  return len;
}

void usb_flush(void) {
  if (usb_connected()) tud_cdc_write_flush();
}

void usb_discard_input(void) {
  if (usb_connected()) tud_cdc_read_flush();
}

uint32_t usb_connected_ms(void) {
  if (!usb_connected() || !g_dtr_since) return 0;
  uint32_t d = sys_millis() - g_dtr_since;
  return d ? d : 1;
}
