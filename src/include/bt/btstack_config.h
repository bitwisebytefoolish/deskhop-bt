/* BTstack compile-time configuration for deskhop-bt.
 *
 * Dual-mode: Bluetooth Classic HID host (BR/EDR) AND BLE HID-over-GATT
 * host (HOGP) — same chip, both transports active simultaneously.
 * One keyboard connection at a time on either transport; bonds stored
 * via pico_btstack_flash_bank.  No A2DP, HFP, or other profiles.
 *
 * Keep this file in sync with the FLASH_BTSTACK_BANK reservation in
 * misc/memory_map_rp2350.ld (8 KB = 2 × FLASH_SECTOR_SIZE).
 */

#ifndef BTSTACK_CONFIG_H
#define BTSTACK_CONFIG_H

/* ---- Feature set -------------------------------------------------- */

/* ENABLE_CLASSIC and ENABLE_BLE are injected by pico_btstack_classic
 * and pico_btstack_ble respectively via their CMake interfaces;
 * defining them here would cause harmless-but-noisy redefinition warnings. */

#define ENABLE_SSP          /* Secure Simple Pairing — just-works for keyboards */

/* BLE-specific feature flags.  We're the Central (we initiate connections
 * to peripherals like keyboards).  LE Secure Connections is required for
 * modern peripherals — most 2018+ BLE keyboards reject pairing without it.
 *
 * ENABLE_LE_PERIPHERAL is technically not strictly needed (we don't expose
 * any GATT services / advertise ourselves), but BTstack's hci.c
 * references `le_advertisements_state` unconditionally in code paths that
 * are reached even in central-only builds — without ENABLE_LE_PERIPHERAL
 * the struct field doesn't exist and BTstack fails to compile.  Treat
 * this as a BTstack quirk: enabling the flag costs a few KB of unreachable
 * peripheral-mode code but isn't otherwise functional. */
#define ENABLE_LE_CENTRAL
#define ENABLE_LE_PERIPHERAL
#define ENABLE_LE_SECURE_CONNECTIONS

/* hci_dump_embedded_stdout.c (compiled unconditionally by pico_btstack_base)
 * requires this flag to exist, even if we never call hci_dump_init(). */
#define ENABLE_PRINTF_HEXDUMP

/* ---- Buffer / object counts --------------------------------------- */

/* Required by btstack_hci_transport_cyw43.c — must be a multiple of 4. */
#define HCI_ACL_CHUNK_SIZE_ALIGNMENT  4

#define HCI_ACL_PAYLOAD_SIZE         (1691 + 4)
/* BLE's gatt_client.c requires INCOMING_PRE_BUFFER_SIZE >= 6 for the
 * "long characteristic read" path.  4 was fine for Classic-only;
 * bumped to 6 when BLE was added (#29). */
#define HCI_INCOMING_PRE_BUFFER_SIZE  6
#define HCI_OUTGOING_PRE_BUFFER_SIZE  4

/* Bumped to 4 for #9 multi-device work: support keyboard + mouse +
 * numpad + spare slot simultaneously.  Mix of Classic and BLE.
 * Static memory cost: ~2 KB per slot. */
#define MAX_NR_HCI_CONNECTIONS        4
#define MAX_NR_L2CAP_SERVICES         3    /* HID-Control, HID-Interrupt, SDP */
/* Classic uses 2 channels per HID device (Control + Interrupt) plus 1
 * shared SDP.  4 Classic devices = 9 channels.  Plus per-BLE-link ATT
 * pseudo-channel.  Sized at 12 for headroom. */
#define MAX_NR_L2CAP_CHANNELS         12
#define MAX_NR_RFCOMM_MULTIPLEXERS    0
#define MAX_NR_RFCOMM_SERVICES        0
#define MAX_NR_RFCOMM_CHANNELS        0

/* ---- BLE pools ---------------------------------------------------- */

/* Per-link GATT client.  4 to match MAX_NR_HCI_CONNECTIONS. */
#define MAX_NR_GATT_CLIENTS           4

/* HIDS (HID Service) client pool.  Sized at 4 for the same reason as
 * HID_HOST_CONNECTIONS below.  Important: undefined defaults to 0 →
 * empty pool → hids_client_connect returns BTSTACK_MEMORY_ALLOC_FAILED
 * (0x56) synchronously (same family of bug as #6's
 * MAX_NR_HID_HOST_CONNECTIONS bug). */
#define MAX_NR_HIDS_CLIENTS           4

/* BLE bond DB: keep parity with Classic NVM_NUM_LINK_KEYS.  8 slots
 * gives room for both transports' bonds without collisions. */
#define MAX_NR_LE_DEVICE_DB_ENTRIES   8

/* In-flight Security Manager transactions.  Default 3 is conservative;
 * bumped to 4 to match concurrent connect attempts. */
#define MAX_NR_SM_LOOKUP_ENTRIES      4

/* Local ATT database size (peripheral-side, advertised services).  We
 * don't host any GATT services — we're a central — but ATT_DB_UTIL
 * still needs a buffer to exist when ENABLE_LE_PERIPHERAL is defined.
 * 256 B is the smallest practical value for an empty service table. */
#define MAX_ATT_DB_SIZE               256

/* HID host connection pool.  hid_host_connect() does
 *   connection = hid_host_create_connection(remote_addr);
 *   if (!connection) return BTSTACK_MEMORY_ALLOC_FAILED;  (= 0x56 = 86)
 * which pulls from a static array of size MAX_NR_HID_HOST_CONNECTIONS in
 * btstack_memory.c.  If undefined, the default is 0 — the pool is empty,
 * every connect attempt returns 0x56.  THIS was the silent failure that
 * caused every pairing attempt to fail synchronously.
 *
 * Bumped to 4 for #9 multi-device.  Allows 3 simultaneous Classic HID
 * devices (keyboard + mouse + numpad) plus 1 transient overlap slot
 * for the brief window during reconnect when an old connection isn't
 * yet freed but a new one is being attempted. */
#define MAX_NR_HID_HOST_CONNECTIONS   4

/* ---- Bond storage (TLV via pico_btstack_flash_bank) --------------- */

#define NVM_NUM_LINK_KEYS             8
#define NVM_NUM_DEVICE_DB_ENTRIES     8

/* Place the BTstack TLV bank 12 KB from the end of flash, directly below
 * FLASH_CONFIG (4 KB).  pico_btstack_flash_bank's default would use the
 * last 8 KB which overlaps our FLASH_CONFIG region.  This define is
 * picked up by pico/btstack_flash_bank.h via the #ifndef guard, provided
 * btstack_config.h is included first (BTstack convention). */
#define PICO_FLASH_BANK_STORAGE_OFFSET (PICO_FLASH_SIZE_BYTES - 4096 - 8192)

/* ---- Logging ------------------------------------------------------- */

/* Route errors to the BTstack log handler registered in bt_hid_host_init.
 * deskhop has stdio disabled; a handler can write to the UART debug port. */
#define ENABLE_LOG_ERROR

#endif /* BTSTACK_CONFIG_H */
