#pragma once

#ifdef DH_BT_HID_HOST_KBD

#include <stdbool.h>

/* Bridge struct passed to bt_hid_host_init().
 *
 * Keeps bt_hid_host.c free of TinyUSB headers: TinyUSB and BTstack both
 * define HID_REPORT_TYPE_INPUT/OUTPUT/FEATURE as enum constants and cannot
 * coexist in the same translation unit.  Callers (setup.c) hold the full
 * device_t and extract only the fields the BT layer needs.
 *
 * Commit 5 will add a report_callback pointer here for routing BT HID
 * reports into process_keyboard_report(). */
typedef struct {
    bool *keyboard_connected; /* pointer into device_t.keyboard_connected */
} bt_hid_state_t;

/* Initialise the BTstack Classic HID host on board A (keyboard socket).
 *
 * Must be called from core0, after cyw43_arch_init() has returned, before
 * the watchdog is enabled.  cyw43_arch_init() already calls
 * btstack_cyw43_init() internally (cyw43_arch_poll.c, CYW43_ENABLE_BLUETOOTH
 * guard), so this function only needs to wire the HID host layer on top:
 * l2cap_init, gap security level, hid_host_init, and hci_power_control.
 *
 * The BTstack run-loop is driven by cyw43_poll_task() on core0 — no
 * additional calls are needed after bt_hid_host_init() returns. */
void bt_hid_host_init(bt_hid_state_t *bt_state);

#endif /* DH_BT_HID_HOST_KBD */
