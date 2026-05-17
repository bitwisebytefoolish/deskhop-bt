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

#include "pico/time.h"

#include "bt_hid_host.h"

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

/* ── Sticky stage indicator ───────────────────────────────────────────────
 *
 * Events arrive faster than a blink can play out (an entire failed pairing
 * cycle takes ~100 ms), so a one-shot blink-per-event design just clobbers
 * its predecessor.  Instead we maintain a single "current stage" variable,
 * and bt_hid_host_stage_tick() — driven from the core0 task scheduler at
 * 30 Hz — continuously loops the LED through "N flashes, pause, N flashes,
 * pause, …" where N is the stage number.
 *
 * The LED at any moment reflects the latest BT state.  If the pairing
 * code is looping in failure, the stage stays at FAILED (7 flashes
 * repeating).  If everything works, the stage settles at CONNECTED (4).
 *
 * 1 = radio up           — HCI reached HCI_STATE_WORKING
 * 2 = keyboard found     — inquiry returned a peripheral+keyboard CoD
 * 3 = connecting         — inquiry done, calling hid_host_connect()
 * 4 = connected          — HID_SUBEVENT_CONNECTION_OPENED status=SUCCESS,
 *                          L2CAP control + interrupt channels open
 * 5 = descriptor parsed  — HID_SUBEVENT_DESCRIPTOR_AVAILABLE arrived with
 *                          status=SUCCESS and parse_descriptor() ran;
 *                          report routing is now ARMED
 * 6 = input flowing      — first HID_SUBEVENT_REPORT was received and
 *                          forwarded to process_keyboard_report; getting
 *                          stuck here means the BT side is healthy but
 *                          the deskhop routing pipeline (kbd_queue → UART
 *                          or → tud_hid) isn't delivering to the host
 * 7 = connect failed     — hid_host_connect() returned error, OR
 *                          HID_SUBEVENT_CONNECTION_OPENED status!=SUCCESS,
 *                          OR HID_SUBEVENT_DESCRIPTOR_AVAILABLE returned
 *                          a non-success status (now also captured)
 *
 * 7 is chosen to skip 5 and 6 — easy to distinguish by eye from 4.
 *
 * When stage == FAILED, a *second* blink cycle follows the stage cycle:
 * the BTstack error-code byte (g_status_code) is blinked out, separated
 * from the stage by a 1 s gap, with a 2 s gap before the loop restarts.
 * Two distinct gap lengths so you can tell where the cycle wraps:
 *
 *   [7 flashes] [1 s] [N flashes] [2 s] [7 flashes] [1 s] [N flashes] …
 *                                  ^^^ end-of-cycle marker
 *
 * Common codes you might see (decimal):
 *    4 = HCI page timeout            (keyboard didn't respond)
 *    5 = HCI authentication failure
 *    6 = HCI PIN or key missing
 *    8 = HCI connection timeout
 *   12 = BTstack ERROR_CODE_COMMAND_DISALLOWED (state-machine reentry)
 *   14 = HCI rejected, security reasons
 *   19 = HCI remote terminated
 *   22 = HCI local terminated                                          */
typedef enum {
    BT_STAGE_IDLE        = 0,
    BT_STAGE_RADIO_UP    = 1,
    BT_STAGE_DISCOVERED  = 2,
    BT_STAGE_CONNECTING  = 3,
    BT_STAGE_CONNECTED   = 4,
    BT_STAGE_DESCRIPTOR  = 5,
    BT_STAGE_INPUT       = 6,
    BT_STAGE_FAILED      = 7,
} bt_stage_t;

/* Blink-pattern timing (microseconds).  Five distinct LED-OFF durations,
 * each at least 2x the previous so they're unambiguous by eye:
 *
 *   FLASH_OFF  (120 ms): between flashes within a tally group
 *   GROUP_GAP  (800 ms): between tally groups of 5 flashes within a phase
 *   MID_GAP    (1.5 s) : between the stage flashes and the status flashes
 *                        (only present when stage == FAILED)
 *   CYCLE_GAP  (2.5 s) : end-of-cycle, before the pattern repeats
 *
 * Status codes can reach 30+ which is uncountable as a single run; the
 * tally-group grouping makes any value up to ~80 readable: count groups
 * of 5, add the trailing partial group.  E.g. 22 displays as
 * 5—5—5—5—2 = "20 + 2". */
