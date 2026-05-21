/*
 * OLED UI implementation — issue #22.
 *
 * Phase 1 landed the always-on status screen.  Phase 2 (this
 * extension) adds button input, a state machine with menu
 * navigation, and three additional screens:
 *
 *   STATUS  ── SELECT ──▶  MAIN_MENU
 *              long-SEL    │   │   │
 *              ▲           ▼   ▼   ▼
 *              │     DEV_LIST (other items disabled in Phase 2 —
 *              │       │       "Pair new" + "Diagnostics" come in
 *              │       │       Phase 3 / Phase 4)
 *              │       │ SEL
 *              │       ▼
 *              │   DEV_INFO ── long-SEL ──▶ DEV_LIST
 *              │
 *              └── 10 s inactivity timer reverts non-STATUS states
 *                  back to STATUS automatically.
 *
 * Phase 2 stays read-only — no forget / pair-new actions yet.
 * DEV_INFO is informational; the "Forget" item is shown but acts
 * as a no-op placeholder so the muscle memory is correct when
 * Phase 3 wires it up.
 *
 * Re-render policy: tick 30 Hz.  Skip the redraw if no button event
 * has been processed AND no BT-state signature has changed since
 * the last frame.  Idle frames produce zero I2C traffic.
 */

#include "ui.h"

#ifdef DH_OLED_UI

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "pico/stdlib.h"
#include "oled.h"
#include "bt_events.h"
#include "buttons.h"

/* ---- State machine -------------------------------------------------- */

typedef enum {
    UI_STATE_STATUS      = 0,
    UI_STATE_MAIN_MENU   = 1,
    UI_STATE_DEVICE_LIST = 2,
    UI_STATE_DEVICE_INFO = 3,
} ui_state_t;

/* MAIN_MENU items (indices) — order is rendered order. */
enum {
    MENU_DEVICES   = 0,
    MENU_PAIR_NEW  = 1,    /* placeholder until Phase 3 */
    MENU_DIAG      = 2,    /* placeholder until Phase 4 */
    MENU__COUNT
};

/* DEVICE_INFO items. */
enum {
    INFO_BACK      = 0,
    INFO_FORGET    = 1,    /* placeholder until Phase 3 */
    INFO__COUNT
};

/* 10 s of no button input -> auto-revert to status from any deeper
 * state.  Held in microseconds since boot. */
#define UI_INACTIVITY_US (10ull * 1000000ull)

static struct {
    ui_state_t state;
    bool       initialised;
    bool       oled_ok;

    /* Cursor positions per screen — stored per-screen so backing out
     * and re-entering preserves where you were.  Reset to 0 when the
     * inactivity timer fires (returning to STATUS). */
    uint8_t    menu_cursor;
    uint8_t    list_cursor;
    uint8_t    info_cursor;

    /* When entering DEVICE_INFO, latch a snapshot of which device's
     * cid is being inspected — the active table may shuffle (a
     * device disconnects) while we're on the info screen, so we
     * track by stable cid rather than by table index. */
    uint16_t   info_cid;

    /* Inactivity tracking.  Updated on every button event. */
    uint64_t   last_input_us;

    /* Dirty tracking for status-screen redraw skipping. */
    uint8_t    last_active_output;
    uint8_t    last_active_count;
    uint8_t    last_bonded_count;
    uint32_t   last_active_hash;

    /* Bump when any button event processed -> force redraw on next
     * tick.  Cleared by the renderer. */
    bool       dirty;
} ui;

/* ---- Helpers -------------------------------------------------------- */

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
    return fnv1a(t, sizeof(bt_active_entry_t) * BT_ACTIVE_CAP);
}

static void fmt_addr_short(char *out, size_t n, const uint8_t addr[6]) {
    snprintf(out, n, "%02x:%02x:%02x", addr[3], addr[4], addr[5]);
}

