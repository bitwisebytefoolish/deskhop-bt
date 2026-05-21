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

    /* Pointer to the hid_interface_t used for keyboard reports.
     * Pre-populated with protocol = HID_PROTOCOL_BOOT in setup.c so
     * deskhop's extract_kbd_data takes the fixed-format _extract_kbd_boot
     * path.  parse_descriptor (if it ever runs) fills it in further;
     * REPORT-mode is currently not used so parse_descriptor is effectively
     * a no-op for BT.  Allocated and owned by the caller (setup.c). */
    struct hid_interface_t *kbd_iface;

    /* Interface index forwarded to process_keyboard_report (slot in
     * device_t.local_kbd_states[]).  Use 0 for the sole BT keyboard slot. */
    uint8_t kbd_itf;

    /* Pointer to the hid_interface_t used for mouse reports.  Separate
     * iface so iface->mouse fields don't collide with iface->keyboard
     * on a single struct.  Pre-populated with protocol = HID_PROTOCOL_BOOT
     * in setup.c; deskhop's mouse pipeline in BOOT mode just casts the
     * 5-byte buffer to hid_mouse_report_t (buttons, x, y, wheel, pan)
     * and extracts those fields directly — no descriptor parsing. */
    struct hid_interface_t *mouse_iface;

    /* Interface index for the mouse — 1 (keyboard uses 0). */
    uint8_t mouse_itf;

    /* Parse raw HID report descriptor bytes into kbd_iface / mouse_iface.
     * Points to parse_report_descriptor() from hid_parser.c.  Currently
     * effectively unused on the BT path (everything is BOOT-mode), kept
     * for the eventual #8 REPORT-mode reactivation. */
    void (*parse_descriptor)(struct hid_interface_t *iface,
                             const uint8_t *desc, int len);

    /* Route a raw HID keyboard report through process_keyboard_report().
     * Signature matches process_keyboard_report() in keyboard.h. */
    void (*process_report)(uint8_t *raw, int len, uint8_t itf,
                           struct hid_interface_t *iface);

    /* Route a raw HID mouse report through process_mouse_report().
     * Signature matches process_mouse_report() in mouse.h. */
    void (*process_mouse_report)(uint8_t *raw, int len, uint8_t itf,
                                 struct hid_interface_t *iface);

    /* Route a packed 7-byte gamepad report (gamepad_report_t layout:
     * int8 lx,ly,rx,ry; uint8 hat; uint16 buttons) onward.  Wired in
     * setup.c to a shim that calls send_gamepad().  Kept free of
     * TinyUSB types so this header doesn't collide with BTstack's.
     * BLE Switch 2 Pro path (#24). */
    void (*process_gamepad_report)(uint8_t *raw, int len);

    /* ---- LED feedback plumbing -------------------------------------- *
     * The BT stage indicator (bt_hid_host_stage_tick) drives the on-board
     * LED *directly* via these pointers — bypassing the blink_n /
     * restore_leds machinery in led.c, which can't give us a deterministic
     * LED-off background between flash sequences (there's a race between
     * led_blinking_task on core1 calling restore_leds and our stage_tick
     * on core0 overriding it).  Keeping blinks_left = 0 throughout means
     * led_blinking_task stays dormant and there's no contention.
     *
     * The blinks_left / last_led_change pointers are vestigial after the
     * sticky-stage redesign but kept for back-compat (other callers may
     * still use blink_led(state) for unrelated patterns). */
    int32_t      *blinks_left;        /* device_t.blinks_left (unused now) */
    int32_t      *last_led_change;    /* device_t.last_led_change (unused) */
    bool         *onboard_led_state;  /* device_t.onboard_led_state */
    volatile bool *onboard_led_dirty; /* device_t.onboard_led_dirty */
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

/* Sticky-stage LED driver.  Call from the core0 task scheduler at ~30 Hz.
 * Continuously loops the on-board LED through the current BT stage's
 * flash count with a 1 s pause between repetitions, so the user can read
 * the latest stage at any time instead of catching a momentary blink. */
void bt_hid_host_stage_tick(void);

#endif /* DH_BT_HID_HOST_KBD */