#define FLASH_ON_US      120000
#define FLASH_OFF_US     120000
#define GROUP_GAP_US     800000
#define MID_GAP_US      1500000
#define CYCLE_GAP_US    2500000
#define GROUP_SIZE            5

static volatile bt_stage_t g_stage       = BT_STAGE_IDLE;
static volatile uint8_t    g_status_code = 0; /* last BTstack/HCI status on failure */

/* While non-zero, the BT debug indicator owns the on-board LED.  Other
 * deskhop LED writers (blink_led, restore_leds) early-return on this so
 * they don't race with our state machine.  Visible to led.c via an
 * extern declaration; cleared when this file isn't built in. */
volatile bool bt_hid_host_owns_led = false;

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

/* Set the current BT stage.  The continuous-loop driver (stage_tick) is
 * what actually paints it to the LED — we just record the new value here.
 * Also flips the cross-module "we own the LED" guard the first time we
 * leave the IDLE state, so deskhop's other LED writers stop racing us. */
static inline void set_stage(bt_stage_t stage) {
    g_stage = stage;
    if (stage != BT_STAGE_IDLE)
        bt_hid_host_owns_led = true;
}

static void start_inquiry(void) {
    g_keyboard_found = false;
    g_state = BT_STATE_INQUIRING;
    gap_inquiry_start(INQUIRY_DURATION_UNITS);
}

