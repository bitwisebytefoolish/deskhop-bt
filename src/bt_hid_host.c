#include "bt/btstack_config.h"

#ifdef DH_BT_HID_HOST_KBD

/* Include surgical BTstack headers rather than the all-inclusive btstack.h.
 * btstack.h pulls in sbc_types.h which typedefs UINT8/UINT16 — clashing with
 * deskhop's protocol.h enum values of the same names.
 * TinyUSB and BTstack are intentionally NOT co-included in this translation
 * unit: both define HID_REPORT_TYPE_INPUT/OUTPUT/FEATURE as enum constants
 * and the names collide.  All deskhop state is accessed through bt_hid_state_t
 * (pointer fields only) to keep this file free of TinyUSB headers.
 *
 * btstack_cyw43_init() is NOT called here.  cyw43_arch_init() (poll variant,
 * cyw43_arch_poll.c) already calls it internally when CYW43_ENABLE_BLUETOOTH
 * is set — which it is whenever pico_btstack_cyw43 is linked.  A second call
 * triggers a BTstack run-loop double-init assertion and crashes the board. */
#include "bluetooth.h"
#include "hci.h"
#include "l2cap.h"
#include "gap.h"
#include "btstack_event.h"
#include "classic/hid_host.h"

#include "bt_hid_host.h"

/* Forward-declare without pulling in cyw43.h (which conflicts with BTstack
 * and TinyUSB types).  cyw43_bluetooth_hci_init downloads the BT firmware
 * blob to the CYW43439 and sets cyw43_state.bt_loaded = true. */
extern int cyw43_bluetooth_hci_init(void);

/* 1 KB is enough for a keyboard's HID report descriptor. */
#define HID_DESCRIPTOR_STORAGE_LEN 1024

/* Bluetooth Class of Device (CoD) masks for HID keyboards.
 * Major Service Class bit 13 = Limited Discoverable Mode (ignored here).
 * Major Device Class 0x05 = Peripheral.
 * Minor Device Class bit 6 = Keyboard. */
#define COD_MAJOR_PERIPHERAL   0x0500
#define COD_MAJOR_MASK         0x1F00
#define COD_MINOR_KEYBOARD     0x0040
#define COD_MINOR_MASK         0x00FC

/* Inquiry window: 4 × 1280 ms ≈ 5 s.  Long enough for most keyboards to
 * respond; short enough to retry quickly if the first sweep misses. */
#define INQUIRY_DURATION_UNITS 4

static bt_hid_state_t *g_bt;
static uint8_t         hid_descriptor_storage[HID_DESCRIPTOR_STORAGE_LEN];

/* State machine for the inquiry → connect flow. */
typedef enum {
    BT_STATE_IDLE,
    BT_STATE_INQUIRING,
    BT_STATE_CONNECTING,
    BT_STATE_CONNECTED,
} bt_app_state_t;

static bt_app_state_t g_state            = BT_STATE_IDLE;
static bd_addr_t      g_keyboard_addr    = {0};
static bool           g_keyboard_found   = false;
static uint16_t       g_hid_cid          = 0;
static bool           g_descriptor_valid = false;

/* ---- helpers ------------------------------------------------------- */

static void start_inquiry(void) {
    g_keyboard_found = false;
    g_state = BT_STATE_INQUIRING;
    gap_inquiry_start(INQUIRY_DURATION_UNITS);
}

