/* One-image USBX standalone <-> ST 802.15.4 MAC bridge.
 * MAC callbacks/queues run in the sequencer foreground. USB lifecycle callbacks
 * can run in the IRQ and only publish an instance/epoch. See README-bridge.md.
 */
#include "wireless_bridge.h"
#include "main.h"
#include "ux_api.h"
#include "ux_device_class_cdc_acm.h"
#include "st_mac_802_15_4_sap.h"
#include "radio.h"
#include "stm32wbaxx_ll_system.h"
#include "log_module.h"
#include <string.h>

#define QUEUE_SIZE 2048U
#define USER_PAYLOAD 64U
#define MAC_PAYLOAD 96U
#define MAC_INSTANCE 0U
#define OP_TIMEOUT_MS 2000U
#define PEER_TIMEOUT_MS 5000U
#define SETTLE_MS 2000U
#define UID_SIZE 12U
#define HELLO_SIZE 38U
#define DATA_HEADER_SIZE 29U
#define HELLO_TYPE 1U
#define DATA_TYPE 2U
/* Wire payload: "WUB1", type, sender UID96 (12), then:
 * HELLO: selected peer EUI64 (8, MAC byte order), selected peer UID96 (12),
 *        local settling complete (1).
 * DATA: destination UID96 (12), user bytes (1..64).
 * UIDs are serialized word0,word1,word2, each least-significant byte first.
 */
enum { RADIO_OFF, RADIO_RESET, RADIO_CONFIG, RADIO_READY, RADIO_FAULT };
enum { PEER_SEARCHING, PEER_CANDIDATE, PEER_PAIRED, PEER_AMBIGUOUS, PEER_COLLISION };
typedef struct { uint8_t data[QUEUE_SIZE]; uint16_t in, out, count; } ByteQueue;
static ByteQueue to_radio, to_usb;
static UX_SLAVE_CLASS_CDC_ACM *cdc;
static UX_SLAVE_CLASS_CDC_ACM * volatile published_cdc;
static volatile uint32_t usb_epoch;
static uint32_t seen_usb_epoch;
static uint8_t usb_read_buffer[512], usb_write_buffer[USER_PAYLOAD];
static ULONG usb_read_size, usb_write_size;
static uint8_t usb_read_pending;
static uint8_t radio_payload[MAC_PAYLOAD], tx_type;
static ST_MAC_dataReq_t data_request;
static ST_MAC_resetReq_t reset_request = { .set_default_PIB = 1 };
static ST_MAC_setReq_t set_request;
static uint8_t config_step, config_pending;
static uint32_t config_started, tx_started, next_hello, peer_seen, candidate_since;
static uint8_t local_uid[UID_SIZE], extended_address[8], peer_eui[8], peer_uid[UID_SIZE];
static uint8_t have_peer, peer_selects_us, peer_settled;
static uint8_t pan[2] = { BRIDGE_PAN_ID & 255U, BRIDGE_PAN_ID >> 8 };
/* 0xFFFE means extended addressing, no allocated short address. */
static uint8_t short_address[2] = { 0xFE, 0xFF };
static uint8_t channel = BRIDGE_CHANNEL, rx_on = 1, retries = 3;
static const struct { uint8_t attribute; uint8_t *value; } config[] = {
  { g_MAC_EXTENDED_ADDRESS_c, extended_address },
  { g_MAC_SHORT_ADDRESS_c, short_address },
  { g_MAC_PAN_ID_c, pan },
  { g_PHY_CURRENT_CHANNEL_c, &channel },
  { g_MAC_MAX_FRAME_RETRIES_c, &retries },
  { g_MAC_RX_ON_WHEN_IDLE_c, &rx_on }
};
volatile BridgeStats bridge_stats;

