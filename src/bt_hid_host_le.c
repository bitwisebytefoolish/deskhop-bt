#include "bt/btstack_config.h"

#ifdef DH_BT_HID_HOST_KBD

/* BLE HID-over-GATT Profile (HOGP) host — peer to bt_hid_host.c.  See
 * src/include/bt_hid_host_le.h for the architectural rationale and #29
 * for the use-case motivation (8BitDo Retro and other BLE-only keyboards).
 *
 * Initial implementation tried boot-mode HOGP (manual GATT discovery of
 * the BOOT_KEYBOARD_INPUT_REPORT characteristic).  The 8BitDo Retro
 * silently rejected the PROTOCOL_MODE = BOOT write — connection reached
 * "READY" but no notifications ever arrived, and the link dropped after
 * a few seconds.  Modern BLE keyboards routinely lack boot-mode support
 * (BIOS hosts are essentially extinct).
 *
 * Switched to BTstack's hids_client helper, which handles report-mode
 * HOGP end-to-end: discovers HID service(s), reads each report's
 * REPORT_REFERENCE descriptor to determine report ID + type, subscribes
 * to all INPUT reports via CCCD, and emits a single
 * GATTSERVICE_SUBEVENT_HID_REPORT event per incoming report with the
 * raw bytes.  Boot-mode is still supported by passing
 * HID_PROTOCOL_MODE_BOOT, but we default to REPORT mode.
 *
 * Report data is converted to deskhop's 8-byte boot-keyboard format
 * (modifier + reserved + 6 keycodes) by walking the report with
 * BTstack's btstack_hid_parser and extracting keyboard-page (0x07)
 * usages.  We feed THAT into process_keyboard_report — which, with
 * iface->protocol = HID_PROTOCOL_BOOT (set in setup.c), takes the
 * _extract_kbd_boot path and copies the 8 bytes straight through.
 * So both transports (Classic boot, BLE report) converge at the same
 * deskhop entry point without per-transport conditionals downstream.
 *
 * Reference: pico-sdk/lib/btstack/example/hog_host_demo.c. */

#include "bluetooth.h"
#include "bluetooth_gatt.h"
#include "hci.h"
#include "l2cap.h"
#include "gap.h"
#include "btstack_event.h"
#include "btstack_util.h"
#include "btstack_hid.h"
#include "btstack_hid_parser.h"
#include "ble/sm.h"
#include "ble/gatt_client.h"
#include "ble/att_db.h"
#include "ble/gatt-service/hids_client.h"
#include "ad_parser.h"
#include "btstack_tlv.h"

#include <stdio.h>
#include <string.h>

#include "bt_hid_host_le.h"

/* ---- Shared state -------------------------------------------------- */

static bt_hid_state_t *g_bt;

/* ---- App state machine -------------------------------------------- */

typedef enum {
    LE_W4_WORKING,                /* wait for HCI_STATE_WORKING */
    LE_W4_HID_DEVICE_FOUND,       /* scanning, waiting for HID adv */
    LE_W4_CONNECTED,              /* gap_connect issued, waiting for LE_CONNECTION_COMPLETE */
    LE_W4_ENCRYPTED,              /* connection up; waiting for pairing or re-encryption */
    LE_W4_HIDS_CONNECTED,         /* hids_client running its discovery */
    LE_READY,                     /* HID input reports flowing */
    LE_W4_TIMEOUT_THEN_SCAN,
    LE_W4_TIMEOUT_THEN_RECONNECT,
} le_app_state_t;

static le_app_state_t le_state = LE_W4_WORKING;

/* Remote BLE device we're connecting to. */
typedef struct {
    bd_addr_t      addr;
    bd_addr_type_t addr_type;
} le_device_addr_t;

static le_device_addr_t le_remote;
static hci_con_handle_t le_connection_handle = HCI_CON_HANDLE_INVALID;

/* hids_client state.  cid is the per-connection HID client ID; descriptor
 * storage holds the parsed REPORT_MAP from the peripheral (used by the
 * btstack_hid_parser when we extract keys from each report).  Size chosen
 * to fit a typical keyboard's HID descriptor (200–400 B) with some slack. */
#define LE_HID_DESCRIPTOR_STORAGE_LEN  512
static uint8_t  le_hid_descriptor_storage[LE_HID_DESCRIPTOR_STORAGE_LEN];
static uint16_t le_hids_cid;

/* Connection-establishment + reconnect timer. */
static btstack_timer_source_t le_connection_timer;