static void connect_to_keyboard(void) {
    g_state = BT_STATE_CONNECTING;
    set_stage(BT_STAGE_CONNECTING);
    /* HID_PROTOCOL_MODE_BOOT (not REPORT): BTstack negotiates with the
     * keyboard to send fixed-format 8-byte boot reports (modifier byte +
     * reserved + 6 keycodes) — same shape as the original USB BIOS
     * keyboard.  This bypasses descriptor-based parsing in deskhop's
     * extract_kbd_data, which is what was silently producing all-zero
     * keycode arrays (BT layer reaching stage 6 with reports flowing,
     * yet nothing landing at the host PC).  Boot-mode reports always
     * route to _extract_kbd_boot regardless of descriptor parsing —
     * iface->protocol = 0 (set in setup.c) selects that path.        */
    uint8_t status = hid_host_connect(g_keyboard_addr,
                                      HID_PROTOCOL_MODE_BOOT,
                                      &g_hid_cid);
    if (status != ERROR_CODE_SUCCESS) {
        g_status_code = status;  /* SYNCHRONOUS failure code */
        set_stage(BT_STAGE_FAILED);
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
            if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
                /* Radio up.  Kick off inquiry — keyboards in pairing mode
                 * are peripherals, they don't initiate; the host scans. */
                set_stage(BT_STAGE_RADIO_UP);
                start_inquiry();
            }
            break;

        case HCI_EVENT_INQUIRY_RESULT:
        case HCI_EVENT_INQUIRY_RESULT_WITH_RSSI: {
            bd_addr_t addr;
            uint32_t  cod;

            if (event_type == HCI_EVENT_INQUIRY_RESULT) {
                hci_event_inquiry_result_get_bd_addr(packet, addr);
                cod = hci_event_inquiry_result_get_class_of_device(packet);
            } else {
                hci_event_inquiry_result_with_rssi_get_bd_addr(packet, addr);
                cod = hci_event_inquiry_result_with_rssi_get_class_of_device(packet);
            }

            if (g_keyboard_found)
                break;

            bool is_peripheral = (cod & COD_MAJOR_MASK) == COD_MAJOR_PERIPHERAL;
            bool has_keyboard  = (cod & COD_MINOR_MASK) & COD_MINOR_KEYBOARD;

            if (is_peripheral && has_keyboard) {
                bd_addr_copy(g_keyboard_addr, addr);
                g_keyboard_found = true;
                set_stage(BT_STAGE_DISCOVERED);
                gap_inquiry_stop();
            }
            break;
        }

        /* Two distinct events both mean "inquiry is over":
         *   HCI_EVENT_INQUIRY_COMPLETE (0x01) — chip-emitted, fires when
         *     the inquiry timer expires naturally.
         *   GAP_EVENT_INQUIRY_COMPLETE (0xDD) — BTstack-synthesised,
         *     fires after gap_inquiry_stop() once the chip ACKs the
         *     INQUIRY_CANCEL command (hci.c line ~2948–2951).
         * We always call gap_inquiry_stop() as soon as we see a
         * matching keyboard — so in practice we receive the GAP_ form.
         * Handle both: missing the GAP_ case was the bug that left us
         * stuck after the 4-flash "discovered" signal forever. */
        case HCI_EVENT_INQUIRY_COMPLETE:
        case GAP_EVENT_INQUIRY_COMPLETE:
            if (g_keyboard_found)
                connect_to_keyboard();
            else
                start_inquiry(); /* nothing found — sweep again */
            break;

        /* SSP just-works: auto-confirm numeric comparison without user input. */
        case HCI_EVENT_USER_CONFIRMATION_REQUEST: {
            bd_addr_t ssp_addr;
            hci_event_user_confirmation_request_get_bd_addr(packet, ssp_addr);
            gap_ssp_confirmation_response(ssp_addr);
            break;
        }

        /* Legacy PIN pairing: reply with "0000" so older keyboards can pair. */
        case HCI_EVENT_PIN_CODE_REQUEST: {
            bd_addr_t pin_addr;
            hci_event_pin_code_request_get_bd_addr(packet, pin_addr);
            gap_pin_code_response(pin_addr, "0000");
            break;
        }

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
                        g_status_code = status; /* ASYNC failure code */
                        set_stage(BT_STAGE_FAILED);
                        g_state = BT_STATE_IDLE;
                        start_inquiry();
                        break;
                    }
                    g_hid_cid                 = hid_subevent_connection_opened_get_hid_cid(packet);
                    g_state                   = BT_STATE_CONNECTED;
                    g_descriptor_valid        = false;
                    *g_bt->keyboard_connected = true;
                    set_stage(BT_STAGE_CONNECTED);
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
                    if (status != ERROR_CODE_SUCCESS) {
                        /* Surface the descriptor fetch error via the FAILED
                         * stage + status code rather than silently dropping. */
                        g_status_code = status;
                        set_stage(BT_STAGE_FAILED);
                        break;
                    }
                    if (!g_bt->parse_descriptor || !g_bt->kbd_iface)
                        break;

                    uint16_t       cid  = hid_subevent_descriptor_available_get_hid_cid(packet);
                    const uint8_t *desc = hid_descriptor_storage_get_descriptor_data(cid);
                    uint16_t       len  = hid_descriptor_storage_get_descriptor_len(cid);

                    g_bt->parse_descriptor(g_bt->kbd_iface, desc, (int)len);
                    g_descriptor_valid = true;
                    set_stage(BT_STAGE_DESCRIPTOR);
                    break;
                }

                case HID_SUBEVENT_REPORT: {
                    if (!g_descriptor_valid)
                        break;
                    if (!g_bt->process_report || !g_bt->kbd_iface)
                        break;

                    const uint8_t *report = hid_subevent_report_get_report(packet);
                    uint16_t       len    = hid_subevent_report_get_report_len(packet);

                    /* BTstack hands us the report with the 1-byte HID over
                     * L2CAP transaction header still attached:
                     *   byte 0 = 0xA1   (DATA | INPUT)
                     *   byte 1..N       (the actual HID input report)
                     * deskhop's process_keyboard_report / extract_kbd_data
                     * expects the report to start at byte 0 with the report
                     * ID (or modifier byte for boot keyboards) — so we
                     * strip the 0xA1 here.  Reference: BTstack's
                     * hid_host_demo.c → hid_host_handle_interrupt_report().
                     * Without this, every keypress was being mis-parsed
                     * (modifier=0xA1, keycode bytes shifted by one). */
                    if (len < 1 || report[0] != 0xA1)
                        break;
                    report++;
                    len--;

                    g_bt->process_report((uint8_t *)report, (int)len,
                                         g_bt->kbd_itf, g_bt->kbd_iface);
                    /* Latch INPUT stage on every report — also re-asserts it
                     * if we'd briefly transitioned elsewhere.  Cheap. */
                    set_stage(BT_STAGE_INPUT);
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

    /* Canonical BTstack hid_host_demo init sequence — mirrors the upstream
     * example in pico-sdk/lib/btstack/example/hid_host_demo.c.  Each call
     * matters:
     *
     *  - Default link policy: enables sniff mode + role switch.  Without
     *    role switch, some peripherals refuse to accept us as master.
     *  - Master/slave policy:  prefer being master for incoming
     *    connections; keyboards are usually slaves.
     *  - SSP IO capability NO_INPUT_NO_OUTPUT:  selects the "just works"
     *    SSP variant — no PIN entry, no numeric comparison UI.
     *  - Security level 2:    standard for HID; allows unauthenticated
     *    pairing which most keyboards expect.
     *  - Bondable mode:       must be enabled for link keys to persist
     *    into TLV flash (see PICO_FLASH_BANK_STORAGE_OFFSET).
     *  - Discoverable + connectable:  required for the keyboard's
     *    inquiry / reconnect-after-bond paths.  Without these the radio
     *    is dark and the keyboard has no way to see us. */
    gap_set_default_link_policy_settings(LM_LINK_POLICY_ENABLE_ROLE_SWITCH
                                       | LM_LINK_POLICY_ENABLE_SNIFF_MODE);
    hci_set_master_slave_policy(HCI_ROLE_MASTER);
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    gap_set_security_level(LEVEL_2);
    gap_set_bondable_mode(1);

    /* Advertise as "deskhop BT" — BTstack auto-replaces the MAC suffix. */
    gap_set_local_name("deskhop BT 00:00:00:00:00:00");

    gap_discoverable_control(1);
    gap_connectable_control(1);

    /* HID host protocol layer */
    hid_host_init(hid_descriptor_storage, sizeof(hid_descriptor_storage));

    /* Two registrations, both required — and skipping the first one was
     * the entire reason inquiry / pairing never started:
     *
     *  - hci_add_event_handler:        general HCI events route to us.
     *    BTSTACK_EVENT_STATE (→ HCI_STATE_WORKING, our trigger to
     *    start_inquiry), HCI_EVENT_INQUIRY_RESULT,
     *    HCI_EVENT_INQUIRY_COMPLETE, HCI_EVENT_PIN_CODE_REQUEST,
     *    HCI_EVENT_USER_CONFIRMATION_REQUEST.  Without this our
     *    packet_handler never sees the radio come up.
     *
     *  - hid_host_register_packet_handler: HID-specific subevents
     *    (HID_SUBEVENT_INCOMING_CONNECTION / CONNECTION_OPENED /
     *    REPORT / DESCRIPTOR_AVAILABLE / CONNECTION_CLOSED).  Routed by
     *    hid_host.c only — does NOT receive the generic HCI events
     *    above, despite the same signature.
     *
     * Reference: pico-sdk/lib/btstack/example/hid_host_demo.c lines
     * 148, 157–158 — same dual registration.
     *
     * The registration struct must outlive bt_hid_host_init() since
     * hci_add_event_handler stores a pointer to it; make it file-scope. */
    static btstack_packet_callback_registration_t hci_event_cb = {
        .callback = &packet_handler,
    };
    hci_add_event_handler(&hci_event_cb);
    hid_host_register_packet_handler(packet_handler);

    /* Power on the radio.  packet_handler will see BTSTACK_EVENT_STATE →
     * HCI_STATE_WORKING when the controller is ready, blink the LED twice,
     * and start GAP inquiry. */
    hci_power_control(HCI_POWER_ON);
}

