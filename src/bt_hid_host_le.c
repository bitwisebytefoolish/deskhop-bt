#include "bt/btstack_config.h"

#ifdef DH_BT_HID_HOST_KBD

/* BLE HID-over-GATT Profile (HOGP) host — peer to bt_hid_host.c.  See
 * src/include/bt_hid_host_le.h for the architectural rationale and #29
 * for the use-case motivation (8BitDo Retro and other BLE-only keyboards).
 *
 * Initial implementation tried boot-mode HOGP (manual GATT discovery of
 * the BOOT_KEYBOARD_INPUT_REPORT characteristic).  The 8BitDo Retro
 * silently rejected the PROTOCOL_MODE = BOOT write — connection reached
 * "READY" but no notifications ever arrived, and the link dropped after
 * a few seconds.  Modern BLE keyboards routinely lack boot-mode support
 * (BIOS hosts are essentially extinct).
 *
 * Switched to BTstack's hids_client helper, which handles report-mode
 * HOGP end-to-end: discovers HID service(s), reads each report's
 * REPORT_REFERENCE descriptor to determine report ID + type, subscribes
 * to all INPUT reports via CCCD, and emits a single
 * GATTSERVICE_SUBEVENT_HID_REPORT event per incoming report with the
 * raw bytes.  Boot-mode is still supported by passing
 * HID_PROTOCOL_MODE_BOOT, but we default to REPORT mode.
 *
 * Report data is converted to deskhop's 8-byte boot-keyboard format
 * (modifier + reserved + 6 keycodes) by walking the report with
 * BTstack's btstack_hid_parser and extracting keyboard-page (0x07)
 * usages.  We feed THAT into process_keyboard_report — which, with
 * iface->protocol = HID_PROTOCOL_BOOT (set in setup.c), takes the
 * _extract_kbd_boot path and copies the 8 bytes straight through.
 * So both transports (Classic boot, BLE report) converge at the same
 * deskhop entry point without per-transport conditionals downstream.
 *
 * Reference: pico-sdk/lib/btstack/example/hog_host_demo.c. */

#include "bluetooth.h"
#include "bluetooth_gatt.h"
#include "hci.h"
#include "l2cap.h"
#include "gap.h"
#include "btstack_event.h"
#include "btstack_util.h"
#include "btstack_hid.h"
#include "btstack_hid_parser.h"
#include "ble/sm.h"
#include "ble/gatt_client.h"
#include "ble/att_db.h"
#include "ble/le_device_db.h"
#include "ble/gatt-service/hids_client.h"
#include "ad_parser.h"
#include "btstack_tlv.h"

#include <stdio.h>
#include <string.h>

#include "pico/time.h"     /* time_us_64 — used in bt_events publish sites */
#include "bt_hid_host_le.h"
#include "bt_events.h"

/* ---- Device-name harvest cache --------------------------------------
 * Some BLE peripherals include their friendly name in the Complete /
 * Shortened Local Name AD records of their UNDIRECTED advertisements.
 * Directed adv (ADV_DIRECT_IND) carries no payload, so the name is
 * unavailable during cold-boot reconnect.  Cache names by address from
 * any undirected adv we see; when the peer connects, publish the name
 * along with the BT_EVT_DEVICE_CONNECTED.  Phase-2/3 work will add a
 * GATT Device Name (UUID 0x2A00) fallback for devices that never
 * advertise undirected (purely directed-mode peers).
 *
 * Size: 8 slots × ~32 B = ~256 B.  Cache is per-session — names
 * persist across reconnects within the same boot but not across
 * reboots.  That's fine: once a peer adv-es undirected once (which it
 * will if put back into pairing mode), the cache repopulates.
 *
 * Build-gated on DH_OLED_UI to keep the cache out of builds that don't
 * need it — it's only consumed by the UI. */
#ifdef DH_OLED_UI
#define LE_NAME_CACHE_CAP 8
static struct {
    bd_addr_t addr;
    char      name[BT_EVT_NAME_MAX];
    bool      in_use;
} le_name_cache[LE_NAME_CACHE_CAP];

static void le_name_cache_put(const bd_addr_t addr, const char *name) {
    /* Replace existing entry first if the addr matches. */
    for (int i = 0; i < LE_NAME_CACHE_CAP; i++) {
        if (le_name_cache[i].in_use && memcmp(le_name_cache[i].addr, addr, 6) == 0) {
            strncpy(le_name_cache[i].name, name, BT_EVT_NAME_MAX - 1);
            le_name_cache[i].name[BT_EVT_NAME_MAX - 1] = '\0';
            return;
        }
    }
    /* Otherwise insert into the first free slot, or evict slot 0 if
     * full (the queue isn't LRU because peers tend to be stable —
     * a flat-evict policy is good enough at this size). */
    int slot = -1;
    for (int i = 0; i < LE_NAME_CACHE_CAP; i++) {
        if (!le_name_cache[i].in_use) { slot = i; break; }
    }
    if (slot < 0) slot = 0;
    memcpy(le_name_cache[slot].addr, addr, 6);
    strncpy(le_name_cache[slot].name, name, BT_EVT_NAME_MAX - 1);
    le_name_cache[slot].name[BT_EVT_NAME_MAX - 1] = '\0';
    le_name_cache[slot].in_use = true;
}

static const char *le_name_cache_get(const bd_addr_t addr) {
    for (int i = 0; i < LE_NAME_CACHE_CAP; i++) {
        if (le_name_cache[i].in_use && memcmp(le_name_cache[i].addr, addr, 6) == 0) {
            return le_name_cache[i].name;
        }
    }
    return "";
}

/* Scan an LE adv payload for AD type 0x09 (Complete Local Name) or
 * 0x08 (Shortened Local Name).  If found, copy up to BT_EVT_NAME_MAX-1
 * bytes into out and return true.  The peripheral advertising name is
 * typically printable ASCII; we copy verbatim and let the OLED font's
 * 0x20..0x7E range handle display. */
static bool le_extract_local_name(const uint8_t *ad_data, uint8_t ad_len, char *out) {
    int i = 0;
    while (i + 1 < ad_len) {
        uint8_t len = ad_data[i];
        if (len == 0) break;
        if (i + 1 + len > ad_len) break;
        uint8_t type = ad_data[i + 1];
        if (type == 0x09 /* COMPLETE_LOCAL_NAME */ ||
            type == 0x08 /* SHORTENED_LOCAL_NAME */) {
            uint8_t name_len = len - 1;
            if (name_len >= BT_EVT_NAME_MAX) name_len = BT_EVT_NAME_MAX - 1;
            memcpy(out, &ad_data[i + 2], name_len);
            out[name_len] = '\0';
            return true;
        }
        i += 1 + len;
    }
    return false;
}

/* Classify a HID report descriptor by its top-level application usage.
 * Walks the descriptor items (respecting item sizes so we never match
 * a data byte by accident), tracks the current Usage Page, and returns
 * on the first Usage that sits on the Generic Desktop page (0x01):
 *   Usage 0x02 = Mouse, 0x06 = Keyboard, 0x07 = Keypad.
 * Anything else (or a descriptor we can't make sense of) → UNKNOWN,
 * which the UI renders with the generic device glyph.
 *
 * HID item prefix byte: bits[1:0]=size code, bits[3:2]=type
 * (0=Main,1=Global,2=Local), bits[7:4]=tag.  Size code 3 means 4
 * data bytes; 0/1/2 mean that many bytes.  0xFE = long item. */
static bt_evt_kind_t le_classify_hid_descriptor(const uint8_t *desc, uint16_t len) {
    uint16_t usage_page = 0;
    uint16_t i = 0;
    while (i < len) {
        uint8_t prefix = desc[i++];
        if (prefix == 0xFE) {              /* long item */
            if (i >= len) break;
            uint8_t data_size = desc[i++];
            i += 1 + data_size;            /* skip long tag + data */
            continue;
        }
        uint8_t size_code = prefix & 0x03;
        uint8_t data_len   = (size_code == 3) ? 4 : size_code;
        uint8_t type       = (prefix >> 2) & 0x03;
        uint8_t tag        = (prefix >> 4) & 0x0F;

        uint32_t data = 0;
        for (uint8_t b = 0; b < data_len && (i + b) < len; b++)
            data |= (uint32_t)desc[i + b] << (8 * b);

        if (type == 1 && tag == 0x0) {            /* Global: Usage Page */
            usage_page = (uint16_t)data;
        } else if (type == 2 && tag == 0x0) {     /* Local: Usage */
            if (usage_page == 0x01) {             /* Generic Desktop */
                if (data == 0x02) return BT_KIND_MOUSE;
                if (data == 0x06) return BT_KIND_KEYBOARD;
                if (data == 0x07) return BT_KIND_KEYPAD;
            }
        }
        i += data_len;
    }
    return BT_KIND_UNKNOWN;
}
#endif /* DH_OLED_UI */

/* ---- Shared state -------------------------------------------------- */

static bt_hid_state_t *g_bt;

/* ---- App state machine -------------------------------------------- */

typedef enum {
    LE_W4_WORKING,                /* wait for HCI_STATE_WORKING */
    LE_W4_HID_DEVICE_FOUND,       /* scanning, waiting for HID adv */
    LE_W4_CONNECTED,              /* gap_connect issued, waiting for LE_CONNECTION_COMPLETE */
    LE_W4_ENCRYPTED,              /* connection up; waiting for pairing or re-encryption */
    LE_W4_HIDS_CONNECTED,         /* hids_client running its discovery */
    LE_READY,                     /* HID input reports flowing */
    LE_W4_TIMEOUT_THEN_SCAN,
    LE_W4_TIMEOUT_THEN_RECONNECT,
} le_app_state_t;

static le_app_state_t le_state = LE_W4_WORKING;

/* Remote BLE device we're connecting to. */
typedef struct {
    bd_addr_t      addr;
    bd_addr_type_t addr_type;
} le_device_addr_t;

static le_device_addr_t le_remote;
static hci_con_handle_t le_connection_handle = HCI_CON_HANDLE_INVALID;

/* ---- Opt-in pairing gate (#22 Phase 3 fix) ---------------------------
 * Pairing is CLOSED by default: we only accept a NEW (unbonded) device
 * while the user has explicitly opened a pairing window via the LCD
 * "Pair new" flow.  This is what makes "Forget" stick — without it, a
 * forgotten device that's still powered on just silently re-pairs the
 * instant it advertises.
 *
 * Bonded devices are unaffected: they reconnect (re-encrypt from the
 * stored LTK) regardless of this flag.  The gate only blocks fresh
 * pairings.
 *
 * Auto-closes after a hard timeout as a safety net in case the UI ever
 * fails to close it explicitly. */