/* Packet-callback registrations.  Must outlive registration (file-scope OK). */
static btstack_packet_callback_registration_t le_hci_cb;
static btstack_packet_callback_registration_t le_sm_cb;

/* Bond persistence: device address/type stored under a tag so we can
 * prefer a direct reconnect on boot rather than re-scanning. */
#define TLV_TAG_HOGD ((((uint32_t)'H') << 24) | (((uint32_t)'O') << 16) | (((uint32_t)'G') << 8) | 'D')
static const btstack_tlv_t *le_tlv_impl;
static void                *le_tlv_ctx;

/* ---- forward declarations ----------------------------------------- */

static void le_start_scan(void);
static void le_start_connect(void);
static void le_connect_to_remote(void);
static void le_connection_timeout_cb(btstack_timer_source_t *ts);
static void le_reconnect_timeout_cb(btstack_timer_source_t *ts);
static void le_handle_outgoing_connection_error(void);
static void le_kick_hids_client(void);

static void le_packet_handler(uint8_t packet_type, uint16_t channel,
                              uint8_t *packet, uint16_t size);
static void le_sm_packet_handler(uint8_t packet_type, uint16_t channel,
                                 uint8_t *packet, uint16_t size);
static void le_hids_client_event_handler(uint8_t packet_type, uint16_t channel,
                                         uint8_t *packet, uint16_t size);
static void le_handle_input_report(uint8_t service_index,
                                   const uint8_t *report, uint16_t report_len);

/* ---- helpers ------------------------------------------------------- */

static bool le_adv_contains_hid_service(const uint8_t *packet) {
    const uint8_t *ad_data = gap_event_advertising_report_get_data(packet);
    uint8_t        ad_len  = gap_event_advertising_report_get_data_length(packet);
    return ad_data_contains_uuid16(ad_len, ad_data,
                                   ORG_BLUETOOTH_SERVICE_HUMAN_INTERFACE_DEVICE);
}

static void le_start_scan(void) {
    printf("[ble] scanning for HID peripherals (UUID 0x1812)\n");
    le_state = LE_W4_HID_DEVICE_FOUND;
    gap_set_scan_parameters(0, 48, 48);
    gap_start_scan();
}

static void le_connect_to_remote(void) {
    btstack_run_loop_set_timer(&le_connection_timer, 10000);
    btstack_run_loop_set_timer_handler(&le_connection_timer, &le_connection_timeout_cb);
    btstack_run_loop_add_timer(&le_connection_timer);
    le_state = LE_W4_CONNECTED;
    printf("[ble] gap_connect %02x:%02x:%02x:%02x:%02x:%02x (addr_type=%u)\n",
           le_remote.addr[0], le_remote.addr[1], le_remote.addr[2],
           le_remote.addr[3], le_remote.addr[4], le_remote.addr[5],
           le_remote.addr_type);
    gap_connect(le_remote.addr, le_remote.addr_type);
}

static void le_start_connect(void) {
    btstack_tlv_get_instance(&le_tlv_impl, &le_tlv_ctx);
    if (le_tlv_impl) {
        int n = le_tlv_impl->get_tag(le_tlv_ctx, TLV_TAG_HOGD,
                                     (uint8_t *)&le_remote, sizeof(le_remote));
        if (n == sizeof(le_remote)) {
            printf("[ble] bonded device found in TLV, reconnecting\n");
            le_connect_to_remote();
            return;
        }
    }
    le_start_scan();
}

static void le_connection_timeout_cb(btstack_timer_source_t *ts) {
    (void)ts;
    printf("[ble] connection timeout, cancelling and rescanning\n");
    gap_connect_cancel();
    le_start_scan();
}

static void le_reconnect_timeout_cb(btstack_timer_source_t *ts) {
    (void)ts;
    switch (le_state) {
        case LE_W4_TIMEOUT_THEN_RECONNECT: le_connect_to_remote(); break;
        case LE_W4_TIMEOUT_THEN_SCAN:      le_start_scan();        break;
        default: break;
    }
}

static void le_handle_outgoing_connection_error(void) {
    printf("[ble] outgoing connection error — disconnect + rescan\n");
    if (le_connection_handle != HCI_CON_HANDLE_INVALID)
        gap_disconnect(le_connection_handle);
    le_start_scan();
}

/* Kick off hids_client discovery once the encrypted link is up.  Called
 * from SM_EVENT_PAIRING_COMPLETE / SM_EVENT_REENCRYPTION_COMPLETE on
 * success.  hids_client_connect handles all the GATT bookkeeping —
 * REPORT_MAP, REPORT characteristics, REPORT_REFERENCE descriptors,
 * CCCD subscriptions — and emits GATTSERVICE_SUBEVENT_HID_REPORT for
 * each notification once subscribed. */
