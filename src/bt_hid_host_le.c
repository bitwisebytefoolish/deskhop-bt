#include "bt/btstack_config.h"

#ifdef DH_BT_HID_HOST_KBD

/* BLE HID-over-GATT Profile (HOGP) host — peer to bt_hid_host.c.  See
 * src/include/bt_hid_host_le.h for the architectural rationale and #29
 * for the use-case motivation (8BitDo Retro and other BLE-only keyboards).
 *
 * The state-machine and packet-handler shape mirrors BTstack's reference
 * example pico-sdk/lib/btstack/example/hog_boot_host_demo.c, adapted to:
 *   - keyboard-only (skip mouse boot characteristic),
 *   - feed reports through deskhop's bt_hid_state_t.process_report so
 *     they enter the same kbd_queue → tud_hid / UART pipeline used by
 *     the Classic path,
 *   - log every state transition over the debug UART (#26) so the
 *     pairing flow is observable in real time. */

/* BTstack-only includes — see bt_hid_host.c for why we avoid btstack.h. */
#include "bluetooth.h"
#include "bluetooth_gatt.h"
#include "hci.h"
#include "l2cap.h"
#include "gap.h"
#include "btstack_event.h"
#include "btstack_util.h"
#include "ble/sm.h"
#include "ble/gatt_client.h"
#include "ble/att_db.h"
#include "ad_parser.h"
#include "btstack_tlv.h"     /* bond storage via TLV (reuses pico_btstack_flash_bank) */

#include <stdio.h>   /* printf — routed to stdio_uart on UART1 / GP4, see #26 */
#include <string.h>

#include "bt_hid_host_le.h"

/* ---- Shared state -------------------------------------------------- */

static bt_hid_state_t *g_bt;

/* ---- LE-specific app state machine -------------------------------- */

typedef enum {
    LE_W4_WORKING,                /* wait for HCI_STATE_WORKING */
    LE_W4_HID_DEVICE_FOUND,       /* scanning, waiting for HID adv */
    LE_W4_CONNECTED,              /* gap_connect issued, waiting for LE-conn-complete */
    LE_W4_ENCRYPTED,              /* sm_request_pairing issued */
    LE_W4_HID_SERVICE_FOUND,      /* discovering primary HID service */
    LE_W4_HID_CHARACTERISTICS,    /* discovering BOOT_KEYBOARD_INPUT_REPORT + PROTOCOL_MODE */
    LE_W4_BOOT_KEYBOARD_ENABLED,  /* writing CCCD to enable notifications */
    LE_READY,                     /* notifications flowing */
    LE_W4_TIMEOUT_THEN_SCAN,
    LE_W4_TIMEOUT_THEN_RECONNECT,
} le_app_state_t;

static le_app_state_t le_state = LE_W4_WORKING;

/* Remote device (the BLE keyboard we're connecting to). */
typedef struct {
    bd_addr_t      addr;
    bd_addr_type_t addr_type;
} le_device_addr_t;

static le_device_addr_t       le_remote;
static hci_con_handle_t       le_connection_handle = HCI_CON_HANDLE_INVALID;

/* GATT-client query state. */
static gatt_client_service_t        hid_service;
static gatt_client_characteristic_t protocol_mode_characteristic;
static gatt_client_characteristic_t boot_keyboard_input_characteristic;
static gatt_client_notification_t   keyboard_notifications;

/* Connection / reconnect timer. */
static btstack_timer_source_t le_connection_timer;

/* Packet-callback registrations.  These structs must outlive registration
 * since hci_add_event_handler stores a pointer; file-scope is fine. */
static btstack_packet_callback_registration_t le_hci_cb;
static btstack_packet_callback_registration_t le_sm_cb;

/* Bond storage: persisted across reboots via the same TLV that backs the
 * Classic link-key DB.  Survives power cycles; cleared when the user
 * forgets the device through #22's LCD UI (eventually) or by a flash erase. */