static bool     le_pairing_open       = false;
static uint64_t le_pairing_open_until = 0;
#define LE_PAIRING_WINDOW_US (65ull * 1000000ull)  /* > UI's 60 s window */

/* Always-on auto-pairing.  DEFAULT TRUE so a headless / lite build
 * (no LCD UI) behaves like 1.0.0: any HID device that advertises while
 * a slot is free gets paired.  The LCD UI disables this at boot — but
 * ONLY when it confirms a panel is actually present (see setup.c).  So:
 *   - lite build / no panel  → auto_pair stays true  → auto-pair
 *   - OLED build + panel     → auto_pair set false    → opt-in pairing
 * Pairing is permitted when (le_auto_pair || le_pairing_open). */
static bool le_auto_pair = true;

void bt_hid_host_le_set_auto_pair(bool enable) {
    le_auto_pair = enable;
    printf("[ble] auto-pair %s\n", enable ? "ON (headless/lite)" : "off (opt-in via UI)");
}

void bt_hid_host_le_set_pairing_open(bool open) {
    le_pairing_open = open;
    le_pairing_open_until = open ? (time_us_64() + LE_PAIRING_WINDOW_US) : 0;
    printf("[ble] pairing window %s\n", open ? "OPEN" : "closed");
}

/* Pairing currently permitted for a NEW (unbonded) device? */
static inline bool le_pairing_permitted(void) {
    return le_auto_pair || le_pairing_open;
}

/* True if `addr` is already in our bond DB, i.e. a reconnect rather than
 * a new pairing.  Resolved-identity address types are inherently bonded
 * (the controller only resolves RPAs for peers whose IRK we hold). */
static bool le_addr_is_bonded(const uint8_t *addr, uint8_t addr_type) {
    if (addr_type == BD_ADDR_TYPE_LE_PUBLIC_IDENTITY ||
        addr_type == BD_ADDR_TYPE_LE_RANDOM_IDENTITY)
        return true;
    int maxn = le_device_db_max_count();
    for (int i = 0; i < maxn; i++) {
        int       t = (int)BD_ADDR_TYPE_UNKNOWN;
        bd_addr_t db;
        sm_key_t  irk;
        le_device_db_info(i, &t, db, irk);
        if (t == (int)BD_ADDR_TYPE_UNKNOWN) continue;
        if (memcmp(db, addr, 6) == 0) return true;
    }
    return false;
}

/* hids_client state.  cid is the per-connection HID client ID; descriptor
 * storage holds the parsed REPORT_MAP from each connected peripheral —
 * **shared across all hids_clients** (see hids_client_descriptor_storage_
 * get_available_space() in BTstack).  Each entry is reserved at
 * SERVICE_CONNECTED time, so with 4 simultaneous devices we need
 * enough space for 4 worst-case descriptors.
 *
 * Typical sizes: simple mouse ~50–100 B, basic keyboard ~150 B, modern
 * mechanical keyboard with media keys / multiple report IDs ~250–400 B,
 * gaming keyboards with extras 400–500 B.  Sized at 2 KB to comfortably
 * fit 4 medium-to-large descriptors (#9 #32: the 512 B size caused the
 * 3rd device's descriptor to be truncated → btstack_hid_parser produced
 * no fields → device "paired" but no input flowed).
 *
 * RAM cost: +1.5 KB; well within the 512 KB pico2_w budget. */
#define LE_HID_DESCRIPTOR_STORAGE_LEN  2048
static uint8_t  le_hid_descriptor_storage[LE_HID_DESCRIPTOR_STORAGE_LEN];
static uint16_t le_hids_cid;

/* Connection-establishment + reconnect timer. */
static btstack_timer_source_t le_connection_timer;

/* Packet-callback registrations.  Must outlive registration (file-scope OK). */
static btstack_packet_callback_registration_t le_hci_cb;
static btstack_packet_callback_registration_t le_sm_cb;

/* Bond persistence: device address/type stored under a tag so we can
 * prefer a direct reconnect on boot rather than re-scanning. */
#define TLV_TAG_HOGD ((((uint32_t)'H') << 24) | (((uint32_t)'O') << 16) | (((uint32_t)'G') << 8) | 'D')
static const btstack_tlv_t *le_tlv_impl;
static void                *le_tlv_ctx;

#ifdef DH_OLED_UI
/* ---- Persistent bonded-device name table (#22 Phase 3) ---------------
 * Stored in the BTstack TLV flash bank under a custom tag so device
 * friendly-names survive a reboot.  Without this the name cache is
 * in-memory only: on cold boot a bonded peer reconnecting via directed
 * adv (which carries no name in its payload) would show only its
 * address tail.  Loading this table at HCI_STATE_WORKING seeds the
 * in-memory cache so reconnects render names immediately.
 *
 * Written ONLY on a new pairing (SM_EVENT_IDENTITY_CREATED) and on
 * forget — never on routine reconnect — so flash wear stays
 * negligible (a TLV store is a log-append; rare writes are fine).
 *
 * The whole table is one TLV blob; capacity 16 mirrors the bond DB's
 * useful range.  Keyed by the address we connect with, which for the
 * static-random / public-identity peripherals this targets equals
 * their identity address.  Gated on DH_OLED_UI since names are only
 * consumed by the UI. */
#define TLV_TAG_BNAM ((((uint32_t)'B')<<24)|(((uint32_t)'N')<<16)|(((uint32_t)'A')<<8)|'M')
#define LE_BNAM_MAX 16
typedef struct __attribute__((packed)) {
    uint8_t addr[6];
    char    name[BT_EVT_NAME_MAX];
} le_bnam_entry_t;
typedef struct __attribute__((packed)) {
    uint8_t         count;
    le_bnam_entry_t e[LE_BNAM_MAX];
} le_bnam_table_t;
static le_bnam_table_t le_bnam;

static void le_bnam_save(void) {
    if (!le_tlv_impl) return;
    le_tlv_impl->store_tag(le_tlv_ctx, TLV_TAG_BNAM,
                           (const uint8_t *)&le_bnam, sizeof(le_bnam));
}

static void le_bnam_load(void) {
    if (!le_tlv_impl) return;
    int n = le_tlv_impl->get_tag(le_tlv_ctx, TLV_TAG_BNAM,
                                 (uint8_t *)&le_bnam, sizeof(le_bnam));
    if (n != (int)sizeof(le_bnam) || le_bnam.count > LE_BNAM_MAX) {
        memset(&le_bnam, 0, sizeof(le_bnam));
        return;
    }
    /* Seed the in-memory cache so reconnects render names right away. */
    for (int i = 0; i < le_bnam.count; i++)
        le_name_cache_put(le_bnam.e[i].addr, le_bnam.e[i].name);
}

static void le_bnam_put(const uint8_t *addr, const char *name) {
    if (!name || !name[0]) return;
    for (int i = 0; i < le_bnam.count; i++) {
        if (memcmp(le_bnam.e[i].addr, addr, 6) == 0) {
            strncpy(le_bnam.e[i].name, name, BT_EVT_NAME_MAX - 1);
            le_bnam.e[i].name[BT_EVT_NAME_MAX - 1] = '\0';
            le_bnam_save();
            return;
        }
    }
    if (le_bnam.count >= LE_BNAM_MAX) return;  /* table full — skip */
    memcpy(le_bnam.e[le_bnam.count].addr, addr, 6);
    strncpy(le_bnam.e[le_bnam.count].name, name, BT_EVT_NAME_MAX - 1);
    le_bnam.e[le_bnam.count].name[BT_EVT_NAME_MAX - 1] = '\0';
    le_bnam.count++;
    le_bnam_save();
}

static void le_bnam_remove(const uint8_t *addr) {
    for (int i = 0; i < le_bnam.count; i++) {
        if (memcmp(le_bnam.e[i].addr, addr, 6) == 0) {
            if (i != le_bnam.count - 1)
                le_bnam.e[i] = le_bnam.e[le_bnam.count - 1];
            le_bnam.count--;
            le_bnam_save();
            return;
        }
    }
}

static void le_bnam_clear(void) {
    memset(&le_bnam, 0, sizeof(le_bnam));
    le_bnam_save();
}
#endif /* DH_OLED_UI */

/* Currently-connected device table.  Populated by GATTSERVICE_SUBEVENT_
 * HID_SERVICE_CONNECTED and drained by GATTSERVICE_SUBEVENT_HID_SERVICE_
 * DISCONNECTED.  Filters scan results to skip advertisers we already
 * have an open hids_client connection to.  Without this filter, our
 * scan-resume-after-pair loop would re-discover the device we just
 * paired and issue another gap_connect to it, which BTstack handles by
 * tearing down the existing connection — observed in #9 v1 hardware
 * test as "pairing the 3rd device breaks the 1st and 2nd".
 *
 * Sized 1:1 with the hids_client pool — no point tracking more than
 * we can host simultaneously.                                       */
typedef struct {
    bd_addr_t       addr;
    uint16_t        hids_cid;
    hci_con_handle_t con_handle;        /* for gap_disconnect on forget */
    uint16_t        name_value_handle;  /* GAP Device Name char handle, 0=unknown/done */
} le_active_entry_t;
static le_active_entry_t le_active[MAX_NR_HIDS_CLIENTS];
static int               le_num_active;

/* ---- Switch 2 Pro gamepad (#24) -----------------------------------
 * The Switch 2 Pro Controller is BLE with a PROPRIETARY GATT service
 * (not HID-over-GATT): it advertises with Nintendo's company ID 0x057E
 * and pushes input via notifications on a vendor characteristic.  So
 * it bypasses the hids_client path entirely and uses a raw gatt_client
 * notification subscription instead.  Reference: the switch2bridge
 * project + #24. */
#define TLV_TAG_GPAD ((((uint32_t)'G')<<24)|(((uint32_t)'P')<<16)|(((uint32_t)'A')<<8)|'D')
#define NINTENDO_COMPANY_ID 0x057E

/* Input report characteristic, 128-bit UUID 7492866c-ec3e-4619-8258-
 * 32755ffcc0f9, big-endian (most-significant byte first). */
static const uint8_t le_gp_input_uuid128[16] = {
    0x74,0x92,0x86,0x6c, 0xec,0x3e, 0x46,0x19,
    0x82,0x58, 0x32,0x75,0x5f,0xfc,0xc0,0xf9};

static bool                         le_remote_is_gamepad = false;
static gatt_client_characteristic_t le_gp_char;
static gatt_client_notification_t   le_gp_notification;
static bool                         le_gp_have_char  = false;
static bool                         le_gp_subscribing = false;
static bool                         le_gp_paired_retry = false;
static bd_addr_t                    le_gp_bonded_addr = {0};
static bool                         le_gp_bonded_valid = false;