/* ---- LED stage-tick driver ----------------------------------------- *
 *
 * Wired into the core0 task scheduler at 30 Hz.  Runs a tiny three-phase
 * state machine — IDLE → BLINKING → PAUSING → IDLE — that continuously
 * paints g_stage onto the LED via the existing blinks_left handshake
 * with led_blinking_task on core1.  All shared device_t fields are
 * accessed through the pointer fields in bt_hid_state_t so this file
 * stays free of TinyUSB headers (see the header comment).             */

typedef enum {
    BLINK_IDLE,             /* waiting for stage > 0                       */
    BLINK_CYCLE_GAP,        /* 2 s LED OFF before each cycle starts        */
    BLINK_STAGE_ON,         /* LED ON  for FLASH_ON_US (one stage flash)   */
    BLINK_STAGE_OFF,        /* LED OFF for FLASH_OFF_US (intra-group gap)  */
    BLINK_STAGE_GROUP_GAP,  /* 500 ms LED OFF between tally groups of 5    */
    BLINK_MID_GAP,          /* 1 s LED OFF between stage and status        */
    BLINK_STATUS_ON,
    BLINK_STATUS_OFF,
    BLINK_STATUS_GROUP_GAP,
} blink_phase_t;

static blink_phase_t g_phase        = BLINK_IDLE;
static uint32_t      g_phase_us     = 0;
static bt_stage_t    g_cycle_stage  = BT_STAGE_IDLE;  /* snapshotted per cycle */
static uint8_t       g_cycle_status = 0;
static uint8_t       g_flashes_left = 0;

/* Direct LED control.  Both core0 (stage_tick, led_apply_task) — no race.
 * led_blinking_task on core1 stays dormant because we never touch
 * blinks_left, so no contention there either. */
