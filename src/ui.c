/*
 * OLED UI implementation — issue #22.
 *
 * Phase 1: always-on status screen.
 * Phase 2: button input + read-only menu navigation.
 * Phase 3 (this revision): device-type icons, plus the action flows —
 *   pair-new (discoverable countdown), forget-device, and forget-all,
 *   all wired to the real BTstack bond DB via bt_hid_host_le_forget*().
 *
 *   STATUS ── SELECT ──▶ MAIN_MENU ──┬─▶ DEVICE_LIST ─▶ DEVICE_INFO
 *     ▲   (10 s idle)                │                     │ SEL Forget
 *     │                              │                     ▼
 *     │                              ├─▶ PAIR_NEW       CONFIRM_FORGET
 *     │                              └─▶ CONFIRM_FORGET_ALL
 *     └──────────────────────────────────────────────────┘
 *   long-press SELECT = back at every depth; 10 s of no input in any
 *   non-STATUS state reverts to STATUS.
 *
 * Re-render policy: tick 30 Hz.  STATUS and the menus redraw only when
 * their content changes (dirty flag or a BT-state signature delta).
 * PAIR_NEW redraws when its countdown second or spinner frame ticks,
 * so the animation runs without blasting the I2C bus every frame.
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
#ifdef DH_BT_HID_HOST_KBD
#include "bt_hid_host_le.h"   /* bt_hid_host_le_forget / _forget_all */
#endif

/* ---- State machine -------------------------------------------------- */

typedef enum {
    UI_STATE_STATUS            = 0,
    UI_STATE_MAIN_MENU         = 1,
    UI_STATE_DEVICE_LIST       = 2,
    UI_STATE_DEVICE_INFO       = 3,
    UI_STATE_PAIR_NEW          = 4,
    UI_STATE_CONFIRM_FORGET    = 5,
    UI_STATE_CONFIRM_FORGET_ALL= 6,
} ui_state_t;

/* MAIN_MENU items (indices) — order is rendered order. */
enum {
    MENU_DEVICES    = 0,
    MENU_PAIR_NEW   = 1,
    MENU_FORGET_ALL = 2,
    MENU__COUNT
};

/* DEVICE_INFO items. */
enum {
    INFO_BACK   = 0,
    INFO_FORGET = 1,
    INFO__COUNT
};

/* Confirm-screen items.  Cancel is index 0 so it's the default cursor
 * position — fat-finger safety on a destructive action. */
enum {
    CONFIRM_CANCEL = 0,
    CONFIRM_YES    = 1,
    CONFIRM__COUNT
};

/* 10 s of no button input -> auto-revert to status from any deeper
 * state.  Held in microseconds since boot. */
#define UI_INACTIVITY_US   (10ull * 1000000ull)
/* Pair-new discoverable window. */
#define UI_PAIR_WINDOW_US  (60ull * 1000000ull)
/* How long the "Paired: X" success splash stays before returning to
 * the status screen. */
#define UI_PAIR_SPLASH_US  (3ull * 1000000ull)

static struct {
    ui_state_t state;
    bool       initialised;
    bool       oled_ok;

    /* Cursor positions per screen — preserved across back/re-enter. */
    uint8_t    menu_cursor;
    uint8_t    list_cursor;
    uint8_t    info_cursor;
    uint8_t    confirm_cursor;

    /* DEVICE_INFO tracks its device by stable cid (the active table
     * may shuffle if another device disconnects while we're here). */
    uint16_t   info_cid;

    /* CONFIRM_FORGET target — latched on entry so the forget acts on
     * the right device even if the table changes underneath us. */
    uint8_t    forget_addr[6];
    char       forget_name[BT_EVT_NAME_MAX];

    /* PAIR_NEW state. */
    uint64_t   pair_deadline_us;     /* discoverable window end */
    bool       pair_succeeded;       /* a device paired during the window */
    uint64_t   pair_splash_until_us; /* show success splash until this time */
    char       pair_name[BT_EVT_NAME_MAX];
    uint8_t    last_pair_secs;       /* redraw tracking */
    uint8_t    last_pair_spin;       /* redraw tracking */

