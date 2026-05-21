/*
 * BT event queue implementation.  See src/include/bt_events.h.
 *
 * Ring buffer sized at 16 events.  Slot 0 sentinel: head==tail means
 * empty; head wraps modulo capacity.  When the queue is full and a
 * new publish arrives, the OLDEST event is dropped (head advances)
 * rather than the new one being lost — UI state freshness > history
 * completeness.
 *
 * Memory: 16 * sizeof(bt_event_t) = ~720 B in .bss.  Acceptable.
 */

#include "bt_events.h"
#include <string.h>

#define BT_EVT_RING_CAP 16

static bt_event_t        ring[BT_EVT_RING_CAP];
static volatile uint8_t  ring_head;  /* next slot to write */
static volatile uint8_t  ring_tail;  /* next slot to read  */

static bt_active_entry_t active[BT_ACTIVE_CAP];
static volatile uint8_t  active_count_cache;
static volatile uint8_t  bonded_count_cache;

static int find_active_slot(const bt_evt_addr_t *addr) {
    for (int i = 0; i < BT_ACTIVE_CAP; i++) {
        if (active[i].in_use && memcmp(active[i].addr.bytes, addr->bytes, 6) == 0)
            return i;
    }
    return -1;
}

static int alloc_active_slot(void) {
    for (int i = 0; i < BT_ACTIVE_CAP; i++) {
        if (!active[i].in_use) return i;
    }
    return -1;
}

void bt_events_publish(const bt_event_t *evt) {
    /* Update the active-device side-table BEFORE enqueuing the event,
     * so when the UI drains the queue and decides to re-render, the
     * counter and table it reads are already consistent. */
    switch (evt->type) {
        case BT_EVT_DEVICE_CONNECTED: {
            int slot = find_active_slot(&evt->addr);
            if (slot < 0) slot = alloc_active_slot();
            if (slot >= 0) {
                active[slot].addr      = evt->addr;
                active[slot].transport = evt->transport;
                active[slot].kind      = evt->kind;
                active[slot].cid       = evt->cid;
                active[slot].in_use    = true;
                /* Initial name: whatever the event carried (often empty
                 * for BLE — the GAP Device Name read happens
                 * asynchronously and fires a separate
                 * BT_EVT_DEVICE_NAME_RESOLVED later). */
                memcpy(active[slot].name, evt->name, BT_EVT_NAME_MAX);
            }
            /* Recount; cheaper to scan 4 slots than to track deltas. */
            uint8_t c = 0;
            for (int i = 0; i < BT_ACTIVE_CAP; i++) if (active[i].in_use) c++;
            active_count_cache = c;
            break;
        }
        case BT_EVT_DEVICE_DISCONNECTED: {
            int slot = find_active_slot(&evt->addr);
            if (slot >= 0) {
                active[slot].in_use = false;
                memset(active[slot].name, 0, BT_EVT_NAME_MAX);
            }
            uint8_t c = 0;
            for (int i = 0; i < BT_ACTIVE_CAP; i++) if (active[i].in_use) c++;
            active_count_cache = c;
            break;
        }
        case BT_EVT_DEVICE_NAME_RESOLVED: {
            /* Patch the name onto the already-connected entry.  If the
             * device disconnected before its name read completed (rare
             * but possible), this is a no-op. */
            int slot = find_active_slot(&evt->addr);
            if (slot >= 0) {
                memcpy(active[slot].name, evt->name, BT_EVT_NAME_MAX);
            }
            break;
        }
        case BT_EVT_DEVICE_PAIRED:
            /* Bonded count bookkeeping is owned by the BTstack TLV; we
             * don't infer it here.  bt_hid_host_le.c's SM_EVENT_IDENTITY_
             * CREATED handler calls bt_events_set_bonded_count() with the
             * authoritative le_device_db_count() AFTER the new bond is in
             * the DB, then publishes this event.  Incrementing here would
             * double-count the just-added bond ("2 of 3" with 2 bonds), so
             * we deliberately do nothing to the count. */
            break;
        default:
            break;
    }

    /* Enqueue.  If full (head + 1 == tail), drop oldest by advancing
     * tail; the new event still goes into ring[head]. */
    uint8_t next_head = (uint8_t)((ring_head + 1) % BT_EVT_RING_CAP);
    if (next_head == ring_tail) {
        ring_tail = (uint8_t)((ring_tail + 1) % BT_EVT_RING_CAP);
    }
    ring[ring_head] = *evt;
    ring_head = next_head;
}

bool bt_events_poll(bt_event_t *out) {
    if (ring_head == ring_tail) return false;
    *out = ring[ring_tail];
    ring_tail = (uint8_t)((ring_tail + 1) % BT_EVT_RING_CAP);
    return true;
}

uint8_t bt_events_active_count(void) { return active_count_cache; }
uint8_t bt_events_bonded_count(void) { return bonded_count_cache; }

const bt_active_entry_t *bt_events_active_table(void) { return active; }

void bt_events_set_bonded_count(uint8_t n) {
    bonded_count_cache = n;
}