#define TLV_TAG_HOGD ((((uint32_t)'H') << 24) | (((uint32_t)'O') << 16) | (((uint32_t)'G') << 8) | 'D')
static const btstack_tlv_t *le_tlv_impl;
static void                *le_tlv_ctx;

/* ---- Forward declarations ----------------------------------------- */

static void le_start_scan(void);
static void le_start_connect(void);
static void le_connect_to_remote(void);
static void le_connection_timeout_cb(btstack_timer_source_t *ts);
static void le_reconnect_timeout_cb(btstack_timer_source_t *ts);
static void le_handle_outgoing_connection_error(void);

static void le_packet_handler(uint8_t packet_type, uint16_t channel,
                              uint8_t *packet, uint16_t size);
static void le_sm_packet_handler(uint8_t packet_type, uint16_t channel,
                                 uint8_t *packet, uint16_t size);
static void le_gatt_client_event_handler(uint8_t packet_type, uint16_t channel,
                                         uint8_t *packet, uint16_t size);
static void le_handle_notification(uint8_t packet_type, uint16_t channel,
                                   uint8_t *packet, uint16_t size);

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
    /* Passive scan, 100% duty cycle: scan_interval == scan_window == 48
     * (30 ms / 30 ms).  Same values as the BTstack reference. */
    gap_set_scan_parameters(0, 48, 48);
    gap_start_scan();
}

static void le_connect_to_remote(void) {
    /* Connection-establishment timeout: 10 s.  If the peripheral doesn't
     * complete LE connect within this, cancel and fall back to scanning. */
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
    /* On startup: if we have a bonded device, try reconnecting to it
     * directly; otherwise scan for a new one. */
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
    if (le_connection_handle != HCI_CON_HANDLE_INVALID) {
        gap_disconnect(le_connection_handle);
    }
    le_start_scan();
}

/* ---- Notification handler: HID input report arrived --------------- */

static void le_handle_notification(uint8_t packet_type, uint16_t channel,
                                   uint8_t *packet, uint16_t size) {
    (void)packet_type;
    (void)channel;
    (void)size;

    if (hci_event_packet_get_type(packet) != GATT_EVENT_NOTIFICATION)
        return;

    const uint8_t *value     = gatt_event_notification_get_value(packet);
    uint16_t       value_len = gatt_event_notification_get_value_length(packet);

    if (!g_bt || !g_bt->process_report || !g_bt->kbd_iface)
        return;

    /* HOGP boot-keyboard notifications carry the same 8-byte format as
     * Classic boot-mode reports — modifier + reserved + 6 keycodes.  No
     * L2CAP transaction header to strip (that's a Classic-side concern).
     * Hand straight to deskhop's process_keyboard_report; _extract_kbd_boot
     * does the rest (iface->protocol = 0 / HID_PROTOCOL_BOOT, set in
     * setup.c before bt_hid_host_init). */
    g_bt->process_report((uint8_t *)value, (int)value_len,
                         g_bt->kbd_itf, g_bt->kbd_iface);
}

/* ---- GATT client state machine: service + char discovery + CCCD -- */