static void queue_put(ByteQueue *q, const uint8_t *p, uint16_t n)
{
  while (n--) { q->data[q->in] = *p++; q->in = (q->in + 1U) % QUEUE_SIZE; q->count++; }
}
static void queue_get(ByteQueue *q, uint8_t *p, uint16_t n)
{
  while (n--) { *p++ = q->data[q->out]; q->out = (q->out + 1U) % QUEUE_SIZE; q->count--; }
}
static void init_fault(uint8_t status)
{
  bridge_stats.last_mac_status = status;
  bridge_stats.init_errors++;
  bridge_stats.radio_state = RADIO_FAULT;
  LOG_ERROR_APP("bridge MAC init fault: status=0x%02X PIB=0x%02X\r\n", status, bridge_stats.last_pib);
}
static void peer_block(uint8_t state)
{
  bridge_stats.peer_state = state;
  peer_selects_us = 0;
  peer_settled = 0;
  bridge_stats.peer_dropped_bytes += to_radio.count;
  memset(&to_radio, 0, sizeof(to_radio));
}
static MAC_Status_t reset_confirm(const ST_MAC_resetCnf_t *p)
{
  if (bridge_stats.radio_state != RADIO_RESET) { bridge_stats.unexpected_confirms++; return MAC_SUCCESS; }
  bridge_stats.last_mac_status = p->status;
  if (p->status != MAC_SUCCESS) init_fault(p->status);
  else {
    config_step = 0; config_pending = 0; bridge_stats.radio_state = RADIO_CONFIG;
    LOG_INFO_APP("bridge MAC reset confirmed; configuring PIB\r\n");
  }
  return MAC_SUCCESS;
}
static MAC_Status_t set_confirm(const ST_MAC_setCnf_t *p)
{
  if (bridge_stats.radio_state != RADIO_CONFIG || !config_pending ||
      p->PIB_attribute != config[config_step].attribute) {
    bridge_stats.unexpected_confirms++; return MAC_SUCCESS;
  }
  bridge_stats.last_mac_status = p->status;
  config_pending = 0;
  if (p->status != MAC_SUCCESS) init_fault(p->status);
  else if (++config_step == sizeof(config) / sizeof(config[0])) {
    bridge_stats.radio_state = RADIO_READY;
    next_hello = HAL_GetTick() + 100U + bridge_stats.factory_udn % 400U;
    LOG_INFO_APP("bridge MAC ready: PAN=%04X channel=%u; discovering peer\r\n", BRIDGE_PAN_ID, BRIDGE_CHANNEL);
  }
  return MAC_SUCCESS;
}
static MAC_Status_t data_confirm(const ST_MAC_dataCnf_t *p)
{
  if (!bridge_stats.tx_busy || p->msdu_handle != data_request.msdu_handle) {
    bridge_stats.unexpected_confirms++; return MAC_SUCCESS;
  }
  bridge_stats.last_mac_status = p->status;
  if (tx_type == HELLO_TYPE) {
    if (p->status == MAC_SUCCESS) bridge_stats.hello_tx++;
    else bridge_stats.discovery_tx_fail++;
  } else if (p->status == MAC_SUCCESS) bridge_stats.radio_tx_ok++;
  else { bridge_stats.radio_tx_fail++; bridge_stats.last_tx_error = p->status; }
  bridge_stats.tx_busy = 0;
  return MAC_SUCCESS;
}
static void receive_hello(const ST_MAC_dataInd_t *p)
{
  const uint8_t *sender_uid = p->msdu + 5;
  const uint8_t *source = p->src_address.a_extend_addr;
  uint32_t now = HAL_GetTick();
  bridge_stats.hello_rx++;
  /* Detect duplicate identity observations; do not silently send to a shared
   * MAC address. Same UID and same EUI is a reflected self-frame and ignored. */
  if (!memcmp(source, extended_address, 8)) {
    if (memcmp(sender_uid, local_uid, UID_SIZE)) peer_block(PEER_COLLISION);
    return;
  }
  if (!memcmp(sender_uid, local_uid, UID_SIZE)) { peer_block(PEER_COLLISION); return; }
  if (bridge_stats.peer_state >= PEER_AMBIGUOUS) return;
  if (!have_peer) {
    memcpy(peer_eui, source, 8); memcpy(peer_uid, sender_uid, UID_SIZE);
    for (unsigned i = 0; i < 8; i++) bridge_stats.peer_eui64[i] = peer_eui[i];
    for (unsigned i = 0; i < UID_SIZE; i++) bridge_stats.peer_uid96[i] = peer_uid[i];
    have_peer = 1; candidate_since = now;
    bridge_stats.peer_state = PEER_CANDIDATE;
  } else if (memcmp(peer_eui, source, 8) || memcmp(peer_uid, sender_uid, UID_SIZE)) {
    /* More than one peer: stop, do not pick a winner or reroute queued data. */
    peer_block(!memcmp(peer_eui, source, 8) ? PEER_COLLISION : PEER_AMBIGUOUS);
    return;
  }
  peer_seen = now;
  peer_selects_us = !memcmp(p->msdu + 17, extended_address, 8) &&
                    !memcmp(p->msdu + 25, local_uid, UID_SIZE);
  peer_settled = p->msdu[37] == 1;
  if ((!peer_selects_us || !peer_settled) && bridge_stats.peer_state == PEER_PAIRED) {
    bridge_stats.peer_losses++;
    peer_block(PEER_CANDIDATE);
    candidate_since = now;
  }
}
static MAC_Status_t data_indication(const ST_MAC_dataInd_t *p)
{
  if (p->src_addr_mode != g_EXTENDED_ADDR_MODE_c ||
      memcmp(p->a_src_PAN_id, pan, 2) || memcmp(p->a_dst_PAN_id, pan, 2) ||
      p->msdu_length < 5 || p->msdu_length > MAC_PAYLOAD || memcmp(p->msdu, "WUB1", 4)) {
    bridge_stats.filtered_frames++; return MAC_SUCCESS;
  }
  if (p->msdu[4] == HELLO_TYPE && p->msdu_length == HELLO_SIZE &&
      p->dst_addr_mode == g_SHORT_ADDR_MODE_c &&
      p->dst_address.a_short_addr[0] == 0xFF && p->dst_address.a_short_addr[1] == 0xFF) {
    receive_hello(p); return MAC_SUCCESS;
  }
  if (p->msdu[4] != DATA_TYPE || p->msdu_length <= DATA_HEADER_SIZE ||
      p->msdu_length > DATA_HEADER_SIZE + USER_PAYLOAD ||
      bridge_stats.peer_state >= PEER_AMBIGUOUS || !have_peer || !peer_selects_us ||
      (uint32_t)(HAL_GetTick() - candidate_since) < SETTLE_MS ||
      (uint32_t)(HAL_GetTick() - peer_seen) > PEER_TIMEOUT_MS ||
      p->dst_addr_mode != g_EXTENDED_ADDR_MODE_c ||
      memcmp(p->src_address.a_extend_addr, peer_eui, 8) ||
      memcmp(p->dst_address.a_extend_addr, extended_address, 8) ||
      memcmp(p->msdu + 5, peer_uid, UID_SIZE) || memcmp(p->msdu + 17, local_uid, UID_SIZE)) {
    bridge_stats.filtered_frames++; return MAC_SUCCESS;
  }
  uint16_t length = p->msdu_length - DATA_HEADER_SIZE;
  bridge_stats.radio_rx_frames++;
  bridge_stats.radio_rx_bytes += length;
  bridge_stats.last_lqi = p->mpdu_link_quality;
  bridge_stats.last_rssi = p->rssi;
  if (BRIDGE_USB_LOOPBACK || !cdc || length > QUEUE_SIZE - to_usb.count)
    bridge_stats.rx_dropped_bytes += length;
  else queue_put(&to_usb, p->msdu + DATA_HEADER_SIZE, length);
  return MAC_SUCCESS;
}
static ST_MAC_callbacks_t callbacks = {
  .mlmeResetCnfCb = reset_confirm, .mlmeSetCnfCb = set_confirm,
  .mcpsDataCnfCb = data_confirm, .mcpsDataIndCb = data_indication
};

