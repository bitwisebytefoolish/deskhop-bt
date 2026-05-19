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

/* 2: one Classic + one BLE link can coexist.  Bumped from 1 when BLE
 * was added.  Static memory cost: ~2 KB per slot. */
#define MAX_NR_HCI_CONNECTIONS        2
#define MAX_NR_L2CAP_SERVICES         3    /* HID-Control, HID-Interrupt, SDP */
/* Classic L2CAP channels (HID Control + HID Interrupt + SDP) plus
 * a BLE pseudo-channel for ATT — bumped from 4 to 6 for BLE. */
#define MAX_NR_L2CAP_CHANNELS         6
#define MAX_NR_RFCOMM_MULTIPLEXERS    0
#define MAX_NR_RFCOMM_SERVICES        0
#define MAX_NR_RFCOMM_CHANNELS        0

/* ---- BLE pools ---------------------------------------------------- */

/* One outgoing GATT client (we connect to one peripheral at a time). */
#define MAX_NR_GATT_CLIENTS           1

/* BLE bond DB: keep parity with Classic side; 4 slots is plenty for
 * the deskhop use case (one keyboard, room for spares). */
#define MAX_NR_LE_DEVICE_DB_ENTRIES   4

/* In-flight Security Manager transactions.  Default 3 is conservative. */
#define MAX_NR_SM_LOOKUP_ENTRIES      3

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
 * Sized at 2 (not 1) to tolerate transient overlap: when the BT link
 * drops and our CONNECTION_CLOSED handler immediately restarts inquiry
 * and re-issues hid_host_connect, BTstack may not yet have freed the
 * outgoing connection slot.  A pool of 1 would then return 0x56; a pool
 * of 2 lets the new connect proceed without waiting for cleanup.       */
#define MAX_NR_HID_HOST_CONNECTIONS   2

/* ---- Bond storage (TLV via pico_btstack_flash_bank) --------------- */

#define NVM_NUM_LINK_KEYS             4
#define NVM_NUM_DEVICE_DB_ENTRIES     4

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
