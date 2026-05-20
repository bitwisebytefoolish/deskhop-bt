/*
 * OLED UI implementation — Phase 1 (#22).  Status screen only.
 *
 * Layout (128x64, 6x8 font, 16 cols × 8 rows of text):
 *
 *   Row 0: "A>B   BT:OK   3/4 paired"   (header)
 *   Row 1: ────────────────────────     (rule)
 *   Row 2-5: paired-and-connected list, one per row
 *   Row 6: (free slot indicator if applicable)
 *   Row 7: "SEL: menu"                  (hint — Phase 1 placeholder)
 *
 * Re-render policy: tick every 33 ms.  Compare a "render signature"
 * (active_output, bt connection states, bonded count, active_count,
 * names) against the last-rendered signature; if changed, redraw +
 * flush.  Worst case: 30 redraws/sec when something is changing
 * fast; typically idle frames do zero I2C traffic.
 */

#include "ui.h"

#ifdef DH_OLED_UI

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "pico/stdlib.h"
#include "oled.h"
#include "bt_events.h"

/* ---- internal state ------------------------------------------------- */

typedef enum {
    UI_STATE_STATUS = 0,
    /* Phase 2 will add MAIN_MENU, DEVICE_LIST, DEVICE_INFO, etc. */
} ui_state_t;

static struct {
    ui_state_t state;
    bool       initialised;
    bool       oled_ok;
    /* Last-rendered signature — coarse change detection. */
    uint8_t    last_active_output;
    uint8_t    last_active_count;
    uint8_t    last_bonded_count;
    /* Hash of all active-table names + addresses; bumping this
     * triggers a redraw without keeping the whole previous frame
     * around for comparison.  Tiny FNV-1a is sufficient. */
    uint32_t   last_active_hash;
} ui;

/* ---- helpers -------------------------------------------------------- */

static uint32_t fnv1a(const void *data, size_t n) {
    const uint8_t *p = data;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

static uint32_t hash_active_table(void) {
    const bt_active_entry_t *t = bt_events_active_table();
    /* sizeof(bt_active_entry_t) * BT_ACTIVE_CAP, includes the name
     * strings and addresses — exactly what we want to react to. */
    return fnv1a(t, sizeof(bt_active_entry_t) * BT_ACTIVE_CAP);
}

static void fmt_addr_short(char *out, size_t n, const uint8_t addr[6]) {
    /* Three-byte tail: "5f:a9:e0" (8 chars + NUL).  Useful when the
     * peripheral hasn't yielded a Device Name yet. */
    snprintf(out, n, "%02x:%02x:%02x", addr[3], addr[4], addr[5]);
}

/* Drain accumulated events.  Phase 1 doesn't react per-event (the UI
 * just reflects current-state cache from bt_events.c); draining keeps
 * the queue from filling and being silently dropped.  Phase 2/3 will
 * add per-event reactions (e.g. flash a "Paired!" banner). */
static void drain_events(void) {
    bt_event_t evt;
    while (bt_events_poll(&evt)) {
        /* No-op for Phase 1 — bt_events.c already updated its caches
         * inside publish.  This loop just clears the ring. */
    }
}

/* ---- screens -------------------------------------------------------- */

static void render_status(device_t *state) {
    const bt_active_entry_t *active = bt_events_active_table();
    uint8_t active_count = bt_events_active_count();
    uint8_t bonded_count = bt_events_bonded_count();

    oled_clear();

    /* ── Row 0: header ─────────────────────────────────────────────── */
    char hdr[24];
    /* Active output: A or B.  We have one host PC per board; show
     * which side input is currently routed to. */
    char active_side = (state->active_output == 0) ? 'A' : 'B';
    /* BT health: "OK" when at least one device is connected, "..." if
     * scanning, "off" if no bonded devices at all. */
    const char *bt_health;
    if (active_count > 0)       bt_health = "OK";
    else if (bonded_count > 0)  bt_health = "..";
    else                        bt_health = "--";

    /* "A   BT:OK  3/4 paired" — 24 char budget, 21 used here */
    snprintf(hdr, sizeof(hdr), "%c   BT:%s  %u/%u",
             active_side, bt_health, active_count, bonded_count);
    oled_text(0, 0, hdr);

    /* ── Row 1: divider rule ───────────────────────────────────────── */
    for (int x = 0; x < OLED_W; x++) {
        oled_set_pixel(x, 9, true);
    }

    /* ── Rows 2-5: active device list (up to 4) ────────────────────── */
    int row = 2;
    for (int i = 0; i < BT_ACTIVE_CAP && row <= 5; i++) {
        if (!active[i].in_use) continue;
        char line[24];
        const char *name = active[i].name;
        if (name[0] == '\0') {
            /* No Device Name yet — render the last 3 bytes of the
             * address as a placeholder.  Updates in place when the
             * BT_EVT_DEVICE_NAME_RESOLVED handler patches the
             * name into the active table. */
            char short_addr[12];
            fmt_addr_short(short_addr, sizeof(short_addr), active[i].addr.bytes);
            snprintf(line, sizeof(line), "* %s",  short_addr);
        } else {
            snprintf(line, sizeof(line), "* %s", name);
        }
        oled_text(0, row, line);
        row++;
    }

    /* ── Row 6: free-slot indicator (if we have headroom) ──────────── */
    if (active_count < BT_ACTIVE_CAP) {
        char free_line[24];
        snprintf(free_line, sizeof(free_line),
                 "o %u free slot%s",
                 (unsigned)(BT_ACTIVE_CAP - active_count),
                 (BT_ACTIVE_CAP - active_count == 1) ? "" : "s");
        oled_text(0, 6, free_line);
    }

    /* ── Row 7: hint footer (Phase 1 placeholder; Phase 2 buttons) ─── */
    oled_text(0, 7, "Phase 1 status");

    oled_flush();
}

/* ---- public API ----------------------------------------------------- */

bool ui_init(void) {
    memset(&ui, 0, sizeof(ui));
    ui.state   = UI_STATE_STATUS;
    ui.oled_ok = oled_init();
    ui.initialised = true;
    if (ui.oled_ok) {
        /* Boot splash — kept on screen until the first render_status()
         * tick paints over it.  Useful to confirm the panel was
         * detected before any BT activity. */
        oled_clear();
        oled_text(0, 2, "  deskhop-bt");
        oled_text(0, 3, "  OLED UI v1");
        oled_text(0, 5, "  awaiting BT...");
        oled_flush();
    }
    return ui.oled_ok;
}

void ui_render_task(device_t *state) {
    if (!ui.initialised || !ui.oled_ok) return;

    drain_events();

    /* Coarse change detection: signature = (active_output, counters,
     * hash of active-table names+addrs).  Skip the redraw if nothing
     * visible changed since last frame.  Saves an I2C flush per tick. */
    uint32_t active_hash = hash_active_table();
    if (state->active_output == ui.last_active_output &&
        bt_events_active_count() == ui.last_active_count &&
        bt_events_bonded_count() == ui.last_bonded_count &&
        active_hash == ui.last_active_hash) {
        return;
    }

    switch (ui.state) {
        case UI_STATE_STATUS:
        default:
            render_status(state);
            break;
    }

    ui.last_active_output = state->active_output;
    ui.last_active_count  = bt_events_active_count();
    ui.last_bonded_count  = bt_events_bonded_count();
    ui.last_active_hash   = active_hash;
}

#endif /* DH_OLED_UI */
