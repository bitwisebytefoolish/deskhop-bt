/*
 * Cross-module BT event bus for the OLED UI (issue #22).
 *
 * The BT subsystem (bt_hid_host.c for Classic, bt_hid_host_le.c for
 * BLE HOGP) already mutates device_t fields directly when a peer
 * pairs / connects / fails.  That's fine for the existing routing
 * code, but the UI wants a queryable history of recent events to
 * render status updates and flash short-lived banners ("paired!",
 * "pair failed: 0x05"...).
 *
 * Rather than refactor every printf site into a publish-and-mutate
 * function, we add a small ring queue that callers push events into
 * at the same time as their existing printfs.  The UI task drains it
 * at 30 Hz.  No subscriptions, no callbacks — pull model, single
 * consumer.
 *
 * Threading: producers (BTstack-level packet handlers) run on core0.
 * Consumer (ui_render_task) runs on core0.  No cross-core
 * synchronisation needed; the queue is plain memory with volatile
 * head/tail.
 *
 * Build gating: this header compiles unconditionally so call sites
 * don't need #ifdef DH_OLED_UI guards everywhere — when the OLED
 * isn't built, bt_events_publish is a no-op stub (see bt_events.c).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* BTstack's bd_addr_t is uint8_t[6].  We mirror the layout here so this
 * header doesn't need to pull in btstack_util.h transitively — keeps
 * the UI side independent of BTstack types. */
typedef struct {
    uint8_t bytes[6];
} bt_evt_addr_t;

typedef enum {
    BT_EVT_NONE = 0,
    BT_EVT_RADIO_UP,
    BT_EVT_SCAN_STARTED,
    BT_EVT_SCAN_STOPPED,
    BT_EVT_DEVICE_PAIRED,
    BT_EVT_DEVICE_CONNECTED,
    BT_EVT_DEVICE_DISCONNECTED,
    BT_EVT_PAIR_FAILED,
    BT_EVT_DEVICE_NAME_RESOLVED,  /* peripheral's GAP Device Name (0x2A00) read back */
} bt_evt_type_t;

typedef enum {
    BT_TRANSPORT_CLASSIC = 0,
    BT_TRANSPORT_LE      = 1,
} bt_evt_transport_t;

/* Device class, derived from the HID report descriptor's top-level
 * application usage (Generic Desktop page).  Drives which icon the
 * status screen shows.  UNKNOWN falls back to a generic glyph. */
typedef enum {
    BT_KIND_UNKNOWN  = 0,
    BT_KIND_KEYBOARD = 1,
    BT_KIND_MOUSE    = 2,
    BT_KIND_KEYPAD   = 3,
} bt_evt_kind_t;

#define BT_EVT_NAME_MAX 24

typedef struct {
    bt_evt_type_t      type;
    bt_evt_transport_t transport;
    bt_evt_kind_t      kind;       /* device class for icon selection */
    bt_evt_addr_t      addr;
    uint16_t           cid;        /* hids_cid (BLE) or HID connection (Classic) — 0 if N/A */
    uint8_t            status;     /* BTstack error code for *_FAILED */
    uint64_t           ts_us;      /* time_us_64() at publish */
    char               name[BT_EVT_NAME_MAX]; /* "" if not known yet */
} bt_event_t;

/* Total currently-bonded count and currently-connected count
 * cached separately so the UI can render the counters without
 * scanning all 32 LE_DEVICE_DB_ENTRIES on every frame.  Updated
 * by bt_events_publish() as a side effect of CONNECTED /
 * DISCONNECTED events.  Other counters can be added without
 * touching the event queue.
 *
 * The "active" array is the runtime cache the UI uses to render
 * the status screen — see the BT_EVT_DEVICE_CONNECTED handler in
 * ui.c.  Sized for MAX_NR_HIDS_CLIENTS (4) currently-connected
 * peers; if BT_EVT_DEVICE_NAME_RESOLVED fills in a name after
 * the initial CONNECTED, the existing entry is patched in place. */
typedef struct {
    bt_evt_addr_t      addr;
    bt_evt_transport_t transport;
    bt_evt_kind_t      kind;
    char               name[BT_EVT_NAME_MAX];
    uint16_t           cid;
    bool               in_use;
} bt_active_entry_t;

#define BT_ACTIVE_CAP 4

/* ---- API ---------------------------------------------------------- */

/* Publish an event.  Always-safe; never blocks; if the queue is full,
 * the oldest event is overwritten (the UI cares about most recent
 * state, not full history). */
void bt_events_publish(const bt_event_t *evt);

/* Drain one event into *out.  Returns true if an event was read.
 * Stable / non-destructive ordering — FIFO from publisher side. */
bool bt_events_poll(bt_event_t *out);

/* Cached counters, updated by publish().  Cheap to read every frame. */
uint8_t bt_events_active_count(void);
uint8_t bt_events_bonded_count(void);

/* Snapshot of the active-device table.  Returns a pointer to the
 * internal array (BT_ACTIVE_CAP entries) — UI reads but doesn't write.
 * Entries with in_use=false are empty slots. */
const bt_active_entry_t *bt_events_active_table(void);

/* Set the bonded count (called from boot-time enumeration of the
 * BTstack TLV; UI reflects it on the status screen).  Separated from
 * events because we don't want to fabricate a fake event per stored
 * bond at HCI_STATE_WORKING — that flood would never get drained
 * before the UI starts. */
void bt_events_set_bonded_count(uint8_t n);

/* Convenience: fill the addr_t from BTstack's bd_addr_t (uint8_t[6]).
 * Callers in bt_hid_host*.c already have the bd_addr at hand and use
 * memcpy; this just documents the intent at call sites. */
static inline void bt_evt_addr_from_bd(bt_evt_addr_t *out, const uint8_t *bd_addr) {
    for (int i = 0; i < 6; i++) out->bytes[i] = bd_addr[i];
}
