/*
 * Button input implementation.  See src/include/buttons.h.
 *
 * Polled, not IRQ-driven.  buttons_task() is called once per UI frame
 * (~30 Hz) and samples each pin's level.  Because the 33 ms sample
 * period is far longer than a tactile switch's bounce (~1-15 ms),
 * every sample is taken after the contact has settled — so bounce is
 * filtered for free, with no edge-window debounce to tune.  This
 * replaced an IRQ + edge-window design whose single time-window had
 * to serve as both "reject bounce" and "minimum press duration",
 * two requirements that conflict (too short lets bounce through; too
 * long drops fast taps).
 *
 * Classification:
 *   - CLICK: emitted on RELEASE of a press shorter than LONG_PRESS_US.
 *   - LONG : emitted the instant the hold crosses LONG_PRESS_US (while
 *            still held) — gives immediate "you've held long enough"
 *            feedback; the subsequent release emits nothing.
 *
 * Memory: 3 × per-button state + 8-slot ring ≈ 150 B in .bss.
 */

#include "pico/stdlib.h"

#ifdef DH_OLED_UI

#include "hardware/gpio.h"
#include "buttons.h"
#include "pinout.h"

/* ---- Pin ↔ button mapping --------------------------------------------
 * Index by button_id_t.  Single source of truth: pinout.h's
 * UI_BUTTON_* defines.  Adding a 4th button (e.g. BACK on GP22)
 * is a 2-line change here and a new enumerator in buttons.h. */
static const uint8_t button_pins[BTN__COUNT] = {
    [BTN_UP]     = UI_BUTTON_UP,
    [BTN_DOWN]   = UI_BUTTON_DOWN,
    [BTN_SELECT] = UI_BUTTON_SELECT,
};

#define LONG_PRESS_US   500000   /* 500 ms threshold for click-vs-long */

/* ---- Per-button polled state -------------------------------------- */

typedef struct {
    bool     pressed;        /* debounced logical state (true = held down) */
    uint64_t press_us;       /* time the current hold started */
    bool     long_emitted;   /* a LONG event already fired for this hold */
} button_state_t;

static button_state_t bs[BTN__COUNT];

/* ---- Event ring ----------------------------------------------------- */

#define BTN_RING_CAP 8
static button_event_t ring[BTN_RING_CAP];
static uint8_t        ring_head;  /* next slot to write */
static uint8_t        ring_tail;  /* next slot to read  */

static void ring_push(button_id_t b, button_event_kind_t kind, uint64_t now) {
    uint8_t next_head = (uint8_t)((ring_head + 1) % BTN_RING_CAP);
    if (next_head == ring_tail) {
        /* Full — drop the oldest (the UI cares about the latest input,
         * not the full history).  Same policy as bt_events. */
        ring_tail = (uint8_t)((ring_tail + 1) % BTN_RING_CAP);
    }
    ring[ring_head].button     = b;
    ring[ring_head].kind       = kind;
    ring[ring_head].release_us = now;
    ring_head = next_head;
}

/* ---- Public API ---------------------------------------------------- */

void buttons_init(void) {
    for (int i = 0; i < BTN__COUNT; i++) {
        bs[i].pressed      = false;
        bs[i].press_us     = 0;
        bs[i].long_emitted = false;
    }
    ring_head = ring_tail = 0;

    /* Each pin: input, pull-up on, pull-down explicitly off.  No IRQ —
     * buttons_task() polls these. */
    for (int i = 0; i < BTN__COUNT; i++) {
        uint8_t pin = button_pins[i];
        gpio_init(pin);
        gpio_set_dir(pin, GPIO_IN);
        gpio_set_pulls(pin, true, false);
    }
}

void buttons_task(void) {
    uint64_t now = time_us_64();
    for (int b = 0; b < BTN__COUNT; b++) {
        /* Active-low: pull-up holds the line high at rest; a press
         * shorts it to GND.  raw_down == true means "button held". */
        bool raw_down = !gpio_get(button_pins[b]);

        if (raw_down && !bs[b].pressed) {
            /* Edge: released → pressed.  Start timing; emit nothing
             * yet (we don't know click vs long until release or until
             * the long threshold elapses). */
            bs[b].pressed      = true;
            bs[b].press_us     = now;
            bs[b].long_emitted = false;
        } else if (!raw_down && bs[b].pressed) {
            /* Edge: pressed → released.  Emit a CLICK only if we
             * haven't already fired a LONG for this hold. */
            bs[b].pressed = false;
            if (!bs[b].long_emitted) {
                ring_push((button_id_t)b, BTN_EVT_CLICK, now);
            }
        } else if (raw_down && bs[b].pressed && !bs[b].long_emitted) {
            /* Still held — fire LONG the instant we cross the
             * threshold, for immediate feedback. */
            if (now - bs[b].press_us >= LONG_PRESS_US) {
                bs[b].long_emitted = true;
                ring_push((button_id_t)b, BTN_EVT_LONG, now);
            }
        }
    }
}

bool buttons_poll(button_event_t *out) {
    if (ring_head == ring_tail) return false;
    *out = ring[ring_tail];
    ring_tail = (uint8_t)((ring_tail + 1) % BTN_RING_CAP);
    return true;
}

#endif /* DH_OLED_UI */