static void le_gatt_client_event_handler(uint8_t packet_type, uint16_t channel,
                                         uint8_t *packet, uint16_t size) {
    (void)packet_type;
    (void)channel;
    (void)size;

    gatt_client_characteristic_t characteristic;
    uint8_t  att_status;
    /* HID protocol mode 0 = BOOT (per HOGP spec).  We write this to the
     * peripheral's PROTOCOL_MODE characteristic to switch it into boot
     * report format, matching the Classic-side BOOT mode choice. */
    static uint8_t boot_protocol_mode = 0;

    switch (le_state) {
        case LE_W4_HID_SERVICE_FOUND:
            switch (hci_event_packet_get_type(packet)) {
                case GATT_EVENT_SERVICE_QUERY_RESULT:
                    gatt_event_service_query_result_get_service(packet, &hid_service);
                    break;
                case GATT_EVENT_QUERY_COMPLETE:
                    att_status = gatt_event_query_complete_get_att_status(packet);
                    if (att_status != ATT_ERROR_SUCCESS) {
                        printf("[ble] HID service discovery FAIL att=0x%02x\n", att_status);
                        le_handle_outgoing_connection_error();
                        break;
                    }
                    printf("[ble] HID service discovered, finding characteristics\n");
                    le_state = LE_W4_HID_CHARACTERISTICS;
                    gatt_client_discover_characteristics_for_service(
                        &le_gatt_client_event_handler,
                        le_connection_handle, &hid_service);
                    break;
                default: break;
            }
            break;

        case LE_W4_HID_CHARACTERISTICS:
            switch (hci_event_packet_get_type(packet)) {
                case GATT_EVENT_CHARACTERISTIC_QUERY_RESULT:
                    gatt_event_characteristic_query_result_get_characteristic(packet, &characteristic);
                    switch (characteristic.uuid16) {
                        case ORG_BLUETOOTH_CHARACTERISTIC_BOOT_KEYBOARD_INPUT_REPORT:
                            printf("[ble] found BOOT_KEYBOARD_INPUT_REPORT (handle 0x%04x)\n",
                                   characteristic.value_handle);
                            memcpy(&boot_keyboard_input_characteristic, &characteristic,
                                   sizeof(characteristic));
                            break;
                        case ORG_BLUETOOTH_CHARACTERISTIC_PROTOCOL_MODE:
                            printf("[ble] found PROTOCOL_MODE\n");
                            memcpy(&protocol_mode_characteristic, &characteristic,
                                   sizeof(characteristic));
                            break;
                        default:
                            /* ignore boot-mouse + report-mode characteristics */
                            break;
                    }
                    break;
                case GATT_EVENT_QUERY_COMPLETE:
                    att_status = gatt_event_query_complete_get_att_status(packet);
                    if (att_status != ATT_ERROR_SUCCESS) {
                        printf("[ble] characteristic discovery FAIL att=0x%02x\n", att_status);
                        le_handle_outgoing_connection_error();
                        break;
                    }
                    printf("[ble] enabling notifications on BOOT_KEYBOARD_INPUT_REPORT\n");
                    le_state = LE_W4_BOOT_KEYBOARD_ENABLED;
                    gatt_client_write_client_characteristic_configuration(
                        &le_gatt_client_event_handler, le_connection_handle,
                        &boot_keyboard_input_characteristic,
                        GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NOTIFICATION);
                    break;
                default: break;
            }
            break;

        case LE_W4_BOOT_KEYBOARD_ENABLED:
            switch (hci_event_packet_get_type(packet)) {
                case GATT_EVENT_QUERY_COMPLETE:
                    att_status = gatt_event_query_complete_get_att_status(packet);
                    if (att_status != ATT_ERROR_SUCCESS) {
                        printf("[ble] CCCD write FAIL att=0x%02x\n", att_status);
                        le_handle_outgoing_connection_error();
                        break;
                    }
                    /* Register our notification listener and switch the
                     * peripheral into boot-mode reporting. */
                    gatt_client_listen_for_characteristic_value_updates(
                        &keyboard_notifications, &le_handle_notification,
                        le_connection_handle, &boot_keyboard_input_characteristic);
                    printf("[ble] writing PROTOCOL_MODE = BOOT (0)\n");
                    gatt_client_write_value_of_characteristic_without_response(
                        le_connection_handle, protocol_mode_characteristic.value_handle,
                        1, &boot_protocol_mode);
                    /* Persist this device as the preferred reconnect target. */
                    if (le_tlv_impl) {
                        le_tlv_impl->store_tag(le_tlv_ctx, TLV_TAG_HOGD,
                                               (const uint8_t *)&le_remote, sizeof(le_remote));
                    }
                    le_state = LE_READY;
                    if (g_bt && g_bt->keyboard_connected)
                        *g_bt->keyboard_connected = true;
                    printf("[ble] READY — keystrokes flowing\n");
                    break;
                default: break;
            }
            break;

        default: break;
    }
}

/* ---- SM (Security Manager) packet handler: pairing exchange ------ */

