#pragma once

#ifdef DH_BT_HID_HOST_KBD

#include "bt_hid_host.h"   /* bt_hid_state_t — shared with the Classic path */

/* BLE (Bluetooth Low Energy) HID-over-GATT Profile (HOGP) host on board A.
 *
 * This is the BLE peer of bt_hid_host.c (Classic BR/EDR HID host).  Both
 * run simultaneously on the shared CYW43 radio so the deskhop pairs with
 * keyboards on either transport — Classic-only (rare), BLE-only (most
 * modern wireless keyboards like the 8BitDo Retro Mechanical), or dual-
 * mode (e.g. Keychron K7).  First connection wins; the other transport
 * keeps scanning but does nothing while a peer is connected.
 *
 * Reports are forwarded through the SAME bt_hid_state_t.process_report
 * callback used by the Classic path.  BLE HOGP boot keyboard reports
 * have the identical 8-byte layout to Classic boot reports (modifier +
 * reserved + 6 keycodes), so deskhop's _extract_kbd_boot path consumes
 * both without any per-transport conditionals.
 *
 * Must be called AFTER cyw43_arch_init() and BEFORE bt_hid_host_init().
 * bt_hid_host_init() is the one that calls hci_power_control(HCI_POWER_ON)
 * to bring the radio up — both transports come online together at that
 * moment.  Calling order:
 *
 *     bt_hid_host_le_init(&bt_state);   // register BLE handlers
 *     bt_hid_host_init(&bt_state);      // register Classic handlers + power on
 *
 * Reference: pico-sdk/lib/btstack/example/hog_boot_host_demo.c.
 *
 * See issue #29 for design rationale and the 8BitDo Retro use case. */
void bt_hid_host_le_init(bt_hid_state_t *bt_state);

/* Forget a single bonded BLE device by address (#22 Phase 3).  Removes
 * the matching LE device DB entry (TLV-backed, so it persists), tears
 * down any live connection, and clears the fast-reconnect hint +
 * persisted name.  Safe with an address that isn't bonded (no-op).
 * `addr` is a 6-byte BD_ADDR. */
void bt_hid_host_le_forget(const uint8_t *addr);

/* Forget every bonded BLE device — disconnects all live links and wipes
 * the LE device DB, the fast-reconnect hint, and the persisted name
 * table.  The bulletproof "factory reset bonds" path. */
void bt_hid_host_le_forget_all(void);

/* Open / close the pairing window (#22 Phase 3 fix).  Pairing is CLOSED
 * by default — the host ignores unbonded advertisers and declines their
 * pairing requests, so a forgotten device can't silently re-pair.  The
 * LCD "Pair new" flow opens the window for its countdown and closes it
 * on success / cancel / timeout.  Bonded devices reconnect regardless. */
void bt_hid_host_le_set_pairing_open(bool open);

/* A bonded-device record for the LCD device list.  Lets the UI show every
 * bond (connected or not) with a connection-status indicator. */
typedef struct {
    uint8_t addr[6];
    uint8_t addr_type;
    char    name[24];   /* "" if no friendly name known; matches BT_EVT_NAME_MAX */
    bool    connected;  /* currently has a live HID connection */
} bt_bond_info_t;

/* Enumerate bonded BLE devices into `out` (capacity `max`).  Returns the
 * number filled.  Each entry carries the identity address, the persisted
 * friendly name (if any), and whether the device is currently connected. */
int bt_hid_host_le_get_bonds(bt_bond_info_t *out, int max);

#endif /* DH_BT_HID_HOST_KBD */
