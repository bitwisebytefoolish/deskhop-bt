#pragma once

#ifdef DH_BT_HID_HOST_KBD

#include <stdbool.h>
#include <stdint.h>

/* Opaque forward declaration — avoids pulling in hid_parser.h (which includes
 * tusb.h) into translation units that also include BTstack headers (where
 * HID_REPORT_TYPE_* names collide).  Callers that need the full definition
 * include hid_parser.h themselves. */
struct hid_interface_t;

/* Bridge struct passed to bt_hid_host_init().
 *
 * Keeps bt_hid_host.c free of TinyUSB headers: TinyUSB and BTstack both
 * define HID_REPORT_TYPE_INPUT/OUTPUT/FEATURE as enum constants and cannot
 * coexist in the same translation unit.  Callers (setup.c) hold the full
 * device_t and extract only the fields the BT layer needs. */
typedef struct {
    bool *keyboard_connected; /* pointer into device_t.keyboard_connected */

    /* Populated by setup.c before bt_hid_host_init() is called. -------- */

    /* Pointer to the hid_interface_t that parse_descriptor fills in.
     * bt_hid_host.c passes this to process_report after the descriptor
     * is parsed.  Allocated and owned by the caller. */
    struct hid_interface_t *kbd_iface;

    /* Interface index forwarded to process_report (slot in
     * device_t.local_kbd_states[]).  Use 0 for the sole BT keyboard. */
    uint8_t kbd_itf;

    /* Parse raw HID report descriptor bytes into kbd_iface.
     * Points to parse_report_descriptor() from hid_parser.c. */
    void (*parse_descriptor)(struct hid_interface_t *iface,
                             const uint8_t *desc, int len);

    /* Route a raw HID report through process_keyboard_report().
     * Signature matches process_keyboard_report() in keyboard.h. */
    void (*process_report)(uint8_t *raw, int len, uint8_t itf,
                           struct hid_interface_t *iface);
} bt_hid_state_t;

/* Initialise the BTstack Classic HID host on board A (keyboard socket).
 *
 * Must be called from core0, after cyw43_arch_init() has returned, before
 * the watchdog is enabled.  cyw43_arch_init() already calls
 * btstack_cyw43_init() internally (cyw43_arch_poll.c, CYW43_ENABLE_BLUETOOTH
 * guard), so this function only needs to wire the HID host layer on top:
 * l2cap_init, gap security level, hid_host_init, and hci_power_control.
 *
 * On HCI_STATE_WORKING the packet handler starts GAP inquiry to locate the
 * nearest BT keyboard.  Incoming connections are also accepted so a keyboard
 * can re-pair on its own initiative.
 *
 * The BTstack run-loop is driven by cyw43_poll_task() on core0 — no
 * additional calls are needed after bt_hid_host_init() returns. */
void bt_hid_host_init(bt_hid_state_t *bt_state);

#endif /* DH_BT_HID_HOST_KBD */
