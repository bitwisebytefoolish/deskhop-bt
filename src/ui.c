/*
 * OLED UI implementation — issue #22.
 *
 * Phase 1: always-on status screen.
 * Phase 2: button input + menu navigation.
 * Phase 3: device-type icons + action flows — pair-new, forget,
 *          forget-all.
 * Phase 3 fixes (this revision):
 *   - Device list/info are now driven by the BOND DB, not the
 *     connected-devices table, so every bonded device is listed
 *     (connected or not) with a connection-status dot.
 *   - Pair-new opens an explicit pairing window on the host; outside
 *     that window the host ignores unbonded advertisers, so forget
 *     actually sticks (the device can't silently re-pair).
 *   - Forget acts on a bonded device by address (works whether the
 *     device is currently connected or not).
 *
 *   STATUS ── SELECT ──▶ MAIN_MENU ──┬─▶ DEVICE_LIST ─▶ DEVICE_INFO
 *     ▲   (10 s idle)                │   (all bonds)        │ Forget
 *     │                              │                      ▼
 *     │                              ├─▶ PAIR_NEW       CONFIRM_FORGET
 *     │                              └─▶ CONFIRM_FORGET_ALL
 *     └──────────────────────────────────────────────────┘
 *   long-press SELECT = back; 10 s idle (except PAIR_NEW) → STATUS.
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
#include "bt_hid_host_le.h"   /* forget / pairing-window / bond-enum API */

/* ---- State machine -------------------------------------------------- */

typedef enum {
    UI_STATE_STATUS             = 0,
    UI_STATE_MAIN_MENU          = 1,
    UI_STATE_DEVICE_LIST        = 2,
    UI_STATE_DEVICE_INFO        = 3,
    UI_STATE_PAIR_NEW           = 4,
    UI_STATE_CONFIRM_FORGET     = 5,
    UI_STATE_CONFIRM_FORGET_ALL = 6,
    UI_STATE_ABOUT              = 7,
} ui_state_t;

enum { MENU_DEVICES = 0, MENU_PAIR_NEW = 1, MENU_FORGET_ALL = 2, MENU_ABOUT = 3, MENU__COUNT };
enum { INFO_BACK = 0, INFO_FORGET = 1, INFO__COUNT };
enum { CONFIRM_CANCEL = 0, CONFIRM_YES = 1, CONFIRM__COUNT };

#define UI_INACTIVITY_US   (10ull * 1000000ull)
#define UI_PAIR_WINDOW_US  (60ull * 1000000ull)
#define UI_PAIR_SPLASH_US  (3ull  * 1000000ull)
#define UI_BONDS_MAX       16     /* device-list capacity (bond DB holds 32) */
#define UI_LIST_ROWS       5      /* visible rows on the device list */

static struct {
    ui_state_t state;
    bool       initialised;
    bool       oled_ok;

    uint8_t    menu_cursor;
    uint8_t    list_cursor;     /* index into the bond list */
    uint8_t    info_cursor;
    uint8_t    confirm_cursor;

    /* Bond list snapshot (refreshed on the list/info screens). */
    bt_bond_info_t bonds[UI_BONDS_MAX];
    uint8_t        bond_count;
    uint32_t       last_bonds_hash;

    /* DEVICE_INFO + CONFIRM_FORGET target, latched by address so it
     * survives connect/disconnect churn under us. */
    uint8_t    sel_addr[6];
    char       sel_name[BT_EVT_NAME_MAX];

    /* PAIR_NEW. */
    uint64_t   pair_deadline_us;
    bool       pair_succeeded;
    uint64_t   pair_splash_until_us;
    char       pair_name[BT_EVT_NAME_MAX];
    uint8_t    last_pair_secs;
    uint8_t    last_pair_spin;

    uint64_t   last_input_us;

    /* STATUS redraw-skip signature. */
    uint8_t    last_active_output;
    uint8_t    last_active_count;
    uint8_t    last_bonded_count;
    uint32_t   last_active_hash;
    uint32_t   last_anim_phase;   /* BT-icon blink + name-marquee tick */

    bool       radio_up;          /* BT_EVT_RADIO_UP seen — stop blinking */

    bool       dirty;
} ui;