static void le_kick_hids_client(void) {
    if (le_state == LE_W4_HIDS_CONNECTED || le_state == LE_READY) {
        /* Already kicking — multiple SM events can fire on bonded
         * reconnects (RESOLVING_SUCCEEDED then REENCRYPTION_COMPLETE).
         * Idempotent guard. */
        return;
    }
    le_state = LE_W4_HIDS_CONNECTED;
    uint8_t status = hids_client_connect(le_connection_handle,
                                         &le_hids_client_event_handler,
                                         HID_PROTOCOL_MODE_REPORT,
                                         &le_hids_cid);
    if (status != ERROR_CODE_SUCCESS) {
        printf("[ble] hids_client_connect FAIL status=0x%02x\n", status);
        le_handle_outgoing_connection_error();
        return;
    }
    printf("[ble] hids_client_connect started (cid=0x%04x, report-mode)\n",
           le_hids_cid);
}

/* ---- HID input report → deskhop's boot-keyboard pipeline --------- */

static void le_handle_input_report(uint8_t service_index,
                                   const uint8_t *report, uint16_t report_len) {
    if (report_len < 1)
        return;

    /* Standard boot-keyboard layout we'll build into and forward:
     *   [0] modifier byte (8 bits, one per modifier key)
     *   [1] reserved (0)
     *   [2..7] up to 6 keycodes
     * deskhop's iface->protocol = HID_PROTOCOL_BOOT (set in setup.c)
     * makes extract_kbd_data take _extract_kbd_boot which is a straight
     * memcpy of the 8 bytes — no descriptor-parsing dependency.       */
    uint8_t boot_report[8] = {0};
    int     key_count      = 0;

    /* Iterate the report fields using the peripheral's HID descriptor
     * (stored by hids_client during its discovery phase).  Pick out
     * keyboard-page (0x07) usages, building modifier bits and keycode
     * slots in the boot format. */
    btstack_hid_parser_t parser;
    btstack_hid_parser_init(
        &parser,
        hids_client_descriptor_storage_get_descriptor_data(le_hids_cid, service_index),
        hids_client_descriptor_storage_get_descriptor_len(le_hids_cid, service_index),
        HID_REPORT_TYPE_INPUT, report, report_len);

    while (btstack_hid_parser_has_more(&parser)) {
        uint16_t usage_page, usage;
        int32_t  value;
        btstack_hid_parser_get_field(&parser, &usage_page, &usage, &value);
        if (usage_page != 0x07)
            continue;
        if (value == 0)
            continue;
        if (usage >= 0xE0 && usage <= 0xE7) {
            /* Modifier key: pack into bit (usage - 0xE0). */
            boot_report[0] |= (uint8_t)(1u << (usage - 0xE0));
            continue;
        }
        if (key_count < 6) {
            /* Regular key: place into next free keycode slot. */
            boot_report[2 + key_count++] = (uint8_t)usage;
        }
    }

    /* Forward to deskhop.  Even an all-zero report is meaningful — it's
     * the key-up notification — so don't filter. */
    if (g_bt && g_bt->process_report && g_bt->kbd_iface) {
        g_bt->process_report(boot_report, sizeof(boot_report),
                             g_bt->kbd_itf, g_bt->kbd_iface);
    }
}

/* ---- hids_client event handler ----------------------------------- */

