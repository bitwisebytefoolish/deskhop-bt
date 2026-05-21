/*
 * Button input subsystem for the OLED UI (issue #22, Phase 2).
 *
 * Three momentary push-buttons wired GPIO -> button -> GND, with
 * internal pull-up on the GPIO.  Idle reads high; press reads low.
 *
 * Threading: the GPIO IRQ runs on whichever core enabled it — we
 * always enable from core0 inside buttons_init() so the callback
 * always fires on core0.  The UI task (also core0) drains the
 * event queue.  Single producer + single consumer on the same core
 * means no locking is needed for the ring.
 *
 * Classification: the ISR doesn't emit raw PRESS/RELEASE events.
 * Instead it times each press, and emits a SINGLE classified
 * event on RELEASE: either BTN_EVT_CLICK (short) or BTN_EVT_LONG
 * (held >500 ms).  Saves UI consumers from doing their own
 * stopwatching; matches the UX described in the Phase 2 plan
 * (long-press SELECT = back).
 *
 * Build gating: compiles only when DH_OLED_UI is defined (buttons
 * have no use without the screen).  When undefined, the public API
 * functions are inline no-ops so call sites can stay clean.
 */

#pragma once

#ifdef DH_OLED_UI

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    BTN_UP     = 0,
    BTN_DOWN   = 1,
    BTN_SELECT = 2,
    BTN__COUNT
} button_id_t;

typedef enum {
    BTN_EVT_CLICK,   /* press + release <500 ms */
    BTN_EVT_LONG,    /* press + release >=500 ms */
} button_event_kind_t;

typedef struct {
    button_id_t         button;
    button_event_kind_t kind;
    uint64_t            release_us;  /* time_us_64() at the rising edge */
} button_event_t;

/* Init GPIO pins (pull-up, both-edge IRQ), zero the event ring.
 * Safe to call once at boot from setup.c. */
void buttons_init(void);

/* Pop one event off the queue.  Returns true if *out was populated.
 * FIFO order.  Called from the UI task. */
bool buttons_poll(button_event_t *out);

/* Diagnostic: print raw pin levels + IRQ edge counters to the debug
 * UART.  Call from the UI task (normal context); cheap to call at a
 * throttled rate.  Used during button bring-up (#22 Phase 2). */
void buttons_debug_print(void);

#else  /* DH_OLED_UI not defined — stubs */

typedef struct { int _unused; } button_event_t;
static inline void buttons_init(void) {}
static inline bool buttons_poll(button_event_t *out) { (void)out; return false; }

#endif