/* Visible columns for a device name before the marquee kicks in.  Names
 * longer than this scroll (see marquee()). */
#define UI_NAME_COLS 16

/* Button-hint tokens: UP/DOWN arrows + SELECT circle glyphs (font slots
 * 0x80..0x82).  "UP/DN" -> "↑/↓", "SEL" -> "○". */
#define UI_H_UPDN  OLED_S_ARROW_UP "/" OLED_S_ARROW_DOWN
#define UI_H_SEL   OLED_S_SEL

/* Pair-new "scanning" ping-pong: a dot bounces left/right across this
 * many character cells, dwelling UI_BOUNCE_STEP_US per cell. */
#define UI_BOUNCE_CELLS   16
#define UI_BOUNCE_STEP_US 110000ull

/* ---- Helpers -------------------------------------------------------- */

static uint32_t fnv1a(const void *data, size_t n) {
    const uint8_t *p = data;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
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

/* Draw a string horizontally centred on `row` (6 px/char, 128 px wide). */
static void oled_text_centered(int row, const char *s) {
    int w = (int)strlen(s) * 6;
    int x = (OLED_W - w) / 2;
    if (x < 0) x = 0;
    oled_text(x, row, s);
}

/* Marquee for names longer than `win` columns.  Writes the substring to
 * draw this frame into `out`.  While more text remains to the right the
 * final three cells show "..." as an overflow hint; once the tail is
 * fully revealed the hint drops, the line holds for 2 s, then the cycle
 * restarts from the left.  Purely time-driven (no per-name state) so all
 * long names animate from one shared clock derived from `now`. */
static void marquee(char *out, size_t outsz, const char *name, int win, uint64_t now) {
    int len = (int)strlen(name);
    if (len <= win) { snprintf(out, outsz, "%s", name); return; }

    int      steps  = len - win;             /* shifts to reveal the tail */
    uint64_t period = (uint64_t)steps + 2;   /* + 2 s hold at the end     */
    uint64_t phase  = (now / 1000000ull) % period;

    int  off;
    bool ellipsis;
    if (phase < (uint64_t)steps) { off = (int)phase; ellipsis = true;  }
    else                         { off = steps;      ellipsis = false; }

    char buf[40];
    int  n = win;
    if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
    memcpy(buf, name + off, (size_t)n);
    buf[n] = '\0';
    /* Compact overflow hint: a single-cell "…" in the last column (the
     * font renders OLED_CH_ELLIPSIS as three baseline dots in one glyph
     * width) rather than three '.' characters eating three cells. */
    if (ellipsis && win >= 1) buf[win - 1] = OLED_CH_ELLIPSIS;
    snprintf(out, outsz, "%s", buf);
}

/* True if any visible name on the status screen needs the marquee — used
 * to decide whether STATUS must redraw on the 1 Hz animation tick. */
static bool status_has_long_name(const bt_active_entry_t *active) {
    for (int i = 0; i < BT_ACTIVE_CAP; i++)
        if (active[i].in_use && (int)strlen(active[i].name) > UI_NAME_COLS)
            return true;
    return false;
}

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

/* The bond DB doesn't store device kind, so connected devices borrow
 * the kind from the active table; disconnected ones get the generic
 * glyph. */
static bt_evt_kind_t kind_for_addr(const uint8_t addr[6]) {
    const bt_active_entry_t *t = bt_events_active_table();
    for (int i = 0; i < BT_ACTIVE_CAP; i++)
        if (t[i].in_use && memcmp(t[i].addr.bytes, addr, 6) == 0)
            return t[i].kind;
    return BT_KIND_UNKNOWN;
}

static void ui_fetch_bonds(void) {
    ui.bond_count = (uint8_t)bt_hid_host_le_get_bonds(ui.bonds, UI_BONDS_MAX);
    if (ui.list_cursor >= ui.bond_count)
        ui.list_cursor = ui.bond_count ? ui.bond_count - 1 : 0;
}

static void close_pairing_window(void) {
    bt_hid_host_le_set_pairing_open(false);
}

static void enter_pair_new(void) {
    ui.state                = UI_STATE_PAIR_NEW;
    ui.pair_deadline_us     = time_us_64() + UI_PAIR_WINDOW_US;
    ui.pair_succeeded       = false;
    ui.pair_splash_until_us = 0;
    ui.pair_name[0]         = '\0';
    ui.last_pair_secs       = 0xFF;
    ui.last_pair_spin       = 0xFF;
    bt_hid_host_le_set_pairing_open(true);
}

/* Drain BT events; in PAIR_NEW, react to a fresh pairing. */
static void drain_bt_events(void) {
    bt_event_t evt;
    while (bt_events_poll(&evt)) {
        if (evt.type == BT_EVT_RADIO_UP) { ui.radio_up = true; ui.dirty = true; }
        if (ui.state == UI_STATE_PAIR_NEW && !ui.pair_succeeded &&
            evt.type == BT_EVT_DEVICE_PAIRED) {
            ui.pair_succeeded       = true;
            ui.pair_splash_until_us = time_us_64() + UI_PAIR_SPLASH_US;
            if (evt.name[0]) strncpy(ui.pair_name, evt.name, BT_EVT_NAME_MAX - 1);
            else             fmt_addr_short(ui.pair_name, BT_EVT_NAME_MAX, evt.addr.bytes);
            ui.pair_name[BT_EVT_NAME_MAX - 1] = '\0';
            /* One device per pair-new session — stop accepting more. */
            close_pairing_window();
            ui.dirty = true;
        }
    }
}

/* ---- Render: STATUS ------------------------------------------------- */

static void render_status(uint8_t active_output) {
    const bt_active_entry_t *active = bt_events_active_table();
    uint8_t active_count = bt_events_active_count();
    uint8_t bonded_count = bt_events_bonded_count();
    uint64_t now = time_us_64();

    oled_clear();

    char letter[2] = { (active_output == 0) ? 'A' : 'B', '\0' };
    oled_text_at(0, 0, 2, letter);

    /* BT status: a Bluetooth icon + a state word.  While the radio is
     * still coming up the icon flashes at 1 Hz next to "init"; once
     * BT_EVT_RADIO_UP arrives the icon goes solid and the word reflects
     * connection state (ready / idle / empty). */
    bool blink_on = ui.radio_up || (((now / 500000ull) & 1ull) == 0);
    if (blink_on) oled_draw_icon(28, 0, 8, 8, oled_icon_bt);

    const char *bt_state;
    if (!ui.radio_up)          bt_state = "init";
    else if (active_count > 0) bt_state = "ready";
    else if (bonded_count > 0) bt_state = "idle";
    else                       bt_state = "empty";
    oled_text_at(40, 0, 1, bt_state);

    char counter[24];
    snprintf(counter, sizeof(counter), "%u of %u paired",
             active_count, bonded_count);
    oled_text_at(40, 8, 1, counter);

    for (int x = 0; x < OLED_W; x++) oled_set_pixel(x, 17, true);

    for (int i = 0; i < BT_ACTIVE_CAP; i++) {
        int y = 20 + i * 8;
        if (active[i].in_use) {
            oled_draw_icon(0, y, 8, 8, icon_for_kind(active[i].kind));
            char line[40];
            if (active[i].name[0]) marquee(line, sizeof(line), active[i].name, UI_NAME_COLS, now);
            else                   fmt_addr_short(line, sizeof(line), active[i].addr.bytes);
            oled_text_at(12, y, 1, line);
            oled_draw_icon(120, y, 8, 8, oled_icon_dot_full);
        } else {
            oled_draw_icon(0, y, 8, 8, oled_icon_dot_empty);
            oled_text_at(12, y, 1, "- free");
        }
    }

    for (int x = 8; x < OLED_W - 8; x += 4) oled_set_pixel(x, 53, true);
    oled_text_at(0, 56, 1, UI_H_SEL ": menu");
    oled_flush();
}

/* ---- Render: MAIN_MENU --------------------------------------------- */

static const char *MAIN_MENU_LABELS[MENU__COUNT] = {
    [MENU_DEVICES]    = "Paired devices",
    [MENU_PAIR_NEW]   = "Pair new device",
    [MENU_FORGET_ALL] = "Forget all bonds",
    [MENU_ABOUT]      = "About",
};

static void render_main_menu(void) {
    oled_clear();
    oled_text(0, 0, "Menu");
    for (int x = 0; x < OLED_W; x++) oled_set_pixel(x, 9, true);
    for (int i = 0; i < MENU__COUNT; i++) {
        int row = 2 + i;
        oled_text(6, row, MAIN_MENU_LABELS[i]);
        if (i == ui.menu_cursor)
            oled_text(0, row, ">");
    }
    oled_text(0, 7, UI_H_UPDN " " UI_H_SEL "  hold=back");
    oled_flush();
}

/* ---- Render: DEVICE_LIST (all bonds, scrolling) -------------------- */

static void render_device_list(void) {
    oled_clear();
    char hdr[24];
    snprintf(hdr, sizeof(hdr), "Paired devices %u", (unsigned)ui.bond_count);
    oled_text(0, 0, hdr);
    for (int x = 0; x < OLED_W; x++) oled_set_pixel(x, 9, true);

    if (ui.bond_count == 0) {
        oled_text(0, 3, "(no bonds yet)");
        oled_text(0, 5, "Menu > Pair new");
        oled_text(0, 7, "hold " UI_H_SEL ": back");
        oled_flush();
        return;
    }

    /* Scroll so the cursor stays visible. */
    int scroll = 0;
    if (ui.list_cursor >= UI_LIST_ROWS)
        scroll = ui.list_cursor - (UI_LIST_ROWS - 1);

    for (int r = 0; r < UI_LIST_ROWS; r++) {
        int idx = scroll + r;
        if (idx >= ui.bond_count) break;
        int row = 2 + r;
        const bt_bond_info_t *b = &ui.bonds[idx];

        oled_draw_icon(8, row * 8, 8, 8, icon_for_kind(kind_for_addr(b->addr)));
        char line[40];
        if (b->name[0]) marquee(line, sizeof(line), b->name, UI_NAME_COLS, time_us_64());
        else            fmt_addr_short(line, sizeof(line), b->addr);
        oled_text(20, row, line);
        oled_draw_icon(120, row * 8, 8, 8,
                       b->connected ? oled_icon_dot_full : oled_icon_dot_empty);
        if (idx == ui.list_cursor)
            oled_text(0, row, ">");
    }

    oled_text(0, 7, UI_H_UPDN " " UI_H_SEL " info");
    oled_flush();
}

/* ---- Render: DEVICE_INFO ------------------------------------------- */

static const char *INFO_LABELS[INFO__COUNT] = {
    [INFO_BACK]   = "Back",
    [INFO_FORGET] = "Forget device",
};

/* Find the latched device in the current bond snapshot.  NULL if it's
 * no longer bonded. */
static const bt_bond_info_t *find_sel_bond(void) {
    for (int i = 0; i < ui.bond_count; i++)
        if (memcmp(ui.bonds[i].addr, ui.sel_addr, 6) == 0)
            return &ui.bonds[i];
    return NULL;
}

static void render_device_info(void) {
    const bt_bond_info_t *b = find_sel_bond();

    oled_clear();
    if (!b) {
        oled_text(0, 0, "(gone)");
        for (int x = 0; x < OLED_W; x++) oled_set_pixel(x, 9, true);
        oled_text(0, 3, "No longer bonded.");
        oled_text(0, 7, "hold " UI_H_SEL ": back");
        oled_flush();
        return;
    }

    if (b->name[0]) { char t[24]; snprintf(t, sizeof(t), "%s", b->name); oled_text(0, 0, t); }
    else            oled_text(0, 0, "(unnamed)");
    for (int x = 0; x < OLED_W; x++) oled_set_pixel(x, 9, true);

    char addr[20];
    fmt_addr_full(addr, sizeof(addr), b->addr);
    oled_text(0, 2, addr);
    oled_text(0, 3, b->connected ? "status: connected" : "status: offline");

    for (int i = 0; i < INFO__COUNT; i++) {
        int row = 5 + i;
        oled_text(6, row, INFO_LABELS[i]);
        if (i == ui.info_cursor)
            oled_text(0, row, ">");
    }
    oled_text(0, 7, UI_H_UPDN " " UI_H_SEL " pick");
    oled_flush();
}

/* ---- Render: PAIR_NEW ---------------------------------------------- */

/* Ping-pong dot position (0 .. UI_BOUNCE_CELLS-1), bouncing off both
 * ends.  Triangle wave over time so it's deterministic from `now` with
 * no stored state. */
static uint8_t bounce_pos(uint64_t now) {
    int      span = UI_BOUNCE_CELLS - 1;
    uint64_t ph   = (now / UI_BOUNCE_STEP_US) % (uint64_t)(2 * span);
    return (uint8_t)((ph < (uint64_t)span) ? ph : (2 * span - ph));
}

static void render_pair_new(void) {
    oled_clear();
    oled_text(0, 0, "Pair new device");
    for (int x = 0; x < OLED_W; x++) oled_set_pixel(x, 9, true);

    if (ui.pair_succeeded) {
        oled_text(0, 3, "Paired!");
        char line[BT_EVT_NAME_MAX + 2];
        snprintf(line, sizeof(line), "%s", ui.pair_name);
        oled_text(0, 4, line);
        oled_text(0, 7, UI_H_SEL ": done");
        oled_flush();
        return;
    }

    if (bt_events_active_count() >= BT_ACTIVE_CAP) {
        oled_text(0, 3, "All slots full.");
        oled_text(0, 4, "Forget one first.");
        oled_text(0, 7, UI_H_SEL ": cancel");
        oled_flush();
        return;
    }

    uint64_t now = time_us_64();
    int secs = (ui.pair_deadline_us > now)
             ? (int)((ui.pair_deadline_us - now) / 1000000ull) : 0;

    char hdr[22];
    snprintf(hdr, sizeof(hdr), "Scanning   %2ds", secs);
    oled_text(0, 3, hdr);

    /* Ping-pong dot: bounces left/right across a blank field. */
    char bar[UI_BOUNCE_CELLS + 1];
    for (int i = 0; i < UI_BOUNCE_CELLS; i++) bar[i] = ' ';
    bar[bounce_pos(now)] = OLED_CH_SEL;
    bar[UI_BOUNCE_CELLS] = '\0';
    oled_text(0, 4, bar);

    oled_text(0, 5, "Put device in");
    oled_text(0, 6, "pairing mode now.");
    oled_text(0, 7, UI_H_SEL ": cancel");
    oled_flush();
}

/* ---- Render: confirm screens --------------------------------------- */

static void render_confirm(const char *line1, const char *line2) {
    oled_clear();
    oled_text(0, 0, line1);
    for (int x = 0; x < OLED_W; x++) oled_set_pixel(x, 9, true);
    if (line2[0]) oled_text(0, 2, line2);
    oled_text(0, 3, "Removes the bond.");
    oled_text(0, 4, "Re-pair to use.");

    const char *opts[CONFIRM__COUNT] = {
        [CONFIRM_CANCEL] = "Cancel",
        [CONFIRM_YES]    = "Forget",
    };
    for (int i = 0; i < CONFIRM__COUNT; i++) {
        int row = 5 + i;
        oled_text(6, row, opts[i]);
        if (i == ui.confirm_cursor)
            oled_text(0, row, ">");
    }
    oled_text(0, 7, UI_H_UPDN " " UI_H_SEL " pick");
    oled_flush();
}

static void render_confirm_forget(void) {
    char name[BT_EVT_NAME_MAX + 2];
    if (ui.sel_name[0]) snprintf(name, sizeof(name), "%s", ui.sel_name);
    else                fmt_addr_short(name, sizeof(name), ui.sel_addr);
    render_confirm("Forget device?", name);
}

static void render_confirm_forget_all(void) {
    render_confirm("Forget ALL bonds?", "every paired dev");
}

/* ---- Render: ABOUT -------------------------------------------------- */

static void render_about(void) {
    oled_clear();

    oled_text_centered(0, "Deskhop:");
    oled_text_centered(1, "Bluetooth Edition");

    char ver[16];
    snprintf(ver, sizeof(ver), "%d.%d.%d",
             VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH);
    oled_text_centered(2, ver);

    /* Horizontal rule below the version (row 2 ends at y=23). */
    for (int x = 0; x < OLED_W; x++) oled_set_pixel(x, 25, true);

    oled_text_centered(4, "Built by M. Rossoni");
    oled_text_centered(5, "Forked from hrvach's");
    oled_text_centered(6, "deskhop. Thank you!");
    oled_text_centered(7, "Build " BUILD_DATE);

    oled_flush();
}

/* ---- Input handling ------------------------------------------------- */

static void handle_button_event(const button_event_t *e) {
    ui.last_input_us = e->release_us;
    ui.dirty         = true;

    bool sel_click = (e->button == BTN_SELECT && e->kind == BTN_EVT_CLICK);
    bool sel_long  = (e->button == BTN_SELECT && e->kind == BTN_EVT_LONG);

    switch (ui.state) {
        case UI_STATE_STATUS:
            if (sel_click) { ui.state = UI_STATE_MAIN_MENU; ui.menu_cursor = 0; }
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
                        ui_fetch_bonds();
                        ui.list_cursor = 0;
                        ui.state = UI_STATE_DEVICE_LIST;
                        break;
                    case MENU_PAIR_NEW:
                        enter_pair_new();
                        break;
                    case MENU_FORGET_ALL:
                        ui.state = UI_STATE_CONFIRM_FORGET_ALL;
                        ui.confirm_cursor = CONFIRM_CANCEL;
                        break;
                    case MENU_ABOUT:
                        ui.state = UI_STATE_ABOUT;
                        break;
                }
            }
            break;

        case UI_STATE_ABOUT:
            /* Any SELECT (click or long-press) returns to the menu. */
            if (sel_click || sel_long) ui.state = UI_STATE_MAIN_MENU;
            break;

        case UI_STATE_DEVICE_LIST:
            if (sel_long) { ui.state = UI_STATE_MAIN_MENU; break; }
            if (ui.bond_count == 0) break;
            if (e->button == BTN_UP)
                ui.list_cursor = cursor_move(ui.list_cursor, -1, ui.bond_count);
            else if (e->button == BTN_DOWN)
                ui.list_cursor = cursor_move(ui.list_cursor, +1, ui.bond_count);
            else if (sel_click) {
                const bt_bond_info_t *b = &ui.bonds[ui.list_cursor];
                memcpy(ui.sel_addr, b->addr, 6);
                strncpy(ui.sel_name, b->name, BT_EVT_NAME_MAX - 1);
                ui.sel_name[BT_EVT_NAME_MAX - 1] = '\0';
                ui.info_cursor = 0;
                ui.state = UI_STATE_DEVICE_INFO;
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
                } else { /* INFO_FORGET */
                    ui.state = UI_STATE_CONFIRM_FORGET;
                    ui.confirm_cursor = CONFIRM_CANCEL;
                }
            }
            break;

        case UI_STATE_PAIR_NEW:
            if (sel_click || sel_long) {
                close_pairing_window();
                ui.state = ui.pair_succeeded ? UI_STATE_STATUS : UI_STATE_MAIN_MENU;
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
                    bt_hid_host_le_forget(ui.sel_addr);
                    ui.state = UI_STATE_STATUS;
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
                    bt_hid_host_le_forget_all();
                    ui.state = UI_STATE_STATUS;
                } else {
                    ui.state = UI_STATE_MAIN_MENU;
                }
            }
            break;
    }
}