    /* Inactivity tracking — updated on every button event. */
    uint64_t   last_input_us;

    /* STATUS-screen redraw-skip signature. */
    uint8_t    last_active_output;
    uint8_t    last_active_count;
    uint8_t    last_bonded_count;
    uint32_t   last_active_hash;

    /* Force a redraw next tick (set on any button event / state change). */
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

/* Find the active table index for a given cid.  -1 if not connected. */
static int find_active_index_by_cid(uint16_t cid) {
    const bt_active_entry_t *t = bt_events_active_table();
    for (int i = 0; i < BT_ACTIVE_CAP; i++) {
        if (t[i].in_use && t[i].cid == cid) return i;
    }
    return -1;
}

static int count_active(void) {
    return (int)bt_events_active_count();
}

/* Move cursor up/down with wrap.  count must be > 0 — callers check. */
static uint8_t cursor_move(uint8_t cur, int delta, uint8_t count) {
    if (count == 0) return 0;
    int v = (int)cur + delta;
    while (v < 0)            v += count;
    while (v >= (int)count)  v -= count;
    return (uint8_t)v;
}

static const uint8_t *icon_for_kind(bt_evt_kind_t kind) {
    switch (kind) {
        case BT_KIND_KEYBOARD: return oled_icon_kbd;
        case BT_KIND_MOUSE:    return oled_icon_mouse;
        case BT_KIND_KEYPAD:   return oled_icon_keypad;
        default:               return oled_icon_generic;
    }
}

/* Drain BT events.  In PAIR_NEW we react to a fresh pairing so the
 * screen can confirm success; otherwise events just keep the queue
 * from filling (bt_events.c owns the active-table cache). */
static void drain_bt_events(void) {
    bt_event_t evt;
    while (bt_events_poll(&evt)) {
        if (ui.state == UI_STATE_PAIR_NEW && !ui.pair_succeeded &&
            evt.type == BT_EVT_DEVICE_PAIRED) {
            ui.pair_succeeded      = true;
            ui.pair_splash_until_us = time_us_64() + UI_PAIR_SPLASH_US;
            if (evt.name[0])
                strncpy(ui.pair_name, evt.name, BT_EVT_NAME_MAX - 1);
            else
                fmt_addr_short(ui.pair_name, BT_EVT_NAME_MAX, evt.addr.bytes);
            ui.pair_name[BT_EVT_NAME_MAX - 1] = '\0';
            ui.dirty = true;
        }
    }
}

/* ---- Render: STATUS ------------------------------------------------- */

static void render_status(uint8_t active_output) {
    const bt_active_entry_t *active = bt_events_active_table();
    uint8_t active_count = bt_events_active_count();
    uint8_t bonded_count = bt_events_bonded_count();

    oled_clear();

    char letter[2] = { (active_output == 0) ? 'A' : 'B', '\0' };
    oled_text_at(0, 0, 2, letter);

    const char *bt_state;
    if (active_count > 0)      bt_state = "BT: ready";
    else if (bonded_count > 0) bt_state = "BT: idle";
    else                       bt_state = "BT: empty";
    oled_text_at(28, 0, 1, bt_state);

    char counter[24];
    snprintf(counter, sizeof(counter), "%u of %u bonded",
             active_count, bonded_count);
    oled_text_at(28, 8, 1, counter);

    for (int x = 0; x < OLED_W; x++) oled_set_pixel(x, 17, true);

    for (int i = 0; i < BT_ACTIVE_CAP; i++) {
        int y = 20 + i * 8;
        if (active[i].in_use) {
            oled_draw_icon(0, y, 8, 8, icon_for_kind(active[i].kind));
            char line[22];
            if (active[i].name[0]) {
                snprintf(line, sizeof(line), "%s", active[i].name);
            } else {
                fmt_addr_short(line, sizeof(line), active[i].addr.bytes);
            }
            oled_text_at(10, y, 1, line);
            oled_draw_icon(120, y, 8, 8, oled_icon_dot_full);
        } else {
            oled_draw_icon(0, y, 8, 8, oled_icon_dot_empty);
            oled_text_at(10, y, 1, "(slot free)");
        }
    }

    for (int x = 8; x < OLED_W - 8; x += 4) oled_set_pixel(x, 53, true);
    oled_text_at(0, 56, 1, "SEL: menu");
    oled_flush();
}

/* ---- Render: MAIN_MENU --------------------------------------------- */

static const char *MAIN_MENU_LABELS[MENU__COUNT] = {
    [MENU_DEVICES]    = "Paired devices",
    [MENU_PAIR_NEW]   = "Pair new device",
    [MENU_FORGET_ALL] = "Forget all bonds",
};

static void render_menu_list(const char *title, const char *const *labels,
                             int count, uint8_t cursor, const char *footer) {
    oled_clear();
    oled_text(0, 0, title);
    for (int x = 0; x < OLED_W; x++) oled_set_pixel(x, 9, true);
    for (int i = 0; i < count; i++) {
        int row = 2 + i;
        oled_text(6, row, labels[i]);
        if (i == cursor) {
            oled_text(0, row, ">");
            for (int x = 0; x < OLED_W; x++) oled_set_pixel(x, row * 8 + 7, true);
        }
    }
    oled_text(0, 7, footer);
    oled_flush();
}

static void render_main_menu(void) {
    render_menu_list("Menu", MAIN_MENU_LABELS, MENU__COUNT,
                     ui.menu_cursor, "UP/DN SEL  hold=back");
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
        oled_text(0, 3, "(none connected)");
        oled_text(0, 7, "hold SEL: back");
        oled_flush();
        return;
    }