static bool le_addr_is_active(const bd_addr_t addr) {
    for (int i = 0; i < le_num_active; i++) {
        if (memcmp(le_active[i].addr, addr, sizeof(bd_addr_t)) == 0)
            return true;
    }
    return false;
}

static void le_add_active(const bd_addr_t addr, uint16_t cid, hci_con_handle_t handle) {
    if (le_num_active >= MAX_NR_HIDS_CLIENTS) {
        printf("[ble] active-device table full (%d slots) — refusing to add %02x:%02x:%02x:%02x:%02x:%02x\n",
               MAX_NR_HIDS_CLIENTS,
               addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
        return;
    }
    bd_addr_copy(le_active[le_num_active].addr, addr);
    le_active[le_num_active].hids_cid          = cid;
    le_active[le_num_active].con_handle        = handle;
    le_active[le_num_active].name_value_handle = 0;
    le_num_active++;
    printf("[ble] active devices: %d/%d (added cid=0x%04x)\n",
           le_num_active, MAX_NR_HIDS_CLIENTS, cid);
}

static void le_remove_active_by_cid(uint16_t cid) {
    for (int i = 0; i < le_num_active; i++) {
        if (le_active[i].hids_cid != cid)
            continue;
        /* Compact: move last entry into the freed slot. */
        if (i != le_num_active - 1)
            le_active[i] = le_active[le_num_active - 1];
        le_num_active--;
        printf("[ble] active devices: %d/%d (removed cid=0x%04x)\n",
               le_num_active, MAX_NR_HIDS_CLIENTS, cid);
        return;
    }
}

/* Drop an active entry by ACL connection handle, publishing a
 * DISCONNECTED event to the UI on the way out.  Called from
 * HCI_EVENT_DISCONNECTION_COMPLETE so a forget that yanks the ACL out
 * from under hids_client still clears the active table + status screen
 * even when no HID_SERVICE_DISCONNECTED subevent arrives.  No-op (and no
 * duplicate event) if the slot was already freed by that subevent. */
static void le_remove_active_by_handle(hci_con_handle_t handle) {
    for (int i = 0; i < le_num_active; i++) {
        if (le_active[i].con_handle != handle)
            continue;
#ifdef DH_OLED_UI
        bt_event_t e = {
            .type      = BT_EVT_DEVICE_DISCONNECTED,
            .transport = BT_TRANSPORT_LE,
            .cid       = le_active[i].hids_cid,
            .ts_us     = time_us_64(),
        };
        bt_evt_addr_from_bd(&e.addr, le_active[i].addr);
        bt_events_publish(&e);
#endif
        if (i != le_num_active - 1)
            le_active[i] = le_active[le_num_active - 1];
        le_num_active--;
        printf("[ble] active devices: %d/%d (removed handle=0x%04x on ACL drop)\n",
               le_num_active, MAX_NR_HIDS_CLIENTS, handle);
        return;
    }
}

/* ---- forward declarations ----------------------------------------- */

static void le_start_scan(void);
static void le_start_connect(void);
static void le_connect_to_remote(void);
static void le_connection_timeout_cb(btstack_timer_source_t *ts);
static void le_reconnect_timeout_cb(btstack_timer_source_t *ts);
static void le_handle_outgoing_connection_error(void);
static void le_kick_hids_client(void);
static void le_kick_gamepad_gatt(void);
static void le_gp_subscribe(void);
static void le_gamepad_after_security(void);
static void le_gamepad_gatt_handler(uint8_t packet_type, uint16_t channel,
                                    uint8_t *packet, uint16_t size);
static void le_gamepad_notify_handler(uint8_t packet_type, uint16_t channel,
                                      uint8_t *packet, uint16_t size);

static void le_packet_handler(uint8_t packet_type, uint16_t channel,
                              uint8_t *packet, uint16_t size);
static void le_sm_packet_handler(uint8_t packet_type, uint16_t channel,
                                 uint8_t *packet, uint16_t size);
static void le_hids_client_event_handler(uint8_t packet_type, uint16_t channel,
                                         uint8_t *packet, uint16_t size);
static void le_handle_input_report(uint16_t hids_cid, uint8_t service_index,
                                   const uint8_t *report, uint16_t report_len);
#ifdef DH_OLED_UI
static void le_gap_name_handler(uint8_t packet_type, uint16_t channel,
                                uint8_t *packet, uint16_t size);
#endif

/* ---- helpers ------------------------------------------------------- */

static bool le_adv_contains_hid_service(const uint8_t *packet) {
    const uint8_t *ad_data = gap_event_advertising_report_get_data(packet);
    uint8_t        ad_len  = gap_event_advertising_report_get_data_length(packet);
    return ad_data_contains_uuid16(ad_len, ad_data,
                                   ORG_BLUETOOTH_SERVICE_HUMAN_INTERFACE_DEVICE);
}

static void le_start_scan(void) {
    printf("[ble] scanning for HID peripherals (UUID 0x1812)\n");
    le_state = LE_W4_HID_DEVICE_FOUND;
    gap_set_scan_parameters(0, 48, 48);
    gap_start_scan();
}

static void le_connect_to_remote(void) {
    btstack_run_loop_set_timer(&le_connection_timer, 10000);
    btstack_run_loop_set_timer_handler(&le_connection_timer, &le_connection_timeout_cb);
    btstack_run_loop_add_timer(&le_connection_timer);
    le_state = LE_W4_CONNECTED;
    printf("[ble] gap_connect %02x:%02x:%02x:%02x:%02x:%02x (addr_type=%u)\n",
           le_remote.addr[0], le_remote.addr[1], le_remote.addr[2],
           le_remote.addr[3], le_remote.addr[4], le_remote.addr[5],
           le_remote.addr_type);
    gap_connect(le_remote.addr, le_remote.addr_type);
}

static void le_start_connect(void) {
    /* Initialize the TLV handle so SM_EVENT_PAIRING_COMPLETE can still
     * persist the most-recently-bonded device's address (used for the
     * "favourite-device" hint, even though we no longer drive a
     * fast-reconnect from it).  Keep the store path; drop the load path. */
    btstack_tlv_get_instance(&le_tlv_impl, &le_tlv_ctx);

    /* Load the persisted gamepad identity address (#24) so a bonded
     * controller reconnecting via directed/identity adv (no manufacturer
     * data) is still recognized as a gamepad. */
    if (le_tlv_impl) {
        bd_addr_t gp;
        int n = le_tlv_impl->get_tag(le_tlv_ctx, TLV_TAG_GPAD, (uint8_t *)gp, sizeof(gp));
        if (n == (int)sizeof(gp)) {
            bd_addr_copy(le_gp_bonded_addr, gp);
            le_gp_bonded_valid = true;
            printf("[ble][gp] persisted gamepad %02x:%02x:%02x:%02x:%02x:%02x\n",
                   gp[0], gp[1], gp[2], gp[3], gp[4], gp[5]);
        }
    }

#ifdef DH_OLED_UI
    /* Load persisted device names now that the TLV is available, seeding
     * the in-memory name cache so reconnecting bonded peers render their
     * names instead of an address tail. */
    le_bnam_load();
#endif

    /* The original design here cold-connected directly to the last-bonded
     * device via gap_connect(stored_address) without first seeing it
     * advertise.  Observed on every cold boot with the multi-device
     * setup:
     *
     *   gap_connect <last paired> → identity resolves → re-encryption
     *   succeeds → hids_client_connect FAILS with status=0x1f
     *
     * That 0x1f is gatt_client_att_status_to_error_code() swallowing a
     * real ATT error during HID service discovery.  Hypothesis: by
     * connecting before the peripheral has advertised, we hit it before
     * its GATT server has finished restoring state from low-power, and
     * the discovery query gets a NOT_FOUND / TIMEOUT response.  Once
     * we let the scan path drive (peer advertises → we connect on its
     * own schedule), the same hids_client_connect succeeds first try.
     *
     * Now that ENABLE_LE_PRIVACY_ADDRESS_RESOLUTION is on and we accept
     * ADV_DIRECT_IND, bonded peers reliably re-find us via their own
     * advertising — so the TLV fast-reconnect provides no value and
     * costs one wasted ~5 s connect-then-fail cycle at every boot.
     * Drop it; always scan. */
    le_start_scan();
}

static void le_connection_timeout_cb(btstack_timer_source_t *ts) {
    (void)ts;
    printf("[ble] connection timeout, cancelling and rescanning\n");
    gap_connect_cancel();
    le_start_scan();
}

static void le_reconnect_timeout_cb(btstack_timer_source_t *ts) {
    (void)ts;
    switch (le_state) {
        case LE_W4_TIMEOUT_THEN_RECONNECT: le_connect_to_remote(); break;
        case LE_W4_TIMEOUT_THEN_SCAN:      le_start_scan();        break;
        default: break;
    }
}

static void le_handle_outgoing_connection_error(void) {
    printf("[ble] outgoing connection error — disconnect + rescan\n");
    if (le_connection_handle != HCI_CON_HANDLE_INVALID)
        gap_disconnect(le_connection_handle);
    le_start_scan();
}

/* Kick off hids_client discovery once the encrypted link is up.  Called
 * from SM_EVENT_PAIRING_COMPLETE / SM_EVENT_REENCRYPTION_COMPLETE on
 * success.  hids_client_connect allocates a new hids_client_t from the
 * pool (MAX_NR_HIDS_CLIENTS — sized for multi-device in #9), so each
 * concurrent BLE connection gets its own per-device state inside
 * BTstack.  Our state machine tracks only the "currently connecting"
 * device; once HIDS_SERVICE_CONNECTED arrives we resume scanning for
 * additional peripherals.                                            */
static void le_kick_hids_client(void) {
    /* Idempotent against multiple SM events during a single connect
     * (RESOLVING_SUCCEEDED then REENCRYPTION_COMPLETE both fire) —
     * once we're in HIDS_CONNECTED or READY for this connection,
     * don't re-issue.  The state is per-current-connect, not
     * per-deskhop-lifetime; scan will be restarted after the
     * service-connected event regardless. */
    if (le_state == LE_W4_HIDS_CONNECTED || le_state == LE_READY)
        return;
    le_state = LE_W4_HIDS_CONNECTED;
    uint8_t status = hids_client_connect(le_connection_handle,
                                         &le_hids_client_event_handler,
                                         HID_PROTOCOL_MODE_REPORT,
                                         &le_hids_cid);
    if (status != ERROR_CODE_SUCCESS) {
        printf("[ble] hids_client_connect FAIL status=0x%02x\n", status);
        le_handle_outgoing_connection_error();
        return;
    }
    printf("[ble] hids_client_connect started (cid=0x%04x, report-mode)\n",
           le_hids_cid);
}

/* ---- Switch 2 Pro gamepad GATT path (#24) -------------------------- */

static inline int8_t le_gp_clamp8(int v) {
    if (v > 127)  return 127;
    if (v < -128) return -128;
    return (int8_t)v;
}

static bool le_adv_is_nintendo_gamepad(const uint8_t *packet) {
    const uint8_t *ad  = gap_event_advertising_report_get_data(packet);
    uint8_t        len = gap_event_advertising_report_get_data_length(packet);
    int i = 0;
    while (i + 1 < len) {
        uint8_t l = ad[i];
        if (l == 0) break;
        if (i + 1 + l > len) break;
        uint8_t type = ad[i + 1];
        if (type == 0xFF && l >= 3) {   /* Manufacturer Specific Data */
            uint16_t company = (uint16_t)ad[i + 2] | ((uint16_t)ad[i + 3] << 8);
            if (company == NINTENDO_COMPANY_ID) return true;
        }
        i += 1 + l;
    }
    return false;
}

static bool le_addr_is_bonded_gamepad(const bd_addr_t addr) {
    return le_gp_bonded_valid && memcmp(addr, le_gp_bonded_addr, 6) == 0;
}

/* Begin the gamepad bring-up: discover the vendor input characteristic
 * by its 128-bit UUID across the whole handle range (we don't know the
 * service UUID, so search by characteristic). */
static void le_kick_gamepad_gatt(void) {
    le_gp_have_char    = false;
    le_gp_subscribing  = false;
    le_gp_paired_retry = false;
    uint8_t st = gatt_client_discover_characteristics_for_handle_range_by_uuid128(
        &le_gamepad_gatt_handler, le_connection_handle, 0x0001, 0xffff,
        le_gp_input_uuid128);
    printf("[ble][gp] discover input characteristic: status=0x%02x\n", st);
    if (st != ERROR_CODE_SUCCESS)
        gap_disconnect(le_connection_handle);
}

/* Register a notification listener and enable notifications (CCCD).
 * If the CCCD write is rejected for insufficient encryption, the query-
 * complete handler triggers pairing and we retry from
 * le_gamepad_after_security(). */
static void le_gp_subscribe(void) {
    le_gp_subscribing = true;
    gatt_client_listen_for_characteristic_value_updates(
        &le_gp_notification, &le_gamepad_notify_handler,
        le_connection_handle, &le_gp_char);
    uint8_t st = gatt_client_write_client_characteristic_configuration(
        &le_gamepad_gatt_handler, le_connection_handle, &le_gp_char,
        GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NOTIFICATION);
    printf("[ble][gp] enable notifications: write status=0x%02x\n", st);
}

/* Re-issue the CCCD write after we paired in response to an
 * insufficient-encryption rejection. */
static void le_gamepad_after_security(void) {
    if (!le_gp_have_char) { le_kick_gamepad_gatt(); return; }
    le_gp_subscribe();
}

/* Notifications are flowing — register the device as active, mark
 * connected, and persist its identity address for reconnect detection. */
static void le_gamepad_on_ready(void) {
    le_add_active(le_remote.addr, 0xFFFF /* no hids_cid */, le_connection_handle);
    le_state = LE_READY;
    if (g_bt && g_bt->keyboard_connected)
        *g_bt->keyboard_connected = true;

    bd_addr_copy(le_gp_bonded_addr, le_remote.addr);
    le_gp_bonded_valid = true;
    if (le_tlv_impl)
        le_tlv_impl->store_tag(le_tlv_ctx, TLV_TAG_GPAD,
                               (const uint8_t *)le_gp_bonded_addr, 6);

#ifdef DH_OLED_UI
    {
        bt_event_t e = {.type = BT_EVT_DEVICE_CONNECTED, .transport = BT_TRANSPORT_LE,
                        .kind = BT_KIND_UNKNOWN, .cid = 0xFFFF, .ts_us = time_us_64()};
        bt_evt_addr_from_bd(&e.addr, le_remote.addr);
        strncpy(e.name, le_name_cache_get(le_remote.addr), BT_EVT_NAME_MAX - 1);
        bt_events_publish(&e);
        bt_events_set_bonded_count((uint8_t)le_device_db_count());
    }
#endif
    printf("[ble][gp] Switch2 Pro ready — input notifications active\n");

    /* Resume scanning for additional peripherals if slots remain. */
    if (le_num_active < MAX_NR_HIDS_CLIENTS)
        le_start_scan();
    else
        le_state = LE_READY;
}

static void le_gamepad_gatt_handler(uint8_t packet_type, uint16_t channel,
                                    uint8_t *packet, uint16_t size) {
    (void)channel; (void)size;
    if (packet_type != HCI_EVENT_PACKET) return;

    switch (hci_event_packet_get_type(packet)) {
        case GATT_EVENT_CHARACTERISTIC_QUERY_RESULT:
            gatt_event_characteristic_query_result_get_characteristic(packet, &le_gp_char);
            le_gp_have_char = true;
            printf("[ble][gp] found input char value_handle=0x%04x\n",
                   le_gp_char.value_handle);
            break;

        case GATT_EVENT_QUERY_COMPLETE: {
            uint8_t att = gatt_event_query_complete_get_att_status(packet);
            if (!le_gp_subscribing) {
                /* Discovery finished. */
                if (!le_gp_have_char) {
                    printf("[ble][gp] input characteristic not found (att=0x%02x) — disconnecting\n", att);
                    gap_disconnect(le_connection_handle);
                    break;
                }
                le_gp_subscribe();
            } else {
                /* CCCD write finished. */
                if (att == ATT_ERROR_SUCCESS) {
                    le_gamepad_on_ready();
                } else if ((att == ATT_ERROR_INSUFFICIENT_ENCRYPTION ||
                            att == ATT_ERROR_INSUFFICIENT_AUTHENTICATION) &&
                           !le_gp_paired_retry) {
                    printf("[ble][gp] CCCD needs encryption (att=0x%02x) — pairing then retrying\n", att);
                    le_gp_paired_retry = true;
                    le_gp_subscribing  = false;   /* will re-subscribe after security */
                    sm_request_pairing(le_connection_handle);
                } else {
                    printf("[ble][gp] enable notifications failed att=0x%02x — disconnecting\n", att);
                    gap_disconnect(le_connection_handle);
                }
            }
            break;
        }

        default: break;
    }
}

/* Parse a Switch 2 Pro input notification into the packed gamepad_report_t
 * byte layout and forward it.  Field offsets are from the switch2bridge
 * reverse-engineering: buttons in bytes 2-4, two 12-bit sticks in bytes
 * 5-10. */
static void le_gamepad_notify_handler(uint8_t packet_type, uint16_t channel,
                                      uint8_t *packet, uint16_t size) {
    (void)channel; (void)size;
    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != GATT_EVENT_NOTIFICATION) return;

    const uint8_t *d   = gatt_event_notification_get_value(packet);
    uint16_t       len = gatt_event_notification_get_value_length(packet);
    if (len < 11) return;

    uint8_t  b2 = d[2], b3 = d[3], b4 = d[4];
    uint16_t btn = 0;
    if (b2 & 0x02) btn |= 1u << 0;   /* A  */
    if (b2 & 0x01) btn |= 1u << 1;   /* B  */
    if (b2 & 0x08) btn |= 1u << 2;   /* X  */
    if (b2 & 0x04) btn |= 1u << 3;   /* Y  */
    if (b3 & 0x10) btn |= 1u << 4;   /* L  */
    if (b2 & 0x10) btn |= 1u << 5;   /* R  */
    if (b3 & 0x20) btn |= 1u << 6;   /* ZL */
    if (b2 & 0x20) btn |= 1u << 7;   /* ZR */
    if (b2 & 0x40) btn |= 1u << 8;   /* +  */
    if (b3 & 0x40) btn |= 1u << 9;   /* -  */
    if (b3 & 0x80) btn |= 1u << 10;  /* LS */
    if (b2 & 0x80) btn |= 1u << 11;  /* RS */
    if (b4 & 0x01) btn |= 1u << 12;  /* Home    */
    if (b4 & 0x10) btn |= 1u << 13;  /* Capture */
    if (b4 & 0x08) btn |= 1u << 14;  /* GL */
    if (b4 & 0x04) btn |= 1u << 15;  /* GR */

    bool up = b3 & 0x08, down = b3 & 0x01, left = b3 & 0x04, right = b3 & 0x02;
    uint8_t hat = 8;   /* centered */
    if (up && !down)        hat = (right && !left) ? 1 : (left && !right) ? 7 : 0;
    else if (down && !up)   hat = (right && !left) ? 3 : (left && !right) ? 5 : 4;
    else                    hat = (right && !left) ? 2 : (left && !right) ? 6 : 8;

    int lx = (int)(d[5] | ((d[6] & 0x0F) << 8)) - 2048;
    int ly = (int)(((d[6] & 0xF0) >> 4) | (d[7] << 4)) - 2048;
    int rx = (int)(d[8] | ((d[9] & 0x0F) << 8)) - 2048;
    int ry = (int)(((d[9] & 0xF0) >> 4) | (d[10] << 4)) - 2048;

    /* Packed gamepad_report_t byte layout: lx, ly, rx, ry, hat, btn_lo, btn_hi.
     * Y axes inverted to the HID convention (up = negative). */
    uint8_t rep[7];
    rep[0] = (uint8_t)le_gp_clamp8(lx >> 4);
    rep[1] = (uint8_t)le_gp_clamp8(-(ly >> 4));
    rep[2] = (uint8_t)le_gp_clamp8(rx >> 4);
    rep[3] = (uint8_t)le_gp_clamp8(-(ry >> 4));
    rep[4] = hat;
    rep[5] = (uint8_t)(btn & 0xFF);
    rep[6] = (uint8_t)(btn >> 8);

    if (g_bt && g_bt->process_gamepad_report)
        g_bt->process_gamepad_report(rep, sizeof(rep));
}