void Bridge_Init(void)
{
  static const char hex[] = "0123456789ABCDEF";
  uint32_t uid[3] = { HAL_GetUIDw0(), HAL_GetUIDw1(), HAL_GetUIDw2() };
  for (unsigned word = 0; word < 3; word++) {
    bridge_stats.uid96[word] = uid[word];
    for (unsigned byte = 0; byte < 4; byte++) local_uid[word * 4 + byte] = uid[word] >> (byte * 8);
  }
  bridge_stats.factory_udn = LL_FLASH_GetUDN();
  bridge_stats.factory_company = LL_FLASH_GetSTCompanyID();
  bridge_stats.factory_device = LL_FLASH_GetDeviceID();
  /* Use the vendor's factory EUI API, not a truncation/hash of the 96-bit UID.
   * This package returns MAC byte order (LSB first). See identity evidence in
   * README: its implementation uses only UDN24; discovery also checks UID96. */
  otPlatRadioGetIeeeEui64(NULL, extended_address);
  for (unsigned i = 0; i < 8; i++) {
    bridge_stats.eui64[i] = extended_address[i];
    bridge_stats.eui64_text[2 * i] = hex[extended_address[7 - i] >> 4];
    bridge_stats.eui64_text[2 * i + 1] = hex[extended_address[7 - i] & 15];
  }
  LOG_INFO_APP("bridge UID=%08lX%08lX%08lX EUI=%s\r\n",
               (unsigned long)uid[0], (unsigned long)uid[1], (unsigned long)uid[2],
               (const char *)bridge_stats.eui64_text);
  if (bridge_stats.factory_udn == 0xFFFFFFFFU || bridge_stats.factory_company == 0xFFFFFFU ||
      bridge_stats.factory_company == 0U) { init_fault(0xFE); return; }
  MAC_Status_t status = ST_MAC_init(&callbacks); /* preInit already ran. */
  if (status != MAC_SUCCESS) { init_fault(status); return; }
  bridge_stats.radio_state = RADIO_RESET;
  config_started = HAL_GetTick();
  status = ST_MAC_MLMEResetReq(MAC_INSTANCE, &reset_request);
  if (status != MAC_SUCCESS) init_fault(status);
}

