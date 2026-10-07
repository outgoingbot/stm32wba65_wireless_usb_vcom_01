#ifndef WIRELESS_BRIDGE_H
#define WIRELESS_BRIDGE_H

#include <stdint.h>

/* Identical firmware on both boards; identities and peer are found at runtime. */
#define BRIDGE_PAN_ID 0xCAFEU
#define BRIDGE_CHANNEL 15U
/* Optional diagnostic: echo USB locally instead of transmitting by radio. */
#ifndef BRIDGE_USB_LOOPBACK
#define BRIDGE_USB_LOOPBACK 0
#endif

/* Watch bridge_stats in CubeIDE. No diagnostic bytes pollute terminal data. */
typedef struct {
  uint32_t usb_rx_bytes, usb_tx_bytes, usb_errors, usb_connections;
  uint32_t usb_irq_count, uart_irq_count, usb_irq_unexpected_off;
  uint32_t radio_tx_ok, radio_tx_fail, radio_rx_frames, radio_rx_bytes;
  uint32_t rx_dropped_bytes, filtered_frames, unexpected_confirms;
  uint32_t init_errors, timeouts, usb_disconnect_dropped_bytes;
  uint32_t hello_tx, hello_rx, discovery_tx_fail, peer_losses, peer_dropped_bytes;
  uint32_t tx_queued, rx_queued;
  uint32_t uid96[3], factory_udn, factory_company, factory_device;
  uint8_t eui64[8], peer_eui64[8], peer_uid96[12];
  char eui64_text[17]; /* conventional most-significant-byte first */
  uint8_t radio_state, peer_state, last_mac_status, last_pib;
  uint8_t last_tx_error;
  uint8_t usb_active, tx_busy, last_lqi;
  int8_t last_rssi;
} BridgeStats;
extern volatile BridgeStats bridge_stats;

void Bridge_Init(void);
void Bridge_Process(void);
void Bridge_USB_Activate(void *instance);
void Bridge_USB_Deactivate(void);

#endif