/* ---- HID input report → deskhop's boot-keyboard / boot-mouse pipeline ----
 *
 * Per-report type dispatch: a HOGP peripheral can advertise both keyboard
 * and mouse functionality in a single HID service (composite devices), or
 * pure keyboard-only, or pure mouse-only.  Rather than detect the device
 * type at pairing time, walk THIS report's fields and decide based on
 * which usage pages we see:
 *
 *   Usage Page 0x07 (Keyboard/Keypad)         → build boot-keyboard report
 *   Usage Page 0x01 (Generic Desktop): X/Y/Wheel + Usage Page 0x09 (Button) → boot-mouse
 *
 * Both reports can be built from the SAME notification if the descriptor
 * mixes them (rare but legal).  Otherwise only one is forwarded.  A
 * keyboard-only device sends nothing to the mouse pipeline; a mouse-only
 * device sends nothing to the keyboard pipeline.  No per-device routing
 * table needed.                                                          */
static void le_handle_input_report(uint16_t hids_cid, uint8_t service_index,
                                   const uint8_t *report, uint16_t report_len) {
    if (report_len < 1)
        return;

    /* Boot-keyboard layout (8 bytes): [mod][rsvd][k1..k6] */
    uint8_t kbd_report[8]  = {0};
    bool    kbd_seen       = false;
    int     kbd_key_count  = 0;

    /* Boot-mouse layout (5 bytes, matches TinyUSB hid_mouse_report_t):
     * [buttons][x][y][wheel][pan].  Wheel and pan are signed; x/y are
     * relative deltas.  We accumulate signed values from descriptor
     * fields and clamp to int8 at the end. */
    uint8_t mouse_report[5] = {0};
    bool    mouse_seen      = false;
    int32_t mouse_dx = 0, mouse_dy = 0, mouse_wheel = 0, mouse_pan = 0;

    /* CRITICAL: parse the report against THIS device's descriptor, not
     * the globally most-recently-paired device's.  Each connected BLE
     * peripheral has its own descriptor stored in hids_client's storage
     * keyed by cid.  Using le_hids_cid (which was the last cid set by
     * hids_client_connect) would parse every incoming report against the
     * wrong descriptor — observed in #9 multi-device test as "mouse stops
     * working when keyboard connects" (mouse reports parsed with kbd
     * descriptor produced garbage). */
    btstack_hid_parser_t parser;
    btstack_hid_parser_init(
        &parser,
        hids_client_descriptor_storage_get_descriptor_data(hids_cid, service_index),
        hids_client_descriptor_storage_get_descriptor_len(hids_cid, service_index),
        HID_REPORT_TYPE_INPUT, report, report_len);

    while (btstack_hid_parser_has_more(&parser)) {
        uint16_t usage_page, usage;
        int32_t  value;
        btstack_hid_parser_get_field(&parser, &usage_page, &usage, &value);

        switch (usage_page) {
            case 0x07: /* Keyboard / Keypad */
                kbd_seen = true;
                if (value == 0) break;
                if (usage >= 0xE0 && usage <= 0xE7) {
                    /* Modifier: pack into bit (usage - 0xE0). */
                    kbd_report[0] |= (uint8_t)(1u << (usage - 0xE0));
                } else if (kbd_key_count < 6) {
                    kbd_report[2 + kbd_key_count++] = (uint8_t)usage;
                }
                break;

            case 0x01: /* Generic Desktop — pointer X/Y/Wheel */
                switch (usage) {
                    case 0x30: mouse_dx    = value; mouse_seen = true; break;
                    case 0x31: mouse_dy    = value; mouse_seen = true; break;
                    case 0x38: mouse_wheel = value; mouse_seen = true; break;
                    default: break;
                }
                break;

            case 0x09: /* Button page — mouse buttons */
                if (usage >= 1 && usage <= 8 && value != 0) {
                    mouse_report[0] |= (uint8_t)(1u << (usage - 1));
                    mouse_seen = true;
                }
                break;

            case 0x0C: /* Consumer page — AC Pan (horizontal scroll) is 0x0238 */
                if (usage == 0x0238) {
                    mouse_pan  = value;
                    mouse_seen = true;
                }
                break;

            default:
                break;
        }
    }

    if (kbd_seen && g_bt && g_bt->process_report && g_bt->kbd_iface) {
        /* Even an all-zero report is meaningful (key-up notification) so
         * we forward whenever the descriptor had keyboard fields, not just
         * when keys are pressed. */
        g_bt->process_report(kbd_report, sizeof(kbd_report),
                             g_bt->kbd_itf, g_bt->kbd_iface);
    }

    if (mouse_seen && g_bt && g_bt->process_mouse_report && g_bt->mouse_iface) {
        /* Clamp dx/dy/wheel/pan to int8 range — boot mouse fields are
         * signed bytes.  Values from the descriptor can be wider when
         * the peripheral declares a 12- or 16-bit logical range; saturate
         * rather than overflow. */
        if (mouse_dx > 127)       mouse_dx = 127;
        else if (mouse_dx < -127) mouse_dx = -127;
        if (mouse_dy > 127)       mouse_dy = 127;
        else if (mouse_dy < -127) mouse_dy = -127;
        if (mouse_wheel > 127)       mouse_wheel = 127;
        else if (mouse_wheel < -127) mouse_wheel = -127;
        if (mouse_pan > 127)         mouse_pan = 127;
        else if (mouse_pan < -127)   mouse_pan = -127;
        mouse_report[1] = (uint8_t)(int8_t)mouse_dx;
        mouse_report[2] = (uint8_t)(int8_t)mouse_dy;
        mouse_report[3] = (uint8_t)(int8_t)mouse_wheel;
        mouse_report[4] = (uint8_t)(int8_t)mouse_pan;
        g_bt->process_mouse_report(mouse_report, sizeof(mouse_report),
                                   g_bt->mouse_itf, g_bt->mouse_iface);
    }
}