static void check_inactivity(void) {
    if (ui.state == UI_STATE_STATUS)   return;
    if (ui.state == UI_STATE_PAIR_NEW) return;  /* own timing */
    if (time_us_64() - ui.last_input_us < UI_INACTIVITY_US) return;
    ui.state          = UI_STATE_STATUS;
    ui.menu_cursor    = 0;
    ui.list_cursor    = 0;
    ui.info_cursor    = 0;
    ui.confirm_cursor = 0;
    ui.dirty          = true;
}

/* PAIR_NEW timing.  Returns true if a redraw is warranted. */
static bool pair_new_tick(void) {
    uint64_t now = time_us_64();
    if (ui.pair_succeeded) {
        if (now >= ui.pair_splash_until_us) { ui.state = UI_STATE_STATUS; ui.dirty = true; }
        return false;
    }
    if (now >= ui.pair_deadline_us) {
        close_pairing_window();
        ui.state = UI_STATE_MAIN_MENU;
        ui.dirty = true;
        return false;
    }
    uint8_t secs = (uint8_t)((ui.pair_deadline_us - now) / 1000000ull);
    uint8_t pos  = bounce_pos(now);   /* ping-pong dot cell */
    bool changed = (secs != ui.last_pair_secs) || (pos != ui.last_pair_spin);
    ui.last_pair_secs = secs;
    ui.last_pair_spin = pos;
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
    while (buttons_poll(&bev)) handle_button_event(&bev);

    check_inactivity();

    bool pair_redraw = false;
    if (ui.state == UI_STATE_PAIR_NEW) pair_redraw = pair_new_tick();

    if (ui.state == UI_STATE_STATUS) {
        uint32_t active_hash = hash_active_table();
        /* Animation phase forces a redraw on a sub-second cadence so the
         * BT-icon blink (2 Hz while the radio comes up) and the name
         * marquee (1 Hz when a name overflows) keep moving even when no
         * BT state changed. */
        uint64_t now = time_us_64();
        const bt_active_entry_t *active = bt_events_active_table();
        uint32_t anim_phase = 0;
        if (!ui.radio_up)
            anim_phase ^= (uint32_t)((now / 500000ull) & 1ull) | 0x10u;
        if (status_has_long_name(active))
            anim_phase ^= ((uint32_t)(now / 1000000ull) & 0xffffu) << 1;

        if (!ui.dirty &&
            state->active_output      == ui.last_active_output &&
            bt_events_active_count()  == ui.last_active_count &&
            bt_events_bonded_count()  == ui.last_bonded_count &&
            active_hash               == ui.last_active_hash &&
            anim_phase                == ui.last_anim_phase) {
            return;
        }
        render_status(state->active_output);
        ui.last_active_output = state->active_output;
        ui.last_active_count  = bt_events_active_count();
        ui.last_bonded_count  = bt_events_bonded_count();
        ui.last_active_hash   = active_hash;
        ui.last_anim_phase    = anim_phase;
        ui.dirty              = false;
        return;
    }

    /* DEVICE_LIST / DEVICE_INFO are bond-driven: refetch the snapshot
     * and redraw if it changed (connect/disconnect flips a dot; a bond
     * added/removed changes the list). */
    bool needs_redraw = ui.dirty || pair_redraw;
    if (ui.state == UI_STATE_DEVICE_LIST || ui.state == UI_STATE_DEVICE_INFO) {
        ui_fetch_bonds();
        uint32_t h = fnv1a(ui.bonds, sizeof(bt_bond_info_t) * ui.bond_count)
                   ^ (uint32_t)((uint32_t)ui.bond_count << 1);
        if (h != ui.last_bonds_hash) { ui.last_bonds_hash = h; needs_redraw = true; }

        /* Marquee tick: if any visible bond name overflows, redraw at
         * 1 Hz so the scroll advances. */
        if (ui.state == UI_STATE_DEVICE_LIST) {
            bool any_long = false;
            for (int i = 0; i < ui.bond_count; i++)
                if ((int)strlen(ui.bonds[i].name) > UI_NAME_COLS) { any_long = true; break; }
            if (any_long) {
                uint32_t sec = (uint32_t)(time_us_64() / 1000000ull);
                if (sec != ui.last_anim_phase) { ui.last_anim_phase = sec; needs_redraw = true; }
            }
        }
    }
    if (!needs_redraw) return;

    switch (ui.state) {
        case UI_STATE_MAIN_MENU:          render_main_menu();          break;
        case UI_STATE_DEVICE_LIST:        render_device_list();        break;
        case UI_STATE_DEVICE_INFO:        render_device_info();        break;
        case UI_STATE_PAIR_NEW:           render_pair_new();           break;
        case UI_STATE_CONFIRM_FORGET:     render_confirm_forget();     break;
        case UI_STATE_CONFIRM_FORGET_ALL: render_confirm_forget_all(); break;
        case UI_STATE_ABOUT:              render_about();              break;
        default:                          render_status(state->active_output); break;
    }
    ui.dirty = false;
}

#endif /* DH_OLED_UI */
