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
        gpio_pull_up(pin);
        if (i == 0) {
            gpio_set_irq_enabled_with_callback(pin,
                GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE,
                true, &gpio_irq_callback);
        } else {
            gpio_set_irq_enabled(pin,
                GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE, true);
        }
    }
}

bool buttons_poll(button_event_t *out) {
    if (ring_head == ring_tail) return false;
    *out = ring[ring_tail];
    ring_tail = (uint8_t)((ring_tail + 1) % BTN_RING_CAP);
    return true;
}

void buttons_debug_print(void) {
    /* Raw line levels: with internal pull-ups, idle reads 1, a press
     * to GND reads 0.  If a press never flips the level here, the
     * problem is wiring (button not connected, wrong pin, no GND) —
     * not the IRQ or queue.
     *
     * Edge counters: how many falling / rising edges the IRQ has seen
     * per button.  If levels flip but these stay 0, the IRQ callback
     * isn't being invoked (registration overridden, or IRQ not
     * enabled on the pin).
     *
     * irq=total raw GPIO IRQ invocations; unmatched=IRQs whose GPIO
     * wasn't one of ours (would indicate a shared-callback collision
     * with another subsystem). */
    printf("[btn] pins U/D/S=%d/%d/%d  fall=%lu/%lu/%lu  rise=%lu/%lu/%lu  irq=%lu unmatched=%lu\n",
           gpio_get(UI_BUTTON_UP), gpio_get(UI_BUTTON_DOWN), gpio_get(UI_BUTTON_SELECT),
           (unsigned long)buttons_dbg_fall[BTN_UP],
           (unsigned long)buttons_dbg_fall[BTN_DOWN],
           (unsigned long)buttons_dbg_fall[BTN_SELECT],
           (unsigned long)buttons_dbg_rise[BTN_UP],
           (unsigned long)buttons_dbg_rise[BTN_DOWN],
           (unsigned long)buttons_dbg_rise[BTN_SELECT],
           (unsigned long)buttons_dbg_irq_total,
           (unsigned long)buttons_dbg_unmatched);
}

#endif /* DH_OLED_UI */