#ifdef DH_OLED_UI
/* ---- GAP Device Name (0x2A00) reader ---------------------------------
 * Some peripherals (e.g. the 8BitDo keyboard) don't put their friendly
 * name in their connectable advertising payload — only the Logitech-
 * style ones (MX Master) do.  The GAP Device Name characteristic is
 * mandatory on every BLE peripheral, so after the HID service is up we
 * read it over GATT to fill in any name the adv didn't give us.  Only
 * issued when the name cache has nothing for the device, so it costs a
 * single extra read per never-before-named device and nothing on
 * reconnect (the persisted name is already cached). */
static int le_active_idx_for_handle(hci_con_handle_t h) {
    for (int i = 0; i < le_num_active; i++)
        if (le_active[i].con_handle == h) return i;
    return -1;
}

static void le_gap_name_handler(uint8_t packet_type, uint16_t channel,
                                uint8_t *packet, uint16_t size) {
    (void)channel; (void)size;
    if (packet_type != HCI_EVENT_PACKET) return;

    switch (hci_event_packet_get_type(packet)) {
        /* Step 1: discovery turned up the GAP Device Name characteristic.
         * Stash its value handle on the matching active entry; we issue
         * the read once discovery completes (below). */
        case GATT_EVENT_CHARACTERISTIC_QUERY_RESULT: {
            hci_con_handle_t h = gatt_event_characteristic_query_result_get_handle(packet);
            int idx = le_active_idx_for_handle(h);
            if (idx < 0) break;
            gatt_client_characteristic_t ch;
            gatt_event_characteristic_query_result_get_characteristic(packet, &ch);
            le_active[idx].name_value_handle = ch.value_handle;
            break;
        }

        /* Step 2: discovery finished.  If we found the characteristic,
         * read its full value by handle (a plain Read Request returns up
         * to ATT_MTU-1 bytes — no Read-By-Type 19-byte cap).  Clear the
         * stashed handle first so this read's own QUERY_COMPLETE doesn't
         * re-issue the read. */
        case GATT_EVENT_QUERY_COMPLETE: {
            hci_con_handle_t h = gatt_event_query_complete_get_handle(packet);
            int idx = le_active_idx_for_handle(h);
            if (idx < 0) break;
            uint16_t vh = le_active[idx].name_value_handle;
            if (vh == 0) break;
            le_active[idx].name_value_handle = 0;
            gatt_client_read_value_of_characteristic_using_value_handle(
                &le_gap_name_handler, h, vh);
            break;
        }

        /* Step 3: the name value came back. */
        case GATT_EVENT_CHARACTERISTIC_VALUE_QUERY_RESULT: {
            hci_con_handle_t h   = gatt_event_characteristic_value_query_result_get_handle(packet);
            const uint8_t   *val = gatt_event_characteristic_value_query_result_get_value(packet);
            uint16_t         len = gatt_event_characteristic_value_query_result_get_value_length(packet);
            if (len == 0) break;
            if (len >= BT_EVT_NAME_MAX) len = BT_EVT_NAME_MAX - 1;

            char name[BT_EVT_NAME_MAX];
            memcpy(name, val, len);
            name[len] = '\0';

            int idx = le_active_idx_for_handle(h);
            if (idx < 0) break;
            le_name_cache_put(le_active[idx].addr, name);
            le_bnam_put(le_active[idx].addr, name);   /* persist for next boot */
            bt_event_t e = {
                .type      = BT_EVT_DEVICE_NAME_RESOLVED,
                .transport = BT_TRANSPORT_LE,
                .ts_us     = time_us_64(),
            };
            bt_evt_addr_from_bd(&e.addr, le_active[idx].addr);
            strncpy(e.name, name, BT_EVT_NAME_MAX - 1);
            e.name[BT_EVT_NAME_MAX - 1] = '\0';
            bt_events_publish(&e);
            printf("[ble] GAP device name resolved: \"%s\"\n", name);
            break;
        }

        default: break;
    }
}
#endif /* DH_OLED_UI */

/* ---- hids_client event handler ----------------------------------- */

