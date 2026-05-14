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
 * the watchdog is enabled.  Internally calls btstack_cyw43_init() which
 * wires the run-loop to the cyw43_arch poll async-context, initialises the
 * CYW43 HCI transport, and sets up TLV flash bond storage.  Then it starts
 * the HID host layer and asserts HCI_POWER_ON.
 *
 * The BTstack run-loop is driven by cyw43_poll_task() on core0 — no
 * additional calls are needed after bt_hid_host_init() returns. */
void bt_hid_host_init(bt_hid_state_t *bt_state);

#endif /* DH_BT_HID_HOST_KBD */