static inline void set_led(bool on) {
    if (*g_bt->onboard_led_state == on) return;
    *g_bt->onboard_led_state = on;
    *g_bt->onboard_led_dirty = true;
}

void bt_hid_host_stage_tick(void) {
    if (!g_bt || !g_bt->onboard_led_state || !g_bt->onboard_led_dirty)
        return;

    uint32_t now     = time_us_32();
    uint32_t elapsed = now - g_phase_us;

    switch (g_phase) {
        case BLINK_IDLE: {
            bt_stage_t stage = g_stage;
            if (stage == BT_STAGE_IDLE)
                return;  /* nothing to indicate yet; leave LED alone */
            /* Snapshot stage + status so they stay consistent for the
             * whole cycle even if the BT handler updates them mid-cycle. */
            g_cycle_stage  = stage;
            g_cycle_status = g_status_code;
            set_led(false);
            g_phase    = BLINK_CYCLE_GAP;
            g_phase_us = now;
            break;
        }

        case BLINK_CYCLE_GAP:
            if (elapsed >= CYCLE_GAP_US) {
                /* Re-snapshot at the START of each visible cycle so
                 * staleness is bounded to one cycle.                     */
                g_cycle_stage  = g_stage;
                g_cycle_status = g_status_code;
                g_flashes_left = (uint8_t)g_cycle_stage;
                set_led(true);
                g_phase    = BLINK_STAGE_ON;
                g_phase_us = now;
            }
            break;

        case BLINK_STAGE_ON:
            if (elapsed >= FLASH_ON_US) {
                set_led(false);
                g_flashes_left--;
                g_phase    = BLINK_STAGE_OFF;
                g_phase_us = now;
            }
            break;

        case BLINK_STAGE_OFF: {
            if (elapsed < FLASH_OFF_US)
                break;
            if (g_flashes_left == 0) {
                /* Stage flashes finished.  Branch to status display
                 * (failure case) or loop back for next cycle.       */
                if (g_cycle_stage == BT_STAGE_FAILED && g_cycle_status > 0) {
                    g_phase = BLINK_MID_GAP;
                } else {
                    g_phase = BLINK_CYCLE_GAP;
                }
                g_phase_us = now;
                break;
            }
            /* Tally-group separator: after every GROUP_SIZE flashes
             * within the same sequence, insert a longer gap so the
             * user can count groups rather than individual flashes. */
            uint8_t flashes_done = (uint8_t)g_cycle_stage - g_flashes_left;
            if ((flashes_done % GROUP_SIZE) == 0) {
                g_phase    = BLINK_STAGE_GROUP_GAP;
                g_phase_us = now;
            } else {
                set_led(true);
                g_phase    = BLINK_STAGE_ON;
                g_phase_us = now;
            }
            break;
        }

        case BLINK_STAGE_GROUP_GAP:
            if (elapsed >= GROUP_GAP_US) {
                set_led(true);
                g_phase    = BLINK_STAGE_ON;
                g_phase_us = now;
            }
            break;

        case BLINK_MID_GAP:
            if (elapsed >= MID_GAP_US) {
                g_flashes_left = g_cycle_status;
                set_led(true);
                g_phase    = BLINK_STATUS_ON;
                g_phase_us = now;
            }
            break;

        case BLINK_STATUS_ON:
            if (elapsed >= FLASH_ON_US) {
                set_led(false);
                g_flashes_left--;
                g_phase    = BLINK_STATUS_OFF;
                g_phase_us = now;
            }
            break;

        case BLINK_STATUS_OFF: {
            if (elapsed < FLASH_OFF_US)
                break;
            if (g_flashes_left == 0) {
                g_phase    = BLINK_CYCLE_GAP;
                g_phase_us = now;
                break;
            }
            uint8_t flashes_done = g_cycle_status - g_flashes_left;
            if ((flashes_done % GROUP_SIZE) == 0) {
                g_phase    = BLINK_STATUS_GROUP_GAP;
                g_phase_us = now;
            } else {
                set_led(true);
                g_phase    = BLINK_STATUS_ON;
                g_phase_us = now;
            }
            break;
        }

        case BLINK_STATUS_GROUP_GAP:
            if (elapsed >= GROUP_GAP_US) {
                set_led(true);
                g_phase    = BLINK_STATUS_ON;
                g_phase_us = now;
            }
            break;
    }
}

#endif /* DH_BT_HID_HOST_KBD */