static void le_hids_client_event_handler(uint8_t packet_type, uint16_t channel,
                                         uint8_t *packet, uint16_t size) {
    (void)packet_type;
    (void)channel;
    (void)size;

    if (hci_event_packet_get_type(packet) != HCI_EVENT_GATTSERVICE_META)
        return;

    uint8_t subevent = hci_event_gattservice_meta_get_subevent_code(packet);
    switch (subevent) {
        case GATTSERVICE_SUBEVENT_HID_SERVICE_CONNECTED: {
            uint8_t status = gattservice_subevent_hid_service_connected_get_status(packet);
            if (status != ERROR_CODE_SUCCESS) {
                printf("[ble] HID service client connect FAIL status=0x%02x\n", status);
                le_handle_outgoing_connection_error();
                break;
            }
            uint16_t connected_cid = gattservice_subevent_hid_service_connected_get_hids_cid(packet);
            uint8_t  num_services  = gattservice_subevent_hid_service_connected_get_num_instances(packet);
            printf("[ble] HID service client CONNECTED (cid=0x%04x, %u services) — READY for input\n",
                   connected_cid, num_services);
            /* Log the actual descriptor size received per service.  If it's
             * 0 (or noticeably less than the device's real descriptor), the
             * shared descriptor buffer was likely full — see comment on
             * LE_HID_DESCRIPTOR_STORAGE_LEN.  An undersized descriptor
             * passes through SERVICE_CONNECTED success but causes
             * btstack_hid_parser to extract no fields → no input flows. */
            for (uint8_t i = 0; i < num_services; i++) {
                uint16_t desc_len =
                    hids_client_descriptor_storage_get_descriptor_len(connected_cid, i);
                printf("[ble]   svc=%u descriptor_len=%u bytes\n", i, desc_len);
            }
            if (g_bt && g_bt->keyboard_connected)
                *g_bt->keyboard_connected = true;
            /* Track this device in the active list so subsequent scans
             * don't try to reconnect to it (which would tear down this
             * very connection). */
            le_add_active(le_remote.addr, connected_cid, le_connection_handle);
#ifdef DH_OLED_UI
            /* Tell the UI the connection is up.  Attach the cached
             * friendly name (from undirected adv earlier this session)
             * if we have one — otherwise the UI renders the address
             * tail until the name is resolved by a future GATT read
             * (Phase 2/3). */
            {
                /* Classify the device from its HID report descriptor
                 * (service 0) so the UI can show a keyboard / mouse /
                 * keypad icon.  Falls back to UNKNOWN → generic glyph
                 * if the descriptor is empty or unrecognised. */
                bt_evt_kind_t kind = BT_KIND_UNKNOWN;
                const uint8_t *desc =
                    hids_client_descriptor_storage_get_descriptor_data(connected_cid, 0);
                uint16_t desc_len =
                    hids_client_descriptor_storage_get_descriptor_len(connected_cid, 0);
                if (desc && desc_len)
                    kind = le_classify_hid_descriptor(desc, desc_len);

                bt_event_t e = {
                    .type      = BT_EVT_DEVICE_CONNECTED,
                    .transport = BT_TRANSPORT_LE,
                    .kind      = kind,
                    .cid       = connected_cid,
                    .ts_us     = time_us_64(),
                };
                bt_evt_addr_from_bd(&e.addr, le_remote.addr);
                strncpy(e.name, le_name_cache_get(le_remote.addr), BT_EVT_NAME_MAX - 1);
                bt_events_publish(&e);
                printf("[ble] device kind=%s\n",
                       kind == BT_KIND_KEYBOARD ? "keyboard" :
                       kind == BT_KIND_MOUSE    ? "mouse" :
                       kind == BT_KIND_KEYPAD   ? "keypad" : "unknown");

                /* If the adv didn't give us a name (e.g. 8BitDo only
                 * exposes it via GATT, not in the adv payload), read the
                 * mandatory GAP Device Name characteristic over GATT now
                 * that hids_client has finished its discovery and the
                 * connection is idle.  The result patches the name into
                 * the cache + persists it + fires NAME_RESOLVED so the
                 * UI updates. */
                if (le_name_cache_get(le_remote.addr)[0] == '\0') {
                    /* Discover the GAP Device Name characteristic to get
                     * its value handle, then read the full value by
                     * handle (see le_gap_name_handler).  A read-by-UUID
                     * (Read-By-Type) caps the returned value at ATT_MTU-4
                     * = 19 bytes on the common 23-byte MTU, which clipped
                     * "8BitDo Retro Keyboard" to "...Keyboa".  A plain
                     * Read Request by handle returns up to ATT_MTU-1. */
                    gatt_client_discover_characteristics_for_handle_range_by_uuid16(
                        &le_gap_name_handler, le_connection_handle,
                        0x0001, 0xffff,
                        ORG_BLUETOOTH_CHARACTERISTIC_GAP_DEVICE_NAME);
                }
            }
#endif
            /* Persist this device address for fast direct-reconnect on
             * next boot.  Stores the MOST RECENTLY connected device.
             * Multi-device extension TODO: maintain an array of bonded
             * addresses (covered by the LCD UI in #22). */
            if (le_tlv_impl) {
                le_tlv_impl->store_tag(le_tlv_ctx, TLV_TAG_HOGD,
                                       (const uint8_t *)&le_remote,
                                       sizeof(le_remote));
            }
            /* Resume scanning so the user can pair additional peripherals
             * in the same session, but only if there are slots left.
             * Each new pair gets its own hids_client_t from the pool. */
            if (le_num_active < MAX_NR_HIDS_CLIENTS) {
                printf("[ble] resuming scan for additional peripherals\n");
                le_start_scan();
            } else {
                printf("[ble] all %d hids_client slots in use — not resuming scan\n",
                       MAX_NR_HIDS_CLIENTS);
                le_state = LE_READY;
            }
            break;
        }

        case GATTSERVICE_SUBEVENT_HID_SERVICE_DISCONNECTED: {
            uint16_t disc_cid = gattservice_subevent_hid_service_disconnected_get_hids_cid(packet);
            printf("[ble] HID service client disconnected (cid=0x%04x)\n", disc_cid);
#ifdef DH_OLED_UI
            /* Publish DISCONNECTED with the address recovered from the
             * active table — must be done BEFORE le_remove_active_by_cid
             * frees the slot, otherwise we lose the addr → cid binding. */
            for (int i = 0; i < le_num_active; i++) {
                if (le_active[i].hids_cid != disc_cid) continue;
                bt_event_t e = {
                    .type      = BT_EVT_DEVICE_DISCONNECTED,
                    .transport = BT_TRANSPORT_LE,
                    .cid       = disc_cid,
                    .ts_us     = time_us_64(),
                };
                bt_evt_addr_from_bd(&e.addr, le_active[i].addr);
                bt_events_publish(&e);
                break;
            }
#endif
            le_remove_active_by_cid(disc_cid);
            /* If we'd capped scanning because slots were full, restart
             * it now that one freed up. */
            if (le_state != LE_W4_HID_DEVICE_FOUND && le_state != LE_W4_CONNECTED)
                le_start_scan();
            break;
        }

        case GATTSERVICE_SUBEVENT_HID_REPORT:
            le_handle_input_report(
                gattservice_subevent_hid_report_get_hids_cid(packet),
                gattservice_subevent_hid_report_get_service_index(packet),
                gattservice_subevent_hid_report_get_report(packet),
                gattservice_subevent_hid_report_get_report_len(packet));
            break;

        default:
            printf("[ble] unhandled gattservice subevent=0x%02x\n", subevent);
            break;
    }
}

/* ---- SM packet handler: pairing exchange ------------------------- */

static void le_sm_packet_handler(uint8_t packet_type, uint16_t channel,
                                 uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;

    if (packet_type != HCI_EVENT_PACKET)
        return;

    bool security_up = false;

    switch (hci_event_packet_get_type(packet)) {
        case SM_EVENT_JUST_WORKS_REQUEST:
            /* Opt-in pairing safety net.  A just-works request only
             * arrives for a NEW pairing (bonded reconnects re-encrypt
             * silently).  If the pairing window isn't open, decline —
             * this stops a forgotten-but-still-advertising device from
             * silently re-pairing.  The pre-connect adv filter should
             * already have skipped it, but declining here is the
             * authoritative gate. */
            if (!le_pairing_permitted()) {
                printf("[ble] just-works request but pairing CLOSED — declining\n");
                sm_bonding_decline(sm_event_just_works_request_get_handle(packet));
                break;
            }
            printf("[ble] SSP just-works request — auto-confirm\n");
            sm_just_works_confirm(sm_event_just_works_request_get_handle(packet));
            break;

        case SM_EVENT_NUMERIC_COMPARISON_REQUEST:
            printf("[ble] numeric comparison request — auto-confirm\n");
            sm_numeric_comparison_confirm(
                sm_event_numeric_comparison_request_get_handle(packet));
            break;

        case SM_EVENT_IDENTITY_RESOLVING_STARTED:
            printf("[ble] identity resolving started (peer using RPA)\n");
            break;

        case SM_EVENT_IDENTITY_RESOLVING_FAILED:
            printf("[ble] identity resolving FAILED (no bond — first-pair path)\n");
            break;

        case SM_EVENT_IDENTITY_RESOLVING_SUCCEEDED:
            printf("[ble] identity resolving SUCCEEDED (matched stored bond)\n");
            break;

        case SM_EVENT_REENCRYPTION_STARTED:
            printf("[ble] re-encryption started\n");
            break;

        case SM_EVENT_PAIRING_STARTED:
            /* First-pair path: the SMP exchange has begun. */
            printf("[ble] pairing started\n");
            break;

        case SM_EVENT_IDENTITY_CREATED:
            /* BTstack just stored an identity entry (IRK + identity
             * address) for this peer in le_device_db.  This is what
             * lets future bonded-reconnects resolve the peer's
             * Resolvable Private Address back to the same device —
             * UNLESS the peer rotates its IRK every session, in which
             * case each pair creates a fresh entry that's useless next
             * time.  The DB count growing without saturating useful
             * reconnects is the smoke signal for that vendor-side
             * behaviour.  See #34. */
            printf("[ble] identity created (peer is now bonded, DB now has %d/%d entries)\n",
                   le_device_db_count(), le_device_db_max_count());
#ifdef DH_OLED_UI
            /* New bond persisted to flash.  Refresh the UI's bonded
             * counter from the authoritative source (le_device_db_count)
             * — bt_events.c also increments locally on the
             * DEVICE_PAIRED event for snappiness, but le_device_db_count
             * is the truth in case the increment race-loses against
             * a near-simultaneous load. */
            bt_events_set_bonded_count((uint8_t)le_device_db_count());
            /* Persist the friendly name for this freshly-bonded peer so
             * it survives reboot.  le_name_cache holds the name harvested
             * from the undirected pairing adv we just connected through. */
            le_bnam_put(le_remote.addr, le_name_cache_get(le_remote.addr));
            {
                bt_event_t e = {
                    .type      = BT_EVT_DEVICE_PAIRED,
                    .transport = BT_TRANSPORT_LE,
                    .ts_us     = time_us_64(),
                };
                bt_evt_addr_from_bd(&e.addr, le_remote.addr);
                strncpy(e.name, le_name_cache_get(le_remote.addr), BT_EVT_NAME_MAX - 1);
                bt_events_publish(&e);
            }
#endif
            break;

        case SM_EVENT_REENCRYPTION_COMPLETE:
            printf("[ble] re-encryption complete — using stored bond\n");
            security_up = true;
            break;

        case SM_EVENT_PAIRING_COMPLETE: {
            uint8_t status = sm_event_pairing_complete_get_status(packet);
            switch (status) {
                case ERROR_CODE_SUCCESS:
                    printf("[ble] pairing complete: SUCCESS\n");
                    security_up = true;
                    break;
                case ERROR_CODE_CONNECTION_TIMEOUT:
                    printf("[ble] pairing FAIL: timeout\n");
                    break;
                case ERROR_CODE_REMOTE_USER_TERMINATED_CONNECTION:
                    printf("[ble] pairing FAIL: remote terminated\n");
                    break;
                case ERROR_CODE_AUTHENTICATION_FAILURE:
                    printf("[ble] pairing FAIL: authentication (reason=%u)\n",
                           sm_event_pairing_complete_get_reason(packet));
                    break;
                default:
                    printf("[ble] pairing FAIL: status=0x%02x\n", status);
                    break;
            }
            break;
        }

        default:
            printf("[ble] unhandled SM event type=0x%02x\n",
                   hci_event_packet_get_type(packet));
            break;
    }

    if (security_up) {
        if (le_remote_is_gamepad) {
            /* Gamepad paired in response to an insufficient-encryption
             * CCCD rejection — retry the subscribe now that the link is
             * encrypted. */
            le_gamepad_after_security();
        } else {
            /* Encrypted link is up.  Now kick off the HID service client
             * — its GATT discovery is allowed to run on the now-encrypted
             * link, and the 8BitDo (and similar BLE-only keyboards) will
             * happily respond to characteristic + descriptor queries that
             * would otherwise have been rejected. */
            le_kick_hids_client();
        }
    }
}