static void fmt_addr_full(char *out, size_t n, const uint8_t addr[6]) {
    snprintf(out, n, "%02x:%02x:%02x:%02x:%02x:%02x",
             addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
}

/* Drain BT events.  Phase-1 behaviour: no per-event reaction, just
 * keep the queue from filling.  Phase 2 still keeps the same semantics
 * — bt_events.c maintains the active-table cache for us; the dirty
 * flag the state-change comparison will catch any visible change. */
static void drain_bt_events(void) {
    bt_event_t evt;
    while (bt_events_poll(&evt)) {
        /* no-op; state cache lives in bt_events */
    }
}

/* Find the active table index for a given cid.  Returns -1 if the cid
 * is no longer connected (device disconnected while we were viewing
 * its info screen). */
static int find_active_index_by_cid(uint16_t cid) {
    const bt_active_entry_t *t = bt_events_active_table();
    for (int i = 0; i < BT_ACTIVE_CAP; i++) {
        if (t[i].in_use && t[i].cid == cid) return i;
    }
    return -1;
}

/* Count active entries.  Could use bt_events_active_count() but keep
 * a local helper for the wrapping cursor math. */
static int count_active(void) {
    return (int)bt_events_active_count();
}

/* Move cursor up/down with wrap.  count must be > 0 — callers check. */
static uint8_t cursor_move(uint8_t cur, int delta, uint8_t count) {
    if (count == 0) return 0;
    int v = (int)cur + delta;
    while (v < 0)         v += count;
    while (v >= (int)count) v -= count;
    return (uint8_t)v;
}

/* ---- Render: STATUS ------------------------------------------------- */

static void render_status(uint8_t active_output) {
    const bt_active_entry_t *active = bt_events_active_table();
    uint8_t active_count = bt_events_active_count();
    uint8_t bonded_count = bt_events_bonded_count();

    oled_clear();

    /* ── Header (y=0-13): hero-sized active-output letter + BT meta ──
     *
     * Scale-2 "A" or "B" at top-left is the single piece of info the
     * user reads at a glance.  Renders into pixels y=2..13 (the "A"
     * glyph only uses font rows 1-6, which at scale 2 lands at y=2-13
     * — see oled_text_at).  Leaves the right side of the header band
     * (x=16..127) for two stacked lines of small text: "BT:STATE" on
     * the top and the bonded/active counter underneath. */
    char letter[2] = { (active_output == 0) ? 'A' : 'B', '\0' };
    oled_text_at(0, 0, 2, letter);

    const char *bt_state;
    if (active_count > 0)      bt_state = "BT: ready";
    else if (bonded_count > 0) bt_state = "BT: idle";
    else                       bt_state = "BT: empty";
    oled_text_at(28, 0, 1, bt_state);

    char counter[16];
    snprintf(counter, sizeof(counter), "%u of %u bonded",
             active_count, bonded_count);
    oled_text_at(28, 8, 1, counter);

    /* ── Divider at y=17 ────────────────────────────────────────────
     * 1-pixel rule between the header band and the body.  The header
     * text occupies rows 0-1 (y=0..15); the divider sits at y=17 to
     * leave a ~2px breathing gap below the "N of M bonded" line rather
     * than cutting through its descenders (which it did at y=14). */
    for (int x = 0; x < OLED_W; x++) oled_set_pixel(x, 17, true);

    /* ── Body: all 4 hids_client slots, one per 8-pixel row ─────────
     * Always render the full 4 — connected slots show an icon + name
     * + filled connection dot; empty slots show an open circle +
     * "(slot free)".  Consistent visual cadence regardless of how
     * many devices are connected; the user sees the slot capacity
     * directly instead of having to infer it. */
    for (int i = 0; i < BT_ACTIVE_CAP; i++) {
        int y = 20 + i * 8;   /* base shifted from 16 → 20 for the new divider gap */
        if (active[i].in_use) {
            /* Device-type icon — for now, generic "device" glyph.
             * Phase 3 work will populate a kind field on connect
             * (parsed from the HID descriptor's app-usage page) so
             * this can show distinct keyboard / mouse / keypad
             * icons.  The icons are already defined in oled.h. */
            oled_draw_icon(0, y, 8, 8, oled_icon_generic);
            /* Name (or address tail as fallback). */
            char line[22];
            if (active[i].name[0]) {
                snprintf(line, sizeof(line), "%s", active[i].name);
            } else {
                fmt_addr_short(line, sizeof(line), active[i].addr.bytes);
            }
            oled_text_at(10, y, 1, line);
            /* Connection-state dot at the far right.  Filled = live. */
            oled_draw_icon(120, y, 8, 8, oled_icon_dot_full);
        } else {
            /* Empty slot row. */
            oled_draw_icon(0, y, 8, 8, oled_icon_dot_empty);
            oled_text_at(10, y, 1, "(slot free)");
        }
    }

    /* ── Status row at y=48-55: reserved for Phase 2/3 ──────────────
     * Future: scanning state, last-paired banner, error code on
     * failure.  Currently a thin separator + nothing, so the eye
     * settles on the body before the footer. */
    for (int x = 8; x < OLED_W - 8; x += 4) {
        oled_set_pixel(x, 53, true);   /* dotted accent rule */
    }

    /* ── Footer at y=56-63: action hint ─────────────────────────── */
    oled_text_at(0, 56, 1, "SEL: menu");

    oled_flush();
}

/* ---- Render: MAIN_MENU --------------------------------------------- */

static const char *MAIN_MENU_LABELS[MENU__COUNT] = {
    [MENU_DEVICES]  = "Paired devices",
    [MENU_PAIR_NEW] = "Pair new (P3)",
    [MENU_DIAG]     = "Diagnostics (P4)",
};

static void render_main_menu(void) {
    oled_clear();
    oled_text(0, 0, "Menu");
    for (int x = 0; x < OLED_W; x++) oled_set_pixel(x, 9, true);

    for (int i = 0; i < MENU__COUNT; i++) {
        int row = 2 + i;
        oled_text(6, row, MAIN_MENU_LABELS[i]);
        if (i == ui.menu_cursor) {
            /* Cursor arrow + selection highlight on the whole row.
             * Highlight is a single-pixel-tall underline at the bottom
             * of the row — cheap and unambiguous; doesn't fight the
             * font's vertical alignment. */
            oled_text(0, row, ">");
            for (int x = 0; x < OLED_W; x++) oled_set_pixel(x, row * 8 + 7, true);
        }
    }

    oled_text(0, 7, "UP/DN  SEL: pick");
    oled_flush();
}

/* ---- Render: DEVICE_LIST ------------------------------------------- */

static void render_device_list(void) {
    const bt_active_entry_t *active = bt_events_active_table();
    int n = count_active();

    oled_clear();
    char hdr[24];
    snprintf(hdr, sizeof(hdr), "Devices  %u/%u",
             (unsigned)n, (unsigned)bt_events_bonded_count());
    oled_text(0, 0, hdr);
    for (int x = 0; x < OLED_W; x++) oled_set_pixel(x, 9, true);

    if (n == 0) {
        oled_text(0, 3, "(no active)");
        oled_text(0, 7, "hold SEL: back");
        oled_flush();
        return;
    }

    /* Iterate active table, skipping empty slots; map row to visible
     * index for cursor highlight.  Cursor counts only in_use slots —
     * not the raw table index — so it always lands on a real entry. */
    int visible_idx = 0;
    int row = 2;
    for (int i = 0; i < BT_ACTIVE_CAP && row <= 6; i++) {
        if (!active[i].in_use) continue;
        char line[24];
        const char *name = active[i].name;
        if (name[0]) {
            snprintf(line, sizeof(line), "%s", name);
        } else {
            char short_addr[12];
            fmt_addr_short(short_addr, sizeof(short_addr), active[i].addr.bytes);
            snprintf(line, sizeof(line), "%s", short_addr);
        }
        oled_text(6, row, line);
        if (visible_idx == ui.list_cursor) {
            oled_text(0, row, ">");
            for (int x = 0; x < OLED_W; x++) oled_set_pixel(x, row * 8 + 7, true);
        }
        visible_idx++;
        row++;
    }

    oled_text(0, 7, "UP/DN SEL info");
    oled_flush();
}

/* ---- Render: DEVICE_INFO ------------------------------------------- */

static const char *INFO_LABELS[INFO__COUNT] = {
    [INFO_BACK]   = "Back",
    [INFO_FORGET] = "Forget (P3)",
};

static void render_device_info(void) {
    int idx = find_active_index_by_cid(ui.info_cid);
    const bt_active_entry_t *active = bt_events_active_table();

    oled_clear();
    if (idx < 0) {
        /* Device disconnected while we were here — render a graceful
         * fallback rather than blank screen, and offer back as the
         * only action. */
        oled_text(0, 0, "(disconnected)");
        for (int x = 0; x < OLED_W; x++) oled_set_pixel(x, 9, true);
        oled_text(0, 3, "Device went away.");
        oled_text(0, 5, "> Back");
        for (int x = 0; x < OLED_W; x++) oled_set_pixel(x, 5 * 8 + 7, true);
        oled_text(0, 7, "SEL: back");
        oled_flush();
        return;
    }

    const bt_active_entry_t *d = &active[idx];

    /* Row 0: name (or "Unnamed device") */
    if (d->name[0]) {
        char title[24];
        snprintf(title, sizeof(title), "%s", d->name);
        oled_text(0, 0, title);
    } else {
        oled_text(0, 0, "(unnamed)");
    }
    for (int x = 0; x < OLED_W; x++) oled_set_pixel(x, 9, true);

    /* Row 2: full address */
    char addr[20];
    fmt_addr_full(addr, sizeof(addr), d->addr.bytes);
    oled_text(0, 2, addr);

    /* Row 3: transport + cid */
    char meta[24];
    snprintf(meta, sizeof(meta), "%s  cid=0x%04x",
             d->transport == BT_TRANSPORT_LE ? "BLE" : "Classic",
             d->cid);
    oled_text(0, 3, meta);

    /* Rows 5-6: action items (with cursor) */
    for (int i = 0; i < INFO__COUNT; i++) {
        int row = 5 + i;
        oled_text(6, row, INFO_LABELS[i]);
        if (i == ui.info_cursor) {
            oled_text(0, row, ">");
            for (int x = 0; x < OLED_W; x++) oled_set_pixel(x, row * 8 + 7, true);
        }
    }

    oled_text(0, 7, "UP/DN SEL pick");
    oled_flush();
}

/* ---- Input handling ------------------------------------------------- */

static void handle_button_event(const button_event_t *e) {
    ui.last_input_us = e->release_us;
    ui.dirty         = true;

    switch (ui.state) {
        case UI_STATE_STATUS:
            if (e->button == BTN_SELECT && e->kind == BTN_EVT_CLICK) {
                ui.state       = UI_STATE_MAIN_MENU;
                ui.menu_cursor = 0;
            }
            break;

        case UI_STATE_MAIN_MENU:
            if (e->kind == BTN_EVT_LONG && e->button == BTN_SELECT) {
                ui.state = UI_STATE_STATUS;
                break;
            }
            if (e->button == BTN_UP) {
                ui.menu_cursor = cursor_move(ui.menu_cursor, -1, MENU__COUNT);
            } else if (e->button == BTN_DOWN) {
                ui.menu_cursor = cursor_move(ui.menu_cursor, +1, MENU__COUNT);
            } else if (e->button == BTN_SELECT && e->kind == BTN_EVT_CLICK) {
                /* Only MENU_DEVICES leads anywhere in Phase 2 — the
                 * other two items are placeholders for Phase 3/4 and
                 * a click on them is intentionally a no-op so the
                 * user can see the labels but not navigate into
                 * un-implemented screens. */
                if (ui.menu_cursor == MENU_DEVICES) {
                    ui.state       = UI_STATE_DEVICE_LIST;
                    ui.list_cursor = 0;
                }
            }
            break;

        case UI_STATE_DEVICE_LIST:
            if (e->kind == BTN_EVT_LONG && e->button == BTN_SELECT) {
                ui.state = UI_STATE_MAIN_MENU;
                break;
            }
            {
                int n = count_active();
                if (n == 0) {
                    /* Only the back-via-long-SEL action makes sense
                     * when the list is empty.  Up/Down are no-ops. */
                    break;
                }
                if (e->button == BTN_UP) {
                    ui.list_cursor = cursor_move(ui.list_cursor, -1, (uint8_t)n);
                } else if (e->button == BTN_DOWN) {
                    ui.list_cursor = cursor_move(ui.list_cursor, +1, (uint8_t)n);
                } else if (e->button == BTN_SELECT && e->kind == BTN_EVT_CLICK) {
                    /* Resolve visible cursor index -> active table index
                     * -> cid, latch it for the info screen. */
                    const bt_active_entry_t *t = bt_events_active_table();
                    int visible_idx = 0;
                    for (int i = 0; i < BT_ACTIVE_CAP; i++) {
                        if (!t[i].in_use) continue;
                        if (visible_idx == ui.list_cursor) {
                            ui.info_cid    = t[i].cid;
                            ui.info_cursor = 0;
                            ui.state       = UI_STATE_DEVICE_INFO;
                            break;
                        }
                        visible_idx++;
                    }
                }
            }
            break;

        case UI_STATE_DEVICE_INFO:
            if (e->kind == BTN_EVT_LONG && e->button == BTN_SELECT) {
                ui.state = UI_STATE_DEVICE_LIST;
                break;
            }
            if (e->button == BTN_UP) {
                ui.info_cursor = cursor_move(ui.info_cursor, -1, INFO__COUNT);
            } else if (e->button == BTN_DOWN) {
                ui.info_cursor = cursor_move(ui.info_cursor, +1, INFO__COUNT);
            } else if (e->button == BTN_SELECT && e->kind == BTN_EVT_CLICK) {
                if (ui.info_cursor == INFO_BACK) {
                    ui.state = UI_STATE_DEVICE_LIST;
                }
                /* INFO_FORGET is a placeholder in Phase 2 — Phase 3
                 * will hook this to the actual TLV + bonds wipe. */
            }
            break;
    }
}

static void check_inactivity(void) {
    if (ui.state == UI_STATE_STATUS) return;
    if (time_us_64() - ui.last_input_us < UI_INACTIVITY_US) return;
    /* Time-out -> snap back to status, reset cursors so re-entering
     * starts fresh. */
    ui.state        = UI_STATE_STATUS;
    ui.menu_cursor  = 0;
    ui.list_cursor  = 0;
    ui.info_cursor  = 0;
    ui.dirty        = true;
}

/* ---- Public API ----------------------------------------------------- */

bool ui_init(void) {
    memset(&ui, 0, sizeof(ui));
    ui.state         = UI_STATE_STATUS;
    ui.last_input_us = time_us_64();
    ui.oled_ok       = oled_init();
    ui.initialised   = true;
    if (ui.oled_ok) {
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

    drain_bt_events();

    /* Process all queued button events before deciding to redraw —
     * a flurry of UP presses then a SELECT should resolve to the
     * post-SELECT screen, not flash through every intermediate
     * cursor position. */
    button_event_t bev;
    while (buttons_poll(&bev)) {
        printf("[btn] event button=%d (%s) kind=%s -> state=%d\n",
               bev.button,
               bev.button == BTN_UP ? "UP" :
               bev.button == BTN_DOWN ? "DOWN" :
               bev.button == BTN_SELECT ? "SELECT" : "?",
               bev.kind == BTN_EVT_LONG ? "LONG" : "CLICK",
               (int)ui.state);
        handle_button_event(&bev);
    }

    /* Button bring-up diagnostics (#22).  Polls levels every frame
     * (latching lows) and prints once a second.  Remove (or gate
     * behind a flag) before merge. */
    buttons_debug_tick();

    check_inactivity();

    /* Status screen has its own redraw-skip path keyed on a state
     * signature.  The menu/list/info screens redraw whenever dirty
     * (set by handle_button_event) — they're mostly static between
     * input events. */
    if (ui.state == UI_STATE_STATUS) {
        uint32_t active_hash = hash_active_table();
        if (!ui.dirty &&
            state->active_output    == ui.last_active_output &&
            bt_events_active_count() == ui.last_active_count &&
            bt_events_bonded_count() == ui.last_bonded_count &&
            active_hash              == ui.last_active_hash) {
            return;
        }
        render_status(state->active_output);
        ui.last_active_output = state->active_output;
        ui.last_active_count  = bt_events_active_count();
        ui.last_bonded_count  = bt_events_bonded_count();
        ui.last_active_hash   = active_hash;
        ui.dirty              = false;
        return;
    }

    /* Non-status states: redraw on dirty OR on a deeper-state visible
     * change (e.g. a device disconnects while DEVICE_LIST is open and
     * the row should disappear).  Cheapest correct check: also watch
     * the active-table hash for the list / info states. */
    bool needs_redraw = ui.dirty;
    if (ui.state == UI_STATE_DEVICE_LIST || ui.state == UI_STATE_DEVICE_INFO) {
        uint32_t h = hash_active_table();
        if (h != ui.last_active_hash) {
            ui.last_active_hash = h;
            needs_redraw = true;
        }
    }
    if (!needs_redraw) return;

    switch (ui.state) {
        case UI_STATE_MAIN_MENU:   render_main_menu();   break;
        case UI_STATE_DEVICE_LIST: render_device_list(); break;
        case UI_STATE_DEVICE_INFO: render_device_info(); break;
        default:                   render_status(state->active_output); break;
    }
    ui.dirty = false;
}

#endif /* DH_OLED_UI */
