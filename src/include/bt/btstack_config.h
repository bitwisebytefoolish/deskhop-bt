/* BTstack compile-time configuration for deskhop-bt.
 *
 * Classic Bluetooth HID host only — no BLE, no A2DP, no HFP.
 * One keyboard connection, bond stored via pico_btstack_flash_bank.
 *
 * Keep this file in sync with the FLASH_BTSTACK_BANK reservation in
 * misc/memory_map_rp2350.ld (8 KB = 2 × FLASH_SECTOR_SIZE).
 */

#ifndef BTSTACK_CONFIG_H
#define BTSTACK_CONFIG_H

/* ---- Feature set -------------------------------------------------- */

/* ENABLE_CLASSIC is injected by pico_btstack_classic's cmake interface;
 * defining it here causes a harmless-but-noisy redefinition warning. */

#define ENABLE_SSP          /* Secure Simple Pairing — just-works for keyboards */

/* hci_dump_embedded_stdout.c (compiled unconditionally by pico_btstack_base)
 * requires this flag to exist, even if we never call hci_dump_init(). */
#define ENABLE_PRINTF_HEXDUMP

/* ---- Buffer / object counts --------------------------------------- */

/* Required by btstack_hci_transport_cyw43.c — must be a multiple of 4. */
#define HCI_ACL_CHUNK_SIZE_ALIGNMENT  4

#define HCI_ACL_PAYLOAD_SIZE         (1691 + 4)
#define HCI_INCOMING_PRE_BUFFER_SIZE  4
#define HCI_OUTGOING_PRE_BUFFER_SIZE  4

#define MAX_NR_HCI_CONNECTIONS        1
#define MAX_NR_L2CAP_SERVICES         3    /* HID-Control, HID-Interrupt, SDP */
#define MAX_NR_L2CAP_CHANNELS         4
#define MAX_NR_RFCOMM_MULTIPLEXERS    0
#define MAX_NR_RFCOMM_SERVICES        0
#define MAX_NR_RFCOMM_CHANNELS        0

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

#define NVM_NUM_LINK_KEYS             1
#define NVM_NUM_DEVICE_DB_ENTRIES     1

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