/* ---- HCI / GAP packet handler ----------------------------------- */

static void le_packet_handler(uint8_t packet_type, uint16_t channel,
                              uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;

    if (packet_type != HCI_EVENT_PACKET)
        return;

    uint8_t event_type = hci_event_packet_get_type(packet);
    switch (event_type) {
        case BTSTACK_EVENT_STATE:
            if (btstack_event_state_get_state(packet) != HCI_STATE_WORKING)
                break;
            if (le_state != LE_W4_WORKING)
                break;
            /* Boot-time bond DB status.  Each entry is one peer's IRK
             * + identity address — used to resolve their rotating RPAs
             * back to a stable identity on reconnect.  Devices that
             * regenerate their IRK each session (some BLE mice do this
             * as a privacy feature — vendor-controlled) accumulate
             * stale entries here.  Watch this count grow over multiple
             * mouse-pair sessions; if it approaches MAX, BTstack will
             * start evicting "good" entries belonging to other devices,
             * cascading into mass re-pair.  See #34 for the diagnosis. */
            printf("[ble] HCI_STATE_WORKING (LE device DB has %d/%d bonded entries)\n",
                   le_device_db_count(), le_device_db_max_count());
#ifdef DH_OLED_UI
            /* UI bring-up: tell the event bus how many bonds we
             * inherited from flash, and emit a RADIO_UP marker so the
             * status screen can transition from "...waiting..." to
             * "scanning".  Done here rather than inside an event
             * because the bonded count is queryable directly via
             * le_device_db_count() and we don't want to fabricate
             * fake-pair events for already-persisted bonds. */
            bt_events_set_bonded_count((uint8_t)le_device_db_count());
            {
                bt_event_t e = {.type = BT_EVT_RADIO_UP, .transport = BT_TRANSPORT_LE,
                                .ts_us = time_us_64()};
                bt_events_publish(&e);
            }
#endif
            /* Populate the controller's LL resolving list from the bond DB.
             * Without this, the controller treats every incoming
             * Resolvable Private Address as a fresh unknown — which means
             * we can't see DIRECTED advertisements that a bonded
             * peripheral sends to our identity (peer puts our resolved
             * RPA in InitA; if the controller can't match it, the host
             * never gets the adv report).
             *
             * Symptom this fixes: cold boot with only the mouse powered
             * shows zero `HID adv from ...` events, because Logitech
             * (and most modern BLE) mice switch to DIRECTED adv toward
             * the bonded central after the first idle window.  Loading
             * the resolving list opens that channel.
             *
             * Safe to call repeatedly; BTstack iterates the bond DB and
             * pushes each (IRK, identity addr) pair to the controller. */
            gap_load_resolving_list_from_le_device_db();
            le_start_connect();
            break;

        case GAP_EVENT_ADVERTISING_REPORT: {
            if (le_state != LE_W4_HID_DEVICE_FOUND)
                break;
            /* Two acceptance paths for adv packets:
             *
             *  (1) UNDIRECTED adv (ADV_IND, ADV_SCAN_IND, ADV_NONCONN_IND)
             *      → must contain the HID service UUID (0x1812) in the AD
             *      payload, otherwise we connect to random non-HID devices.
             *
             *  (2) DIRECTED adv (ADV_DIRECT_IND) → accept unconditionally.
             *      A directed adv only goes out from a peer that already
             *      knows us (the bond exchanged identity info both ways);
             *      its 12-byte LL payload is `AdvA + InitA` with NO AD
             *      data, so the UUID filter would reject every one.  This
             *      is the path bonded HID mice take when they wake from
             *      sleep — high-duty-cycle directed adv for ~1.28 s
             *      targeting the bonded central, then back to sleep.
             *      Pre-fix symptom: mouse never reconnects after cold
             *      boot because every one of its 300+ HDC adv packets in
             *      that 1.28 s window hits the UUID filter and is
             *      silently dropped.
             *
             *  Also accept resolved-identity address types (PUBLIC_IDENTITY
             *  / RANDOM_IDENTITY) — those indicate the controller resolved
             *  an incoming RPA to a bonded peer via the LL resolving list,
             *  so it's definitely one of our HID peripherals even if the
             *  payload lacks UUIDs (e.g. directed adv to RPA). */
            uint8_t adv_event_type = gap_event_advertising_report_get_advertising_event_type(packet);
            uint8_t adv_addr_type  = gap_event_advertising_report_get_address_type(packet);
            bool is_directed  = (adv_event_type == 0x01);  /* ADV_DIRECT_IND */
            bool is_identity  = (adv_addr_type == BD_ADDR_TYPE_LE_PUBLIC_IDENTITY) ||
                                (adv_addr_type == BD_ADDR_TYPE_LE_RANDOM_IDENTITY);
            /* Switch 2 Pro (and other Nintendo controllers) advertise with
             * the Nintendo company ID in manufacturer data instead of the
             * HID service UUID — accept those too (#24). */
            bool is_gamepad   = le_adv_is_nintendo_gamepad(packet);
            if (!is_directed && !is_identity && !is_gamepad &&
                !le_adv_contains_hid_service(packet))
                break;
            bd_addr_t adv_addr;
            gap_event_advertising_report_get_address(packet, adv_addr);
            /* Already connected?  Skip — re-connecting to the same
             * peripheral while a connection is open causes BTstack to
             * tear down the existing connection (observed in #9 v1
             * hardware test: pairing the 3rd device disconnected the
             * 1st two). */
            if (le_addr_is_active(adv_addr))
                break;
            /* Opt-in pairing gate: only connect to an UNBONDED device
             * when a pairing window is open (the user picked "Pair new"
             * on the LCD).  Bonded devices reconnect regardless.  This
             * is what makes "Forget" stick — a forgotten device that's
             * still advertising won't silently re-pair. */
            if (!le_pairing_permitted() &&
                !le_addr_is_bonded(adv_addr,
                                   gap_event_advertising_report_get_address_type(packet))) {
                break;  /* unknown device, pairing closed → ignore */
            }
            /* Auto-close the pairing window if its safety timeout lapsed
             * (UI normally closes it explicitly; this is belt-and-braces). */
            if (le_pairing_open && time_us_64() > le_pairing_open_until)
                le_pairing_open = false;
            /* All hids_client slots in use?  Skip — no point connecting
             * if we can't host the HID service client. */
            if (le_num_active >= MAX_NR_HIDS_CLIENTS) {
                printf("[ble] %d/%d devices active — ignoring further HID advs until one disconnects\n",
                       le_num_active, MAX_NR_HIDS_CLIENTS);
                break;
            }
            gap_stop_scan();
            bd_addr_copy(le_remote.addr, adv_addr);
            le_remote.addr_type = gap_event_advertising_report_get_address_type(packet);
            /* Remember whether this peer is a gamepad: either it advertised
             * Nintendo manufacturer data, or it's our persisted bonded
             * gamepad reconnecting via directed/identity adv (no mfr data). */
            le_remote_is_gamepad = is_gamepad || le_addr_is_bonded_gamepad(adv_addr);
#ifdef DH_OLED_UI
            /* Harvest the peer's friendly name from any AD records in
             * this packet.  Directed adv has no payload (is_directed
             * implies the UUID-bypass path above), so this only
             * succeeds on undirected adv.  Cached by address so the
             * eventual BT_EVT_DEVICE_CONNECTED can attach the name. */
            if (!is_directed) {
                const uint8_t *ad_data = gap_event_advertising_report_get_data(packet);
                uint8_t        ad_len  = gap_event_advertising_report_get_data_length(packet);
                char           name[BT_EVT_NAME_MAX];
                if (le_extract_local_name(ad_data, ad_len, name) && name[0]) {
                    le_name_cache_put(adv_addr, name);
                }
            }
#endif
            printf("[ble] HID adv from %02x:%02x:%02x:%02x:%02x:%02x (addr_type=%u, adv_type=%s)\n",
                   le_remote.addr[0], le_remote.addr[1], le_remote.addr[2],
                   le_remote.addr[3], le_remote.addr[4], le_remote.addr[5],
                   le_remote.addr_type,
                   adv_event_type == 0x00 ? "ADV_IND" :
                   adv_event_type == 0x01 ? "ADV_DIRECT_IND" :
                   adv_event_type == 0x02 ? "ADV_SCAN_IND" :
                   adv_event_type == 0x03 ? "ADV_NONCONN_IND" :
                   adv_event_type == 0x04 ? "SCAN_RSP" : "UNKNOWN");
            le_connect_to_remote();
            break;
        }

        case HCI_EVENT_META_GAP:
            if (hci_event_gap_meta_get_subevent_code(packet) != GAP_SUBEVENT_LE_CONNECTION_COMPLETE)
                break;
            if (le_state != LE_W4_CONNECTED)
                break;
            btstack_run_loop_remove_timer(&le_connection_timer);
            le_connection_handle =
                gap_subevent_le_connection_complete_get_connection_handle(packet);
            /* Connection is up but not yet encrypted.  Wait for the SM
             * exchange (just-works pairing on first connect, or
             * re-encryption from stored LTK on subsequent connects) to
             * complete, then kick hids_client.  gatt_client_set_required_
             * security_level(LEVEL_2) from init triggers pairing
             * automatically if hids_client's GATT queries hit
             * insufficient-encryption errors, but in practice the
             * peripheral initiates security itself or BTstack handles
             * it as part of the bond-resume flow. */
            le_state = LE_W4_ENCRYPTED;
            if (le_remote_is_gamepad) {
                /* Gamepad: try the GATT subscribe WITHOUT bonding first;
                 * only pair if the CCCD write is rejected for insufficient
                 * encryption (handled in le_gamepad_gatt_handler).  Many
                 * controllers expose the input characteristic unencrypted. */
                printf("[ble][gp] LE connection complete (handle 0x%04x) — subscribing (no bond yet)\n",
                       le_connection_handle);
                le_kick_gamepad_gatt();
            } else {
                printf("[ble] LE connection complete (handle 0x%04x) — waiting for security\n",
                       le_connection_handle);
                sm_request_pairing(le_connection_handle);
            }
            break;

        case HCI_EVENT_DISCONNECTION_COMPLETE: {
            hci_con_handle_t h = hci_event_disconnection_complete_get_connection_handle(packet);
            /* Clear the active table + UI for whichever device dropped,
             * independent of the reconnect bookkeeping below.  This is
             * what makes a forget-while-connected actually vanish from
             * the status screen: gap_disconnect() drops the ACL and the
             * HID_SERVICE_DISCONNECTED subevent may never arrive. */
            le_remove_active_by_handle(h);

            if (le_connection_handle == HCI_CON_HANDLE_INVALID)
                break;
            if (h != le_connection_handle)
                break;
            printf("[ble] LE disconnected (state was %d) — back to scan/reconnect loop\n",
                   (int)le_state);
            le_connection_handle = HCI_CON_HANDLE_INVALID;
            if (g_bt && g_bt->keyboard_connected)
                *g_bt->keyboard_connected = false;
            /* Reset per-connection gamepad bring-up state. */
            le_remote_is_gamepad = false;
            le_gp_have_char      = false;
            le_gp_subscribing    = false;
            le_gp_paired_retry   = false;
            switch (le_state) {
                case LE_READY:
                    /* Reconnect to the same peer only if it's still
                     * bonded.  If we just forgot it (bond removed), a
                     * direct reconnect would connect → JUST_WORKS →
                     * decline (pairing closed) → disconnect → loop.
                     * Falling through to SCAN avoids that: the scan
                     * filter skips the now-unbonded device. */
                    if (le_addr_is_bonded(le_remote.addr, le_remote.addr_type))
                        le_state = LE_W4_TIMEOUT_THEN_RECONNECT;
                    else
                        le_state = LE_W4_TIMEOUT_THEN_SCAN;
                    break;
                default:
                    le_state = LE_W4_TIMEOUT_THEN_SCAN;
                    break;
            }
            btstack_run_loop_set_timer(&le_connection_timer, 100);
            btstack_run_loop_set_timer_handler(&le_connection_timer, &le_reconnect_timeout_cb);
            btstack_run_loop_add_timer(&le_connection_timer);
            break;
        }

        default: break;
    }
}