    int visible_idx = 0;
    int row = 2;
    for (int i = 0; i < BT_ACTIVE_CAP && row <= 6; i++) {
        if (!active[i].in_use) continue;
        oled_draw_icon(8, row * 8, 8, 8, icon_for_kind(active[i].kind));
        char line[20];
        if (active[i].name[0]) {
            snprintf(line, sizeof(line), "%s", active[i].name);
        } else {
            fmt_addr_short(line, sizeof(line), active[i].addr.bytes);
        }
        oled_text(18, row, line);
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
    [INFO_FORGET] = "Forget device",
};

static void render_device_info(void) {
    int idx = find_active_index_by_cid(ui.info_cid);
    const bt_active_entry_t *active = bt_events_active_table();

    oled_clear();
    if (idx < 0) {
        oled_text(0, 0, "(disconnected)");
        for (int x = 0; x < OLED_W; x++) oled_set_pixel(x, 9, true);
        oled_text(0, 3, "Device went away.");
        oled_text(0, 7, "hold SEL: back");
        oled_flush();
        return;
    }

    const bt_active_entry_t *d = &active[idx];

    if (d->name[0]) {
        char title[24];
        snprintf(title, sizeof(title), "%s", d->name);
        oled_text(0, 0, title);
    } else {
        oled_text(0, 0, "(unnamed)");
    }
    for (int x = 0; x < OLED_W; x++) oled_set_pixel(x, 9, true);

    char addr[20];
    fmt_addr_full(addr, sizeof(addr), d->addr.bytes);
    oled_text(0, 2, addr);

    const char *kind_str =
        d->kind == BT_KIND_KEYBOARD ? "keyboard" :
        d->kind == BT_KIND_MOUSE    ? "mouse" :
        d->kind == BT_KIND_KEYPAD   ? "keypad" : "device";
    char meta[24];
    snprintf(meta, sizeof(meta), "%s %s",
             d->transport == BT_TRANSPORT_LE ? "BLE" : "Classic", kind_str);
    oled_text(0, 3, meta);

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

/* ---- Render: PAIR_NEW ---------------------------------------------- */

static void render_pair_new(void) {
    oled_clear();
    oled_text(0, 0, "Pair new device");
    for (int x = 0; x < OLED_W; x++) oled_set_pixel(x, 9, true);

    if (ui.pair_succeeded) {
        oled_text(0, 3, "Paired!");
        char line[BT_EVT_NAME_MAX + 2];
        snprintf(line, sizeof(line), "%s", ui.pair_name);
        oled_text(0, 4, line);
        oled_text(0, 7, "SEL: done");
        oled_flush();
        return;
    }

    if (count_active() >= BT_ACTIVE_CAP) {
        /* No free hids_client slot — pairing can't take.  Tell the user
         * to forget something first instead of spinning forever. */
        oled_text(0, 3, "All slots full.");
        oled_text(0, 4, "Forget one first.");
        oled_text(0, 7, "SEL: cancel");
        oled_flush();
        return;
    }

    uint64_t now = time_us_64();
    int secs = 0;
    if (ui.pair_deadline_us > now)
        secs = (int)((ui.pair_deadline_us - now) / 1000000ull);

    static const char spin[4] = { '|', '/', '-', '\\' };
    uint8_t spin_idx = (uint8_t)((now / 250000ull) & 3);

    char hdr[22];
    snprintf(hdr, sizeof(hdr), "%c Scanning  %2ds", spin[spin_idx], secs);
    oled_text(0, 3, hdr);
    oled_text(0, 5, "Put device in");
    oled_text(0, 6, "pairing mode now.");
    oled_text(0, 7, "SEL: cancel");
    oled_flush();
}

/* ---- Render: confirm screens --------------------------------------- */

static void render_confirm(const char *line1, const char *line2) {
    oled_clear();
    oled_text(0, 0, line1);
    for (int x = 0; x < OLED_W; x++) oled_set_pixel(x, 9, true);
    if (line2[0]) oled_text(0, 2, line2);
    oled_text(0, 3, "This wipes the");
    oled_text(0, 4, "bond from flash.");

    const char *opts[CONFIRM__COUNT] = {
        [CONFIRM_CANCEL] = "Cancel",
        [CONFIRM_YES]    = "Forget",
    };
    for (int i = 0; i < CONFIRM__COUNT; i++) {
        int row = 5 + i;
        oled_text(6, row, opts[i]);
        if (i == ui.confirm_cursor) {
            oled_text(0, row, ">");
            for (int x = 0; x < OLED_W; x++) oled_set_pixel(x, row * 8 + 7, true);
        }
    }
    oled_text(0, 7, "UP/DN SEL pick");
    oled_flush();
}

static void render_confirm_forget(void) {
    char title[22];
    snprintf(title, sizeof(title), "Forget device?");
    char name[BT_EVT_NAME_MAX + 2];
    if (ui.forget_name[0]) snprintf(name, sizeof(name), "%s", ui.forget_name);
    else                   fmt_addr_short(name, sizeof(name), ui.forget_addr);
    render_confirm(title, name);
}

static void render_confirm_forget_all(void) {
    render_confirm("Forget ALL bonds?", "every paired dev");
}

/* ---- Action helpers ------------------------------------------------- */

static void do_forget_current(void) {
#ifdef DH_BT_HID_HOST_KBD
    bt_hid_host_le_forget(ui.forget_addr);
#endif
}

static void do_forget_all(void) {
#ifdef DH_BT_HID_HOST_KBD
    bt_hid_host_le_forget_all();
#endif
}

static void enter_pair_new(void) {
    ui.state              = UI_STATE_PAIR_NEW;
    ui.pair_deadline_us   = time_us_64() + UI_PAIR_WINDOW_US;
    ui.pair_succeeded     = false;
    ui.pair_splash_until_us = 0;
    ui.pair_name[0]       = '\0';
    ui.last_pair_secs     = 0xFF;   /* force first render */
    ui.last_pair_spin     = 0xFF;
}

/* ---- Input handling ------------------------------------------------- */

static void handle_button_event(const button_event_t *e) {
    ui.last_input_us = e->release_us;
    ui.dirty         = true;

    bool sel_click = (e->button == BTN_SELECT && e->kind == BTN_EVT_CLICK);
    bool sel_long  = (e->button == BTN_SELECT && e->kind == BTN_EVT_LONG);

    switch (ui.state) {
        case UI_STATE_STATUS:
            if (sel_click) {
                ui.state       = UI_STATE_MAIN_MENU;
                ui.menu_cursor = 0;
            }
            break;

        case UI_STATE_MAIN_MENU:
            if (sel_long) { ui.state = UI_STATE_STATUS; break; }
            if (e->button == BTN_UP)
                ui.menu_cursor = cursor_move(ui.menu_cursor, -1, MENU__COUNT);
            else if (e->button == BTN_DOWN)
                ui.menu_cursor = cursor_move(ui.menu_cursor, +1, MENU__COUNT);
            else if (sel_click) {
                switch (ui.menu_cursor) {
                    case MENU_DEVICES:
                        ui.state       = UI_STATE_DEVICE_LIST;
                        ui.list_cursor = 0;
                        break;
                    case MENU_PAIR_NEW:
                        enter_pair_new();
                        break;
                    case MENU_FORGET_ALL:
                        ui.state          = UI_STATE_CONFIRM_FORGET_ALL;
                        ui.confirm_cursor = CONFIRM_CANCEL;
                        break;
                }
            }
            break;

        case UI_STATE_DEVICE_LIST:
            if (sel_long) { ui.state = UI_STATE_MAIN_MENU; break; }
            {
                int n = count_active();
                if (n == 0) break;   /* only back makes sense */
                if (e->button == BTN_UP)
                    ui.list_cursor = cursor_move(ui.list_cursor, -1, (uint8_t)n);
                else if (e->button == BTN_DOWN)
                    ui.list_cursor = cursor_move(ui.list_cursor, +1, (uint8_t)n);
                else if (sel_click) {
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
            if (sel_long) { ui.state = UI_STATE_DEVICE_LIST; break; }
            if (e->button == BTN_UP)
                ui.info_cursor = cursor_move(ui.info_cursor, -1, INFO__COUNT);
            else if (e->button == BTN_DOWN)
                ui.info_cursor = cursor_move(ui.info_cursor, +1, INFO__COUNT);
            else if (sel_click) {
                if (ui.info_cursor == INFO_BACK) {
                    ui.state = UI_STATE_DEVICE_LIST;
                } else if (ui.info_cursor == INFO_FORGET) {
                    /* Latch the target device's addr + name so the
                     * confirm + forget act on the right device even if
                     * the active table shifts. */
                    int idx = find_active_index_by_cid(ui.info_cid);
                    if (idx >= 0) {
                        const bt_active_entry_t *d = &bt_events_active_table()[idx];
                        memcpy(ui.forget_addr, d->addr.bytes, 6);
                        strncpy(ui.forget_name, d->name, BT_EVT_NAME_MAX - 1);
                        ui.forget_name[BT_EVT_NAME_MAX - 1] = '\0';
                        ui.state          = UI_STATE_CONFIRM_FORGET;
                        ui.confirm_cursor = CONFIRM_CANCEL;
                    } else {
                        /* Device vanished — just go back to the list. */
                        ui.state = UI_STATE_DEVICE_LIST;
                    }
                }
            }
            break;

        case UI_STATE_PAIR_NEW:
            /* SELECT dismisses — whether confirming a success splash or
             * cancelling the scan.  Long-press also backs out. */
            if (sel_click || sel_long) {
                ui.state = ui.pair_succeeded ? UI_STATE_STATUS
                                             : UI_STATE_MAIN_MENU;
            }
            break;

        case UI_STATE_CONFIRM_FORGET:
            if (sel_long) { ui.state = UI_STATE_DEVICE_INFO; break; }
            if (e->button == BTN_UP)
                ui.confirm_cursor = cursor_move(ui.confirm_cursor, -1, CONFIRM__COUNT);
            else if (e->button == BTN_DOWN)
                ui.confirm_cursor = cursor_move(ui.confirm_cursor, +1, CONFIRM__COUNT);
            else if (sel_click) {
                if (ui.confirm_cursor == CONFIRM_YES) {
                    do_forget_current();
                    ui.state = UI_STATE_STATUS;   /* device is gone now */
                } else {
                    ui.state = UI_STATE_DEVICE_INFO;
                }
            }
            break;

        case UI_STATE_CONFIRM_FORGET_ALL:
            if (sel_long) { ui.state = UI_STATE_MAIN_MENU; break; }
            if (e->button == BTN_UP)
                ui.confirm_cursor = cursor_move(ui.confirm_cursor, -1, CONFIRM__COUNT);
            else if (e->button == BTN_DOWN)
                ui.confirm_cursor = cursor_move(ui.confirm_cursor, +1, CONFIRM__COUNT);
            else if (sel_click) {
                if (ui.confirm_cursor == CONFIRM_YES) {
                    do_forget_all();
                    ui.state = UI_STATE_STATUS;
                } else {
                    ui.state = UI_STATE_MAIN_MENU;
                }
            }
            break;
    }
}

static void check_inactivity(void) {
    if (ui.state == UI_STATE_STATUS) return;
    /* PAIR_NEW has its own timing (handled in the render task); don't
     * let the generic inactivity timer yank it away mid-scan. */
    if (ui.state == UI_STATE_PAIR_NEW) return;
    if (time_us_64() - ui.last_input_us < UI_INACTIVITY_US) return;
    ui.state          = UI_STATE_STATUS;
    ui.menu_cursor    = 0;
    ui.list_cursor    = 0;
    ui.info_cursor    = 0;
    ui.confirm_cursor = 0;
    ui.dirty          = true;
}

/* PAIR_NEW timing: expire the scan window, or dismiss the success
 * splash.  Returns true if a redraw is warranted (countdown second or
 * spinner frame changed). */
static bool pair_new_tick(void) {
    uint64_t now = time_us_64();

    if (ui.pair_succeeded) {
        if (now >= ui.pair_splash_until_us) {
            ui.state = UI_STATE_STATUS;
            ui.dirty = true;
        }
        return false;
    }

    if (now >= ui.pair_deadline_us) {
        ui.state = UI_STATE_MAIN_MENU;   /* window elapsed */
        ui.dirty = true;
        return false;
    }

    /* Redraw when the displayed second or spinner frame changes. */
    uint8_t secs = (uint8_t)((ui.pair_deadline_us - now) / 1000000ull);
    uint8_t spin = (uint8_t)((now / 250000ull) & 3);
    bool changed = (secs != ui.last_pair_secs) || (spin != ui.last_pair_spin);
    ui.last_pair_secs = secs;
    ui.last_pair_spin = spin;
    return changed;
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
        oled_text(0, 3, "  OLED UI");
        oled_text(0, 5, "  awaiting BT...");
        oled_flush();
    }
    return ui.oled_ok;
}

void ui_render_task(device_t *state) {
    if (!ui.initialised || !ui.oled_ok) return;

    drain_bt_events();

    buttons_task();
    button_event_t bev;
    while (buttons_poll(&bev)) {
        handle_button_event(&bev);
    }

    check_inactivity();

    /* PAIR_NEW drives its own timing/animation redraws. */
    bool pair_redraw = false;
    if (ui.state == UI_STATE_PAIR_NEW)
        pair_redraw = pair_new_tick();

    /* STATUS: redraw only on a content-signature change. */
    if (ui.state == UI_STATE_STATUS) {
        uint32_t active_hash = hash_active_table();
        if (!ui.dirty &&
            state->active_output     == ui.last_active_output &&
            bt_events_active_count()  == ui.last_active_count &&
            bt_events_bonded_count()  == ui.last_bonded_count &&
            active_hash               == ui.last_active_hash) {
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

    /* Other states: redraw on dirty, on a pair-new animation tick, or
     * (for the device list/info) when the active table changes under us. */
    bool needs_redraw = ui.dirty || pair_redraw;
    if (ui.state == UI_STATE_DEVICE_LIST || ui.state == UI_STATE_DEVICE_INFO) {
        uint32_t h = hash_active_table();
        if (h != ui.last_active_hash) {
            ui.last_active_hash = h;
            needs_redraw = true;
        }
    }
    if (!needs_redraw) return;

    switch (ui.state) {
        case UI_STATE_MAIN_MENU:          render_main_menu();         break;
        case UI_STATE_DEVICE_LIST:        render_device_list();       break;
        case UI_STATE_DEVICE_INFO:        render_device_info();       break;
        case UI_STATE_PAIR_NEW:           render_pair_new();          break;
        case UI_STATE_CONFIRM_FORGET:     render_confirm_forget();    break;
        case UI_STATE_CONFIRM_FORGET_ALL: render_confirm_forget_all();break;
        default:                          render_status(state->active_output); break;
    }
    ui.dirty = false;
}

#endif /* DH_OLED_UI */