static void connect_to_keyboard(void) {
    g_state = BT_STATE_CONNECTING;
    uint8_t status = hid_host_connect(g_keyboard_addr,
                                      HID_PROTOCOL_MODE_REPORT,
                                      &g_hid_cid);
    if (status != ERROR_CODE_SUCCESS) {
        g_state = BT_STATE_IDLE;
        start_inquiry();
    }
}

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
            if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING)
                start_inquiry();
            break;

        case HCI_EVENT_INQUIRY_RESULT:
        case HCI_EVENT_INQUIRY_RESULT_WITH_RSSI: {
            if (g_keyboard_found)
                break;

            bd_addr_t addr;
            uint32_t  cod;

            if (event_type == HCI_EVENT_INQUIRY_RESULT) {
                hci_event_inquiry_result_get_bd_addr(packet, addr);
                cod = hci_event_inquiry_result_get_class_of_device(packet);
            } else {
                hci_event_inquiry_result_with_rssi_get_bd_addr(packet, addr);
                cod = hci_event_inquiry_result_with_rssi_get_class_of_device(packet);
            }

            bool is_peripheral = (cod & COD_MAJOR_MASK) == COD_MAJOR_PERIPHERAL;
            bool has_keyboard  = (cod & COD_MINOR_MASK) & COD_MINOR_KEYBOARD;

            if (is_peripheral && has_keyboard) {
                bd_addr_copy(g_keyboard_addr, addr);
                g_keyboard_found = true;
                gap_inquiry_stop();
            }
            break;
        }

        case HCI_EVENT_INQUIRY_COMPLETE:
            if (g_keyboard_found)
                connect_to_keyboard();
            else
                start_inquiry(); /* nothing found — sweep again */
            break;

        case HCI_EVENT_HID_META:
            switch (hci_event_hid_meta_get_subevent_code(packet)) {
                case HID_SUBEVENT_INCOMING_CONNECTION:
                    /* A keyboard initiated the connection (re-pair or reconnect). */
                    hid_host_accept_connection(
                        hid_subevent_incoming_connection_get_hid_cid(packet),
                        HID_PROTOCOL_MODE_REPORT);
                    break;

                case HID_SUBEVENT_CONNECTION_OPENED: {
                    uint8_t status = hid_subevent_connection_opened_get_status(packet);
                    if (status != ERROR_CODE_SUCCESS) {
                        g_state = BT_STATE_IDLE;
                        start_inquiry();
                        break;
                    }
                    g_hid_cid             = hid_subevent_connection_opened_get_hid_cid(packet);
                    g_state               = BT_STATE_CONNECTED;
                    g_descriptor_valid    = false;
                    *g_bt->keyboard_connected = true;
                    break;
                }

                case HID_SUBEVENT_CONNECTION_CLOSED:
                    g_state                   = BT_STATE_IDLE;
                    g_descriptor_valid        = false;
                    *g_bt->keyboard_connected = false;
                    start_inquiry(); /* attempt to reconnect */
                    break;

                case HID_SUBEVENT_DESCRIPTOR_AVAILABLE: {
                    uint8_t status = hid_subevent_descriptor_available_get_status(packet);
                    if (status != ERROR_CODE_SUCCESS)
                        break;
                    if (!g_bt->parse_descriptor || !g_bt->kbd_iface)
                        break;

                    uint16_t       cid  = hid_subevent_descriptor_available_get_hid_cid(packet);
                    const uint8_t *desc = hid_descriptor_storage_get_descriptor_data(cid);
                    uint16_t       len  = hid_descriptor_storage_get_descriptor_len(cid);

                    g_bt->parse_descriptor(g_bt->kbd_iface, desc, (int)len);
                    g_descriptor_valid = true;
                    break;
                }

                case HID_SUBEVENT_REPORT: {
                    if (!g_descriptor_valid)
                        break;
                    if (!g_bt->process_report || !g_bt->kbd_iface)
                        break;

                    const uint8_t *report = hid_subevent_report_get_report(packet);
                    uint16_t       len    = hid_subevent_report_get_report_len(packet);

                    g_bt->process_report((uint8_t *)report, (int)len,
                                         g_bt->kbd_itf, g_bt->kbd_iface);
                    break;
                }

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

    /* btstack_cyw43_init() was already called inside cyw43_arch_init()
     * (via cyw43_arch_poll.c when CYW43_ENABLE_BLUETOOTH is set).
     * Start directly at the Classic BT stack layers. */
    l2cap_init();

    /* Just-works SSP pairing — no PIN, no confirmation UI needed.
     * Classic BT SSP is handled automatically when ENABLE_SSP is defined
     * in btstack_config.h; no sm_init() call needed (that is BLE-only). */
    gap_set_security_level(LEVEL_2);

    /* HID host protocol layer */
    hid_host_init(hid_descriptor_storage, sizeof(hid_descriptor_storage));
    hid_host_register_packet_handler(packet_handler);

    /* Pre-download BT firmware before entering hci_power_control.
     *
     * hci_power_control → hci_power_control_on → hci_transport_cyw43_open
     * → cyw43_bluetooth_hci_init → cyw43_ensure_bt_up → cyw43_btbus_init is
     * a deep call chain that overflows the 2 KB core0 (SCRATCH_Y) stack.
     * Calling cyw43_bluetooth_hci_init here uses a shallower frame; once
     * bt_loaded is true, cyw43_ensure_bt_up skips the download on the
     * second call from inside hci_transport_cyw43_open. */
    cyw43_bluetooth_hci_init();

    /* Power on the radio — BTstack will call back via packet_handler as
     * HCI_STATE_WORKING when the controller is ready. */
    hci_power_control(HCI_POWER_ON);
}

#endif /* DH_BT_HID_HOST_KBD */