/* ---- Forget / bond removal (#22 Phase 3) ------------------------------
 *
 * Not gated on DH_OLED_UI — these are plain bond-DB operations.  Their
 * only caller today is the OLED UI, but exposing them unconditionally
 * (within DH_BT_HID_HOST_KBD) keeps them link-visible without extra
 * guards.  Each removes the LE device DB entry (TLV-backed, so the
 * removal persists), tears down any live connection to the device, and
 * clears the fast-reconnect hint + persisted name. */

/* Remove all LE device DB entries whose stored identity address matches
 * `addr`.  Returns the number removed. */
static int le_db_remove_by_addr(const uint8_t *addr) {
    int removed = 0;
    int maxn = le_device_db_max_count();
    for (int i = 0; i < maxn; i++) {
        int        type = (int)BD_ADDR_TYPE_UNKNOWN;
        bd_addr_t  db_addr;
        sm_key_t   irk;
        le_device_db_info(i, &type, db_addr, irk);
        if (type == (int)BD_ADDR_TYPE_UNKNOWN) continue;  /* empty slot */
        if (memcmp(db_addr, addr, 6) == 0) {
            le_device_db_remove(i);
            removed++;
        }
    }
    return removed;
}

void bt_hid_host_le_forget(const uint8_t *addr) {
    /* Tear down a live connection to this device, if any.  Crucially we
     * drop the ACL LINK (gap_disconnect), not just the HID client —
     * hids_client_disconnect leaves the encrypted ACL up, and the device
     * would re-attach over it with no re-pairing, so "forget" wouldn't
     * stick.  gap_disconnect forces a full disconnect; with the bond
     * removed and the pairing window closed, the device can't come back
     * until the user runs "Pair new".  The HID-service-disconnected
     * event from the ACL drop cleans up the active table + UI. */
    for (int i = 0; i < le_num_active; i++) {
        if (memcmp(le_active[i].addr, addr, 6) == 0) {
            hids_client_disconnect(le_active[i].hids_cid);
            if (le_active[i].con_handle != HCI_CON_HANDLE_INVALID)
                gap_disconnect(le_active[i].con_handle);
            break;
        }
    }

    int removed = le_db_remove_by_addr(addr);

    /* Drop the fast-reconnect hint if it points at this device. */
    if (le_tlv_impl) {
        le_device_addr_t hint;
        int n = le_tlv_impl->get_tag(le_tlv_ctx, TLV_TAG_HOGD,
                                     (uint8_t *)&hint, sizeof(hint));
        if (n == (int)sizeof(hint) && memcmp(hint.addr, addr, 6) == 0)
            le_tlv_impl->delete_tag(le_tlv_ctx, TLV_TAG_HOGD);
    }

#ifdef DH_OLED_UI
    le_bnam_remove(addr);
    bt_events_set_bonded_count((uint8_t)le_device_db_count());
#endif

    printf("[ble] forget %02x:%02x:%02x:%02x:%02x:%02x — removed %d DB entr%s, DB now %d\n",
           addr[0], addr[1], addr[2], addr[3], addr[4], addr[5],
           removed, removed == 1 ? "y" : "ies", le_device_db_count());
}

void bt_hid_host_le_forget_all(void) {
    /* Disconnect everything that's live — ACL drop, not just HID client
     * (see the rationale in bt_hid_host_le_forget). */
    for (int i = 0; i < le_num_active; i++) {
        hids_client_disconnect(le_active[i].hids_cid);
        if (le_active[i].con_handle != HCI_CON_HANDLE_INVALID)
            gap_disconnect(le_active[i].con_handle);
    }

    int maxn = le_device_db_max_count();
    int removed = 0;
    for (int i = 0; i < maxn; i++) {
        int        type = (int)BD_ADDR_TYPE_UNKNOWN;
        bd_addr_t  db_addr;
        sm_key_t   irk;
        le_device_db_info(i, &type, db_addr, irk);
        if (type == (int)BD_ADDR_TYPE_UNKNOWN) continue;
        le_device_db_remove(i);
        removed++;
    }

    if (le_tlv_impl)
        le_tlv_impl->delete_tag(le_tlv_ctx, TLV_TAG_HOGD);

#ifdef DH_OLED_UI
    le_bnam_clear();
    bt_events_set_bonded_count(0);
#endif

    printf("[ble] forget ALL — removed %d bond(s), DB now %d\n",
           removed, le_device_db_count());
}

int bt_hid_host_le_get_bonds(bt_bond_info_t *out, int max) {
    int count = 0;
    int maxn = le_device_db_max_count();
    for (int i = 0; i < maxn && count < max; i++) {
        int        type = (int)BD_ADDR_TYPE_UNKNOWN;
        bd_addr_t  db_addr;
        sm_key_t   irk;
        le_device_db_info(i, &type, db_addr, irk);
        if (type == (int)BD_ADDR_TYPE_UNKNOWN) continue;  /* empty slot */

        memcpy(out[count].addr, db_addr, 6);
        out[count].addr_type = (uint8_t)type;
        out[count].name[0]   = '\0';
#ifdef DH_OLED_UI
        /* Persisted friendly name (cache is seeded from the TLV name
         * table at boot, so this resolves for any bonded device). */
        const char *nm = le_name_cache_get(db_addr);
        if (nm[0]) {
            strncpy(out[count].name, nm, sizeof(out[count].name) - 1);
            out[count].name[sizeof(out[count].name) - 1] = '\0';
        }
#endif
        /* Connection status: is this identity address in the active
         * (connected) table?  For the static-random / public-identity
         * peripherals this targets, the connect address equals the
         * identity, so a direct compare works. */
        out[count].connected = false;
        for (int j = 0; j < le_num_active; j++) {
            if (memcmp(le_active[j].addr, db_addr, 6) == 0) {
                out[count].connected = true;
                break;
            }
        }
        count++;
    }
    return count;
}

/* ---- public init ------------------------------------------------- */

void bt_hid_host_le_init(bt_hid_state_t *bt_state) {
    g_bt = bt_state;

    /* Security Manager — just-works pairing with LE Secure Connections
     * required AND bonding.  Initial #29 implementation requested SC and
     * the 8BitDo timed out — but the actual cause of that timeout was a
     * separate bug (bare sm_request_pairing() that the 8BitDo ignored),
     * fixed later in #29 via gatt_client_set_required_security_level(
     * LEVEL_2) which triggers pairing organically through a GATT op.
     *
     * Re-enabling SC now per #36: modern BLE peripherals (post-2017)
     * tend to treat **legacy pairing as throwaway** and only persist
     * a bond when SC is negotiated.  Symptom: our previously-paired
     * mouse re-pairs every session ("identity resolving FAILED")
     * despite SM_EVENT_IDENTITY_CREATED firing successfully.  Same
     * mouse on macOS retains its bond, suggesting Apple's SC-required
     * pairing is what triggers persistent bonding peripheral-side.
     *
     * If this regresses the 8BitDo or any other previously-working
     * device, we have a fallback in mind: detect SC-pairing failure
     * and retry with legacy AuthReq.  For now, take the simpler path
     * and require SC for all LE pairs. */
    sm_init();
    sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    sm_set_authentication_requirements(SM_AUTHREQ_SECURE_CONNECTION | SM_AUTHREQ_BONDING);

    /* GATT client base + security policy: BTstack auto-triggers pairing
     * when an op needs higher security than the current connection has. */
    gatt_client_init();
    gatt_client_set_required_security_level(LEVEL_2);

    /* HID Service client.  Stores per-service report descriptors here;
     * we read them back during report parsing to drive btstack_hid_parser. */
    hids_client_init(le_hid_descriptor_storage, sizeof(le_hid_descriptor_storage));

    /* HCI / GAP events. */
    le_hci_cb.callback = &le_packet_handler;
    hci_add_event_handler(&le_hci_cb);

    /* SM events. */
    le_sm_cb.callback = &le_sm_packet_handler;
    sm_add_event_handler(&le_sm_cb);

    printf("[ble] handlers registered (hids_client, report-mode), awaiting HCI_STATE_WORKING\n");
}

#endif /* DH_BT_HID_HOST_KBD */
