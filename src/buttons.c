/*
 * Button input implementation.  See src/include/buttons.h.
 *
 * IRQ-driven, with software debounce and on-release classification
 * (short click vs long press).  Designed for the 3-button UI on
 * board A (#22 Phase 2).
 *
 * Memory: 3 pins × per-button state (~24 B) + 8-slot ring (~96 B) =
 * <200 B in .bss.  Trivial.
 */

#include "pico/stdlib.h"

#ifdef DH_OLED_UI

#include <stdio.h>
#include "hardware/gpio.h"
#include "buttons.h"
#include "pinout.h"

/* ---- Diagnostics (issue #22 button bring-up) -------------------------
 * Edge counters incremented in the IRQ; raw levels read on demand.
 * Printed from the UI task (normal context) — never printf() from the
 * IRQ itself, which can deadlock on the pico-sdk stdio mutex if the
 * foreground code is mid-print. */
volatile uint32_t buttons_dbg_fall[BTN__COUNT];
volatile uint32_t buttons_dbg_rise[BTN__COUNT];
volatile uint32_t buttons_dbg_irq_total;
volatile uint32_t buttons_dbg_unmatched;  /* IRQs whose GPIO didn't map to a button */

/* ---- Pin ↔ button mapping --------------------------------------------
 * Index by button_id_t.  Single source of truth: pinout.h's
 * UI_BUTTON_* defines.  Adding a 4th button (e.g. BACK on GP22)
 * is a 2-line change here and a new enumerator in buttons.h. */
static const uint8_t button_pins[BTN__COUNT] = {
    [BTN_UP]     = UI_BUTTON_UP,
    [BTN_DOWN]   = UI_BUTTON_DOWN,
    [BTN_SELECT] = UI_BUTTON_SELECT,
};

/* ---- Per-button state owned by the ISR ----------------------------- */

#define DEBOUNCE_US      20000   /* 20 ms — ignore edges closer than this */
#define LONG_PRESS_US   500000   /* 500 ms threshold for click-vs-long */

typedef struct {
    uint64_t last_edge_us;   /* most recent edge timestamp, any direction */
    uint64_t press_us;       /* timestamp of the active press, 0 if released */
} button_state_t;

static volatile button_state_t bs[BTN__COUNT];

/* ---- Event ring ----------------------------------------------------- */

#define BTN_RING_CAP 8
static volatile button_event_t ring[BTN_RING_CAP];
static volatile uint8_t        ring_head;  /* next slot to write */
static volatile uint8_t        ring_tail;  /* next slot to read  */

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

/* ---- IRQ handler --------------------------------------------------- */

static int8_t pin_to_button(uint gpio) {
    for (int i = 0; i < BTN__COUNT; i++) {
        if (button_pins[i] == gpio) return (int8_t)i;
    }
    return -1;
}

static void gpio_irq_callback(uint gpio, uint32_t events) {
    buttons_dbg_irq_total++;
    int8_t b = pin_to_button(gpio);
    if (b < 0) { buttons_dbg_unmatched++; return; }  /* not one of our pins */

    if (events & GPIO_IRQ_EDGE_FALL) buttons_dbg_fall[b]++;
    if (events & GPIO_IRQ_EDGE_RISE) buttons_dbg_rise[b]++;

    uint64_t now = time_us_64();

    /* Software debounce: ignore edges arriving inside the chatter
     * window of the previous edge on the same pin.  Mechanical
     * tactiles bounce for 5-15 ms typically; 20 ms is comfortable. */
    if (now - bs[b].last_edge_us < DEBOUNCE_US) return;
    bs[b].last_edge_us = now;

    if (events & GPIO_IRQ_EDGE_FALL) {
        /* Press — record start time, don't emit yet. */
        bs[b].press_us = now;
    } else if (events & GPIO_IRQ_EDGE_RISE) {
        /* Release — classify and emit.  Guard against release-without-
         * preceding-press (can happen if the first edge after boot is
         * a rising one because the pin was already low). */
        if (bs[b].press_us == 0) return;
        uint64_t held = now - bs[b].press_us;
        bs[b].press_us = 0;
        ring_push((button_id_t)b,
                  (held >= LONG_PRESS_US) ? BTN_EVT_LONG : BTN_EVT_CLICK,
                  now);
    }
}

/* ---- Public API ---------------------------------------------------- */