static void le_hids_client_event_handler(uint8_t packet_type, uint16_t channel,
                                         uint8_t *packet, uint16_t size) {
    (void)packet_type;
    (void)channel;
    (void)size;

    if (hci_event_packet_get_type(packet) != HCI_EVENT_GATTSERVICE_META)
        return;

    uint8_t subevent = hci_event_gattservice_meta_get_subevent_code(packet);
    switch (subevent) {
        case GATTSERVICE_SUBEVENT_HID_SERVICE_CONNECTED: {
            uint8_t status = gattservice_subevent_hid_service_connected_get_status(packet);
            if (status != ERROR_CODE_SUCCESS) {
                printf("[ble] HID service client connect FAIL status=0x%02x\n", status);
                le_handle_outgoing_connection_error();
                break;
            }
            printf("[ble] HID service client CONNECTED (%u services) — READY for input\n",
                   gattservice_subevent_hid_service_connected_get_num_instances(packet));
            le_state = LE_READY;
            if (g_bt && g_bt->keyboard_connected)
                *g_bt->keyboard_connected = true;
            /* Persist the device address for fast direct-reconnect on
             * next boot.  Without this, we'd scan every time. */
            if (le_tlv_impl) {
                le_tlv_impl->store_tag(le_tlv_ctx, TLV_TAG_HOGD,
                                       (const uint8_t *)&le_remote,
                                       sizeof(le_remote));
            }
            break;
        }

        case GATTSERVICE_SUBEVENT_HID_SERVICE_DISCONNECTED:
            printf("[ble] HID service client disconnected\n");
            /* The LE link teardown follows; HCI_EVENT_DISCONNECTION_COMPLETE
             * in our HCI handler then resets state and re-scans. */
            break;

        case GATTSERVICE_SUBEVENT_HID_REPORT:
            le_handle_input_report(
                gattservice_subevent_hid_report_get_service_index(packet),
                gattservice_subevent_hid_report_get_report(packet),
                gattservice_subevent_hid_report_get_report_len(packet));
            break;

        default:
            printf("[ble] unhandled gattservice subevent=0x%02x\n", subevent);
            break;
    }
}

/* ---- SM packet handler: pairing exchange ------------------------- */

static void le_sm_packet_handler(uint8_t packet_type, uint16_t channel,
                                 uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;

    if (packet_type != HCI_EVENT_PACKET)
        return;

    bool security_up = false;

    switch (hci_event_packet_get_type(packet)) {
        case SM_EVENT_JUST_WORKS_REQUEST:
            printf("[ble] SSP just-works request — auto-confirm\n");
            sm_just_works_confirm(sm_event_just_works_request_get_handle(packet));
            break;

        case SM_EVENT_NUMERIC_COMPARISON_REQUEST:
            printf("[ble] numeric comparison request — auto-confirm\n");
            sm_numeric_comparison_confirm(
                sm_event_numeric_comparison_request_get_handle(packet));
            break;

        case SM_EVENT_IDENTITY_RESOLVING_STARTED:
            printf("[ble] identity resolving started (peer using RPA)\n");
            break;

        case SM_EVENT_IDENTITY_RESOLVING_FAILED:
            printf("[ble] identity resolving FAILED (no bond — first-pair path)\n");
            break;

        case SM_EVENT_IDENTITY_RESOLVING_SUCCEEDED:
            printf("[ble] identity resolving SUCCEEDED (matched stored bond)\n");
            break;

        case SM_EVENT_REENCRYPTION_STARTED:
            printf("[ble] re-encryption started\n");
            break;

        case SM_EVENT_REENCRYPTION_COMPLETE:
            printf("[ble] re-encryption complete — using stored bond\n");
            security_up = true;
            break;

        case SM_EVENT_PAIRING_COMPLETE: {
            uint8_t status = sm_event_pairing_complete_get_status(packet);
            switch (status) {
                case ERROR_CODE_SUCCESS:
                    printf("[ble] pairing complete: SUCCESS\n");
                    security_up = true;
                    break;
                case ERROR_CODE_CONNECTION_TIMEOUT:
                    printf("[ble] pairing FAIL: timeout\n");
                    break;
                case ERROR_CODE_REMOTE_USER_TERMINATED_CONNECTION:
                    printf("[ble] pairing FAIL: remote terminated\n");
                    break;
                case ERROR_CODE_AUTHENTICATION_FAILURE:
                    printf("[ble] pairing FAIL: authentication (reason=%u)\n",
                           sm_event_pairing_complete_get_reason(packet));
                    break;
                default:
                    printf("[ble] pairing FAIL: status=0x%02x\n", status);
                    break;
            }
            break;
        }

        default:
            printf("[ble] unhandled SM event type=0x%02x\n",
                   hci_event_packet_get_type(packet));
            break;
    }

    if (security_up) {
        /* Encrypted link is up.  Now kick off the HID service client
         * — its GATT discovery is allowed to run on the now-encrypted
         * link, and the 8BitDo (and similar BLE-only keyboards) will
         * happily respond to characteristic + descriptor queries that
         * would otherwise have been rejected. */
        le_kick_hids_client();
    }
}

/* ---- HCI / GAP packet handler ----------------------------------- */