static void le_sm_packet_handler(uint8_t packet_type, uint16_t channel,
                                 uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;

    if (packet_type != HCI_EVENT_PACKET)
        return;

    bool proceed_to_service_discovery = false;

    switch (hci_event_packet_get_type(packet)) {
        case SM_EVENT_JUST_WORKS_REQUEST:
            printf("[ble] SSP just-works request — auto-confirm\n");
            sm_just_works_confirm(sm_event_just_works_request_get_handle(packet));
            break;

        case SM_EVENT_NUMERIC_COMPARISON_REQUEST:
            /* The deskhop has no display — we accept whatever number the
             * peripheral suggests.  Not maximally secure but matches our
             * "no display, no keyboard input" IO capability. */
            printf("[ble] numeric comparison request — auto-confirm\n");
            sm_numeric_comparison_confirm(
                sm_event_numeric_comparison_request_get_handle(packet));
            break;

        case SM_EVENT_PAIRING_COMPLETE: {
            uint8_t status = sm_event_pairing_complete_get_status(packet);
            switch (status) {
                case ERROR_CODE_SUCCESS:
                    printf("[ble] pairing complete: SUCCESS\n");
                    proceed_to_service_discovery = true;
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

        case SM_EVENT_REENCRYPTION_COMPLETE:
            /* This fires on the path where a previously-bonded device
             * reconnects and the existing LTK is used to re-encrypt
             * without a fresh pairing exchange.  Treat it as success. */
            printf("[ble] re-encryption complete — using stored bond\n");
            proceed_to_service_discovery = true;
            break;

        case SM_EVENT_IDENTITY_RESOLVING_STARTED:
            /* Peer used a Resolvable Private Address; BTstack is trying
             * to resolve it against our LE device DB.  Informational. */
            printf("[ble] identity resolving started (peer using RPA)\n");
            break;

        case SM_EVENT_IDENTITY_RESOLVING_FAILED:
            /* RPA could not be resolved — peer is unknown (no bond yet)
             * or its IRK doesn't match anything in our DB.  Expected on
             * first-pair; on subsequent pairs the IRK is stored and this
             * event becomes SM_EVENT_IDENTITY_RESOLVING_SUCCEEDED. */
            printf("[ble] identity resolving FAILED (no bond — first-pair path)\n");
            break;

        default:
            /* Diagnostic: log any SM event we don't explicitly handle.
             * Helps identify cases like SM_EVENT_PAIRING_STARTED or
             * passkey display where the peer expects us to do something
             * we're not.  Remove or gate behind a debug flag once the
             * SM exchange is stable on the K7 + 8BitDo. */
            printf("[ble] unhandled SM event type=0x%02x\n",
                   hci_event_packet_get_type(packet));
            break;
    }

    /* No explicit re-issue of GATT discovery here.  Since we removed the
     * sm_request_pairing() call on LE_CONNECTION_COMPLETE, GATT discovery
     * starts immediately and BTstack pauses it internally if/when the
     * peripheral demands encryption.  Once SM_EVENT_PAIRING_COMPLETE or
     * SM_EVENT_REENCRYPTION_COMPLETE fires, BTstack auto-resumes the
     * paused GATT query — we don't need to do anything.  The
     * `proceed_to_service_discovery` flag becomes a pure log assertion. */
    if (proceed_to_service_discovery) {
        printf("[ble] security elevated — pending GATT query should resume\n");
    }
}

/* ---- HCI / GAP packet handler: state, advertising, connect, disc -- */

static void le_packet_handler(uint8_t packet_type, uint16_t channel,
                              uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;

    if (packet_type != HCI_EVENT_PACKET)
        return;

    uint8_t event_type = hci_event_packet_get_type(packet);
    switch (event_type) {
        case BTSTACK_EVENT_STATE:
            /* BTSTACK_EVENT_STATE → HCI_STATE_WORKING also reaches the
             * Classic packet handler in bt_hid_host.c, which starts
             * Classic inquiry there.  Here we additionally kick off the
             * BLE side: either scan for new peripherals or reconnect to
             * the last-bonded one. */
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
            /* Stop scanning and connect.  We log every advertising report
             * that contains the HID service UUID — useful when there are
             * multiple BLE keyboards in range and we want to confirm
             * which one we latched onto. */
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
            /* Skip the explicit sm_request_pairing() — earlier attempt timed
             * out with the 8BitDo Retro (no SM events ever arrived).  Some
             * BLE peripherals only respond to security elevation when it's
             * triggered organically by a GATT operation that requires it.
             * Jump straight to GATT service discovery instead; BTstack
             * automatically initiates pairing when the peripheral rejects
             * an unauthenticated read with "Insufficient Authentication".
             * SM event handler still catches the resulting just-works /
             * numeric-comparison / pairing-complete events. */
            printf("[ble] LE connection complete (handle 0x%04x) — discovering HID service (security follows automatically if required)\n",
                   le_connection_handle);
            le_state = LE_W4_HID_SERVICE_FOUND;
            gatt_client_discover_primary_services_by_uuid16(
                &le_gatt_client_event_handler, le_connection_handle,
                ORG_BLUETOOTH_SERVICE_HUMAN_INTERFACE_DEVICE);
            break;

        case HCI_EVENT_DISCONNECTION_COMPLETE:
            /* Filter: only react if THIS is our connection.  Classic
             * disconnects also produce this event but with a different
             * handle, and we don't want to clobber Classic state. */
            if (le_connection_handle == HCI_CON_HANDLE_INVALID)
                break;
            {
                hci_con_handle_t h = hci_event_disconnection_complete_get_connection_handle(packet);
                if (h != le_connection_handle)
                    break;
            }
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

        default: break;
    }
}

/* ---- public init --------------------------------------------------- */

void bt_hid_host_le_init(bt_hid_state_t *bt_state) {
    g_bt = bt_state;

    /* Security Manager: just-works pairing, no display, no input.
     * AuthReq is intentionally permissive — only request BONDING (so the
     * LTK is stored for fast re-encryption on reconnect), and let the
     * peripheral dictate the rest.  Initial implementation requested
     * SM_AUTHREQ_SECURE_CONNECTION too, but the 8BitDo Retro Mechanical
     * Keyboard timed out during pairing under that policy — likely a
     * negotiation mismatch.  Removing the SC requirement makes us
     * compatible with both legacy and SC-capable peripherals; the
     * peripheral's own AuthReq still applies, so SC happens when both
     * sides support it (which is usually the case for 2018+ devices). */
    sm_init();
    sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    sm_set_authentication_requirements(SM_AUTHREQ_BONDING);

    /* GATT client — required to discover services + characteristics on
     * the peripheral and to subscribe for notifications.  Set the
     * required security level to LEVEL_2 (encrypted + bonded with no
     * MITM, suitable for just-works pairing): BTstack will auto-initiate
     * pairing whenever a GATT operation needs higher security than the
     * current connection has.  This is the cleaner trigger than an
     * explicit sm_request_pairing() call — the 8BitDo Retro doesn't
     * respond to out-of-the-blue pairing requests, but it DOES respond
     * to security elevation arising organically from a GATT op. */
    gatt_client_init();
    gatt_client_set_required_security_level(LEVEL_2);

    /* Register for HCI / GAP events (BTSTACK_EVENT_STATE, advertising
     * reports, LE connection-complete, disconnection-complete). */
    le_hci_cb.callback = &le_packet_handler;
    hci_add_event_handler(&le_hci_cb);

    /* Register for SM events (pairing exchange + re-encryption). */
    le_sm_cb.callback = &le_sm_packet_handler;
    sm_add_event_handler(&le_sm_cb);

    /* NOTE: do NOT call hci_power_control(HCI_POWER_ON) here.  The
     * Classic init (bt_hid_host_init) does it after both transports
     * have registered their handlers, so they come online together. */
    printf("[ble] handlers registered, awaiting HCI_STATE_WORKING\n");
}

#endif /* DH_BT_HID_HOST_KBD */