void Bridge_USB_Activate(void *instance)
{
  published_cdc = (UX_SLAVE_CLASS_CDC_ACM *)instance;
  usb_epoch++;
}
void Bridge_USB_Deactivate(void)
{
  published_cdc = NULL;
  usb_epoch++;
}
static void usb_process(void)
{
  ULONG actual = 0;
  UINT result;
  ByteQueue *destination = BRIDGE_USB_LOOPBACK ? &to_usb : &to_radio;
  /* Called with only USB IRQ masked. Radio interrupts remain serviceable. */
  if (seen_usb_epoch != usb_epoch) {
    seen_usb_epoch = usb_epoch;
    bridge_stats.usb_disconnect_dropped_bytes += to_radio.count + to_usb.count + usb_write_size;
    memset(&to_radio, 0, sizeof(to_radio)); memset(&to_usb, 0, sizeof(to_usb));
    usb_read_pending = 0; usb_write_size = 0;
    cdc = published_cdc;
    bridge_stats.usb_active = cdc != NULL;
    if (cdc) bridge_stats.usb_connections++;
    /* A radio request already owned by the MAC is allowed to finish. */
  }
  if (!cdc) return;
  if (!usb_read_pending) {
    usb_read_size = (_ux_system_slave->ux_system_slave_speed == UX_HIGH_SPEED_DEVICE) ? 512U : 64U;
    if (QUEUE_SIZE - destination->count >= usb_read_size &&
        (BRIDGE_USB_LOOPBACK || (bridge_stats.radio_state == RADIO_READY &&
                                bridge_stats.peer_state == PEER_PAIRED))) usb_read_pending = 1;
  }
  if (usb_read_pending) {
    result = ux_device_class_cdc_acm_read_run(cdc, usb_read_buffer, usb_read_size, &actual);
    if (result == UX_STATE_NEXT) {
      if (BRIDGE_USB_LOOPBACK || bridge_stats.peer_state == PEER_PAIRED)
        queue_put(destination, usb_read_buffer, (uint16_t)actual);
      else bridge_stats.peer_dropped_bytes += actual;
      bridge_stats.usb_rx_bytes += actual; usb_read_pending = 0;
    } else if (result < UX_STATE_NEXT) { bridge_stats.usb_errors++; usb_read_pending = 0; }
  }
  if (!usb_write_size && to_usb.count) {
    usb_write_size = to_usb.count > USER_PAYLOAD ? USER_PAYLOAD : to_usb.count;
    queue_get(&to_usb, usb_write_buffer, (uint16_t)usb_write_size);
  }
  if (usb_write_size) {
    result = ux_device_class_cdc_acm_write_run(cdc, usb_write_buffer, usb_write_size, &actual);
    if (result == UX_STATE_NEXT) { bridge_stats.usb_tx_bytes += actual; usb_write_size = 0; }
    else if (result < UX_STATE_NEXT) {
      bridge_stats.usb_errors++; bridge_stats.rx_dropped_bytes += usb_write_size; usb_write_size = 0;
    }
  }
}
static void transmit(uint8_t type, uint32_t now)
{
  memset(&data_request, 0, sizeof(data_request));
  static uint8_t handle;
  data_request.msdu_handle = ++handle;
  data_request.src_addr_mode = g_EXTENDED_ADDR_MODE_c;
  memcpy(data_request.a_dst_PAN_id, pan, 2);
  memcpy(radio_payload, "WUB1", 4); radio_payload[4] = type;
  memcpy(radio_payload + 5, local_uid, UID_SIZE);
  if (type == HELLO_TYPE) {
    data_request.dst_addr_mode = g_SHORT_ADDR_MODE_c;
    memset(data_request.dst_address.a_short_addr, 0xFF, 2);
    memset(radio_payload + 17, 0, 21);
    if (have_peer && bridge_stats.peer_state < PEER_AMBIGUOUS) {
      memcpy(radio_payload + 17, peer_eui, 8); memcpy(radio_payload + 25, peer_uid, UID_SIZE);
      radio_payload[37] = (uint32_t)(now - candidate_since) >= SETTLE_MS;
    }
    data_request.msdu_length = HELLO_SIZE;
    next_hello = now + 700U + ((bridge_stats.factory_udn ^ now) % 400U);
  } else {
    data_request.dst_addr_mode = g_EXTENDED_ADDR_MODE_c;
    memcpy(data_request.dst_address.a_extend_addr, peer_eui, 8);
    memcpy(radio_payload + 17, peer_uid, UID_SIZE);
    uint16_t length = to_radio.count > USER_PAYLOAD ? USER_PAYLOAD : to_radio.count;
    queue_get(&to_radio, radio_payload + DATA_HEADER_SIZE, length);
    data_request.msdu_length = DATA_HEADER_SIZE + length;
    data_request.ack_Tx = 1;
  }
  data_request.msduPtr = radio_payload;
  bridge_stats.tx_busy = 1; tx_started = now; tx_type = type;
  MAC_Status_t status = ST_MAC_MCPSDataReq(MAC_INSTANCE, &data_request);
  if (status != MAC_SUCCESS) {
    bridge_stats.last_mac_status = status;
    if (type == HELLO_TYPE) bridge_stats.discovery_tx_fail++;
    else { bridge_stats.radio_tx_fail++; bridge_stats.last_tx_error = status; }
    bridge_stats.tx_busy = 0;
  }
}
void Bridge_Process(void)
{
  uint32_t now = HAL_GetTick();
  if (bridge_stats.radio_state == RADIO_RESET ||
      (bridge_stats.radio_state == RADIO_CONFIG && config_pending)) {
    if ((uint32_t)(now - config_started) > OP_TIMEOUT_MS) { bridge_stats.timeouts++; init_fault(0xFF); }
  }
  if (bridge_stats.radio_state == RADIO_CONFIG && !config_pending) {
    set_request.PIB_attribute = config[config_step].attribute;
    set_request.PIB_attribute_valuePtr = config[config_step].value;
    bridge_stats.last_pib = set_request.PIB_attribute;
    config_pending = 1; config_started = now;
    MAC_Status_t status = ST_MAC_MLMESetReq(MAC_INSTANCE, &set_request);
    if (status != MAC_SUCCESS) init_fault(status);
  }
  if (have_peer && bridge_stats.peer_state < PEER_AMBIGUOUS) {
    if ((uint32_t)(now - peer_seen) > PEER_TIMEOUT_MS && peer_selects_us) {
      bridge_stats.peer_losses++; peer_block(PEER_CANDIDATE); candidate_since = now;
    } else if (peer_selects_us && peer_settled && (uint32_t)(now - candidate_since) >= SETTLE_MS)
      bridge_stats.peer_state = PEER_PAIRED;
  }
  /* Prevent USB endpoint teardown during nonblocking I/O. */
  uint32_t usb_irq_enabled = NVIC_GetEnableIRQ(USB_OTG_HS_IRQn);
  if (!usb_irq_enabled) bridge_stats.usb_irq_unexpected_off++;
  NVIC_DisableIRQ(USB_OTG_HS_IRQn); __DSB(); __ISB();
  usb_process();
  NVIC_EnableIRQ(USB_OTG_HS_IRQn); __DSB(); __ISB();
  if (bridge_stats.tx_busy && bridge_stats.radio_state == RADIO_READY &&
      (uint32_t)(now - tx_started) > OP_TIMEOUT_MS) {
    bridge_stats.timeouts++; bridge_stats.radio_state = RADIO_FAULT;
  }
  if (!BRIDGE_USB_LOOPBACK && bridge_stats.radio_state == RADIO_READY && !bridge_stats.tx_busy) {
    if ((int32_t)(now - next_hello) >= 0) transmit(HELLO_TYPE, now);
    else if (bridge_stats.peer_state == PEER_PAIRED && to_radio.count) transmit(DATA_TYPE, now);
  }
  bridge_stats.tx_queued = to_radio.count; bridge_stats.rx_queued = to_usb.count;
  /* Rate-limited foreground summaries; no payload dumps or per-packet logs. */
  static uint32_t last_log;
  static BridgeStats previous;
  if ((uint32_t)(now - last_log) >= 2000U) {
    BridgeStats current = bridge_stats;
    last_log = now;
    if (memcmp(&previous, &current, sizeof(current))) {
      LOG_INFO_APP("bridge t=%lu mac=%u peer=%u usb=%u txOK=%lu txFail=%lu rx=%lu usbRx=%lu usbTx=%lu drop=%lu status=%02X\r\n",
        (unsigned long)now, current.radio_state, current.peer_state, current.usb_active,
        (unsigned long)current.radio_tx_ok, (unsigned long)current.radio_tx_fail,
        (unsigned long)current.radio_rx_frames, (unsigned long)current.usb_rx_bytes,
        (unsigned long)current.usb_tx_bytes,
        (unsigned long)(current.rx_dropped_bytes + current.peer_dropped_bytes), current.last_tx_error);
      if (memcmp(previous.peer_eui64, current.peer_eui64, 8)) {
        LOG_INFO_APP("bridge peer EUI=%02X%02X%02X%02X%02X%02X%02X%02X (states: 0 search,1 candidate,2 paired,3 ambiguous,4 collision)\r\n",
          peer_eui[7], peer_eui[6], peer_eui[5], peer_eui[4], peer_eui[3], peer_eui[2], peer_eui[1], peer_eui[0]);
      }
      previous = current;
    }
  }
}