static void le_packet_handler(uint8_t packet_type, uint16_t channel,
                              uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;

    if (packet_type != HCI_EVENT_PACKET)
        return;

    uint8_t event_type = hci_event_packet_get_type(packet);
    switch (event_type) {
        case BTSTACK_EVENT_STATE:
            if (btstack_event_state_get_state(packet) != HCI_STATE_WORKING)
                break;
            if (le_state != LE_W4_WORKING)
                break;
            printf("[ble] HCI_STATE_WORKING\n");
            le_start_connect();
            break;

        case GAP_EVENT_ADVERTISING_REPORT: {
            if (le_state != LE_W4_HID_DEVICE_FOUND)
                break;
            if (!le_adv_contains_hid_service(packet))
                break;
            gap_stop_scan();
            gap_event_advertising_report_get_address(packet, le_remote.addr);
            le_remote.addr_type = gap_event_advertising_report_get_address_type(packet);
            printf("[ble] HID adv from %02x:%02x:%02x:%02x:%02x:%02x (addr_type=%u)\n",
                   le_remote.addr[0], le_remote.addr[1], le_remote.addr[2],
                   le_remote.addr[3], le_remote.addr[4], le_remote.addr[5],
                   le_remote.addr_type);
            le_connect_to_remote();
            break;
        }

        case HCI_EVENT_META_GAP:
            if (hci_event_gap_meta_get_subevent_code(packet) != GAP_SUBEVENT_LE_CONNECTION_COMPLETE)
                break;
            if (le_state != LE_W4_CONNECTED)
                break;
            btstack_run_loop_remove_timer(&le_connection_timer);
            le_connection_handle =
                gap_subevent_le_connection_complete_get_connection_handle(packet);
            /* Connection is up but not yet encrypted.  Wait for the SM
             * exchange (just-works pairing on first connect, or
             * re-encryption from stored LTK on subsequent connects) to
             * complete, then kick hids_client.  gatt_client_set_required_
             * security_level(LEVEL_2) from init triggers pairing
             * automatically if hids_client's GATT queries hit
             * insufficient-encryption errors, but in practice the
             * peripheral initiates security itself or BTstack handles
             * it as part of the bond-resume flow. */
            le_state = LE_W4_ENCRYPTED;
            printf("[ble] LE connection complete (handle 0x%04x) — waiting for security\n",
                   le_connection_handle);
            sm_request_pairing(le_connection_handle);
            break;

        case HCI_EVENT_DISCONNECTION_COMPLETE: {
            if (le_connection_handle == HCI_CON_HANDLE_INVALID)
                break;
            hci_con_handle_t h = hci_event_disconnection_complete_get_connection_handle(packet);
            if (h != le_connection_handle)
                break;
            printf("[ble] LE disconnected (state was %d) — back to scan/reconnect loop\n",
                   (int)le_state);
            le_connection_handle = HCI_CON_HANDLE_INVALID;
            if (g_bt && g_bt->keyboard_connected)
                *g_bt->keyboard_connected = false;
            switch (le_state) {
                case LE_READY:
                    le_state = LE_W4_TIMEOUT_THEN_RECONNECT;
                    break;
                default:
                    le_state = LE_W4_TIMEOUT_THEN_SCAN;
                    break;
            }
            btstack_run_loop_set_timer(&le_connection_timer, 100);
            btstack_run_loop_set_timer_handler(&le_connection_timer, &le_reconnect_timeout_cb);
            btstack_run_loop_add_timer(&le_connection_timer);
            break;
        }

        default: break;
    }
}

/* ---- public init ------------------------------------------------- */

void bt_hid_host_le_init(bt_hid_state_t *bt_state) {
    g_bt = bt_state;

    /* Security Manager — just-works pairing, permissive AuthReq (peripheral
     * dictates).  See earlier commits in #29 for the rationale: requesting
     * SC up-front caused the 8BitDo to time out the pairing exchange. */
    sm_init();
    sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    sm_set_authentication_requirements(SM_AUTHREQ_BONDING);

    /* GATT client base + security policy: BTstack auto-triggers pairing
     * when an op needs higher security than the current connection has. */
    gatt_client_init();
    gatt_client_set_required_security_level(LEVEL_2);

    /* HID Service client.  Stores per-service report descriptors here;
     * we read them back during report parsing to drive btstack_hid_parser. */
    hids_client_init(le_hid_descriptor_storage, sizeof(le_hid_descriptor_storage));

    /* HCI / GAP events. */
    le_hci_cb.callback = &le_packet_handler;
    hci_add_event_handler(&le_hci_cb);

    /* SM events. */
    le_sm_cb.callback = &le_sm_packet_handler;
    sm_add_event_handler(&le_sm_cb);

    printf("[ble] handlers registered (hids_client, report-mode), awaiting HCI_STATE_WORKING\n");
}

#endif /* DH_BT_HID_HOST_KBD */