void buttons_init(void) {
    /* Reset state in case of warm boot. */
    for (int i = 0; i < BTN__COUNT; i++) {
        bs[i].last_edge_us = 0;
        bs[i].press_us     = 0;
    }
    ring_head = ring_tail = 0;

    /* Set up each pin: input, pull-up, IRQ on both edges.
     * gpio_set_irq_enabled_with_callback registers ONE global callback
     * for all GPIO IRQs — we install it on the first iteration, then
     * use the cheaper enable-only variant for the rest. */
    for (int i = 0; i < BTN__COUNT; i++) {
        uint8_t pin = button_pins[i];
        gpio_init(pin);
        gpio_set_dir(pin, GPIO_IN);
        /* Explicit pull config: pull-up ON, pull-down OFF.  Equivalent
         * to gpio_pull_up() but states the pull-down=false intent
         * directly — guards against any prior config leaving a
         * pull-down latched, which would fight the pull-up and leave
         * the pin near ground. */
        gpio_set_pulls(pin, true, false);
        if (i == 0) {
            gpio_set_irq_enabled_with_callback(pin,
                GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE,
                true, &gpio_irq_callback);
        } else {
            gpio_set_irq_enabled(pin,
                GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE, true);
        }
    }

    /* One-time readback so the UART log proves the firmware-side pull
     * config actually took.  If this prints pu=1 pd=0 for every pin
     * but a multimeter still reads ~0 V at idle, the pull-down is
     * external (wiring) — not us.  If it prints pu=0, the pico-sdk
     * call didn't stick and the bug is here. */
    for (int i = 0; i < BTN__COUNT; i++) {
        uint8_t pin = button_pins[i];
        printf("[btn] init pin GP%u: dir=in pu=%d pd=%d level=%d\n",
               pin,
               gpio_is_pulled_up(pin),
               gpio_is_pulled_down(pin),
               gpio_get(pin));
    }
}

bool buttons_poll(button_event_t *out) {
    if (ring_head == ring_tail) return false;
    *out = ring[ring_tail];
    ring_tail = (uint8_t)((ring_tail + 1) % BTN_RING_CAP);
    return true;
}

void buttons_debug_tick(void) {
    /* Called every UI frame (~30 Hz).  Polls the raw pin levels and
     * LATCHES whether each was ever seen low since the last printed
     * report — the once-a-second print alone is too coarse to catch a
     * brief button press, but polling at 30 Hz reliably samples a
     * normal human press (50-200 ms low).
     *
     * Decision matrix (compare seenlow vs the IRQ edge counters):
     *   seenlow stays 0 on a press   -> WIRING.  The line never pulls
     *       to GND: button not connected, wrong pin, or no ground
     *       path.  The IRQ/queue are irrelevant until this flips.
     *   seenlow flips 1 but fall=0   -> IRQ NOT FIRING.  The level
     *       changes but the GPIO IRQ callback isn't invoked — our
     *       gpio_set_irq_enabled_with_callback got displaced (a
     *       later registrant on this core wins), or the IRQ was
     *       disabled.  Fix = poll instead of IRQ, or re-register.
     *   seenlow + fall both move but no [btn] event line elsewhere
     *       -> debounce / queue / classification bug in this file. */
    static uint8_t  seen_low[BTN__COUNT];
    static uint32_t tick;

    if (!gpio_get(UI_BUTTON_UP))     seen_low[BTN_UP]     = 1;
    if (!gpio_get(UI_BUTTON_DOWN))   seen_low[BTN_DOWN]   = 1;
    if (!gpio_get(UI_BUTTON_SELECT)) seen_low[BTN_SELECT] = 1;

    if (++tick < 30) return;
    tick = 0;

    printf("[btn] now U/D/S=%d/%d/%d  seenlow=%d/%d/%d  fall=%lu/%lu/%lu  rise=%lu/%lu/%lu  irq=%lu unm=%lu\n",
           gpio_get(UI_BUTTON_UP), gpio_get(UI_BUTTON_DOWN), gpio_get(UI_BUTTON_SELECT),
           seen_low[BTN_UP], seen_low[BTN_DOWN], seen_low[BTN_SELECT],
           (unsigned long)buttons_dbg_fall[BTN_UP],
           (unsigned long)buttons_dbg_fall[BTN_DOWN],
           (unsigned long)buttons_dbg_fall[BTN_SELECT],
           (unsigned long)buttons_dbg_rise[BTN_UP],
           (unsigned long)buttons_dbg_rise[BTN_DOWN],
           (unsigned long)buttons_dbg_rise[BTN_SELECT],
           (unsigned long)buttons_dbg_irq_total,
           (unsigned long)buttons_dbg_unmatched);

    seen_low[BTN_UP] = seen_low[BTN_DOWN] = seen_low[BTN_SELECT] = 0;
}

#endif /* DH_OLED_UI */
