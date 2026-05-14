#include "bt/btstack_config.h"

#ifdef DH_BT_HID_HOST_KBD

/* Include surgical BTstack headers rather than the all-inclusive btstack.h.
 * btstack.h pulls in sbc_types.h which typedefs UINT8/UINT16 — clashing with
 * deskhop's protocol.h enum values of the same names.
 * TinyUSB and BTstack are intentionally NOT co-included in this translation
 * unit: both define HID_REPORT_TYPE_INPUT/OUTPUT/FEATURE as enum constants
 * and the names collide.  All deskhop state is accessed through bt_hid_state_t
 * (pointer fields only) to keep this file free of TinyUSB headers. */
#include "bluetooth.h"
#include "hci.h"
#include "l2cap.h"
#include "gap.h"
#include "btstack_event.h"
#include "classic/hid_host.h"

#include "pico/btstack_cyw43.h"
#include "pico/cyw43_arch.h"

#include "bt_hid_host.h"

/* 1 KB is enough for a keyboard's HID report descriptor. */
#define HID_DESCRIPTOR_STORAGE_LEN 1024

static bt_hid_state_t *g_bt;
static uint8_t         hid_descriptor_storage[HID_DESCRIPTOR_STORAGE_LEN];

/* ---- packet handler ------------------------------------------------ */

static void packet_handler(uint8_t packet_type, uint16_t channel,
                            uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;

    if (packet_type != HCI_EVENT_PACKET)
        return;

    uint8_t event_type = hci_event_packet_get_type(packet);

    switch (event_type) {
        case BTSTACK_EVENT_STATE:
            if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
                /* Radio is up — ready to accept connections or initiate inquiry.
                 * Commit 5 will start inquiry / reconnect to bonded device here. */
            }
            break;

        case HCI_EVENT_HID_META:
            switch (hci_event_hid_meta_get_subevent_code(packet)) {
                case HID_SUBEVENT_CONNECTION_OPENED: {
                    uint8_t status = hid_subevent_connection_opened_get_status(packet);
                    if (status != ERROR_CODE_SUCCESS) {
                        break;
                    }
                    *g_bt->keyboard_connected = true;
                    break;
                }
                case HID_SUBEVENT_CONNECTION_CLOSED:
                    *g_bt->keyboard_connected = false;
                    /* Commit 5 will add reconnect logic here. */
                    break;

                case HID_SUBEVENT_REPORT:
                    /* Stub: report received — wired to process_keyboard_report in commit 5. */
                    break;

                case HID_SUBEVENT_DESCRIPTOR_AVAILABLE:
                    /* HID descriptor parsed and ready.
                     * Commit 5 populates bt_hid_iface from the descriptor here. */
                    break;

                default:
                    break;
            }
            break;

        default:
            break;
    }
}

/* ---- public init --------------------------------------------------- */

void bt_hid_host_init(bt_hid_state_t *bt_state) {
    g_bt = bt_state;

    /* Wire the BTstack run-loop to the cyw43_arch poll async-context,
     * initialise the CYW43 HCI transport, and set up TLV flash bond
     * storage in FLASH_BTSTACK_BANK (offset set in btstack_config.h). */
    btstack_cyw43_init(cyw43_arch_async_context());

    /* Classic BT stack layers */
    l2cap_init();

    /* Just-works SSP pairing — no PIN, no confirmation UI needed.
     * Classic BT SSP is handled automatically when ENABLE_SSP is defined
     * in btstack_config.h; no sm_init() call needed (that is BLE-only). */
    gap_set_security_level(LEVEL_2);

    /* HID host protocol layer */
    hid_host_init(hid_descriptor_storage, sizeof(hid_descriptor_storage));
    hid_host_register_packet_handler(packet_handler);

    /* Power on the radio — BTstack will call back via packet_handler as
     * HCI_STATE_WORKING when the controller is ready. */
    hci_power_control(HCI_POWER_ON);
}

#endif /* DH_BT_HID_HOST_KBD */
