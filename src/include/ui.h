/*
 * OLED UI for board A — issue #22.  Phase 1: status screen only.
 *
 * The UI runs as a single task on core0 at 30 Hz.  It owns the OLED
 * framebuffer, polls the BT event queue, and re-renders only when
 * something visible changed (dirty flag).
 *
 * Phase 1 has no navigation — there's a single screen state
 * (UI_STATE_STATUS) and no button input.  The state machine is
 * structured so Phase 2 can extend it without restructuring.
 */

#pragma once

#include <stdbool.h>
#include "main.h"      /* device_t and its dependency graph */

#ifdef DH_OLED_UI

/* Init the OLED and reset UI state.  Safe to call before tasks start.
 * Returns false if the OLED probe failed (no panel wired) — caller
 * may continue regardless; the task will just no-op. */
bool ui_init(void);

/* Render task entry point — called by the core0 task scheduler.
 * Internally rate-limited; harmless to call faster than needed. */
void ui_render_task(device_t *state);

#else  /* DH_OLED_UI not defined: provide stubs so call sites stay clean */

static inline bool ui_init(void) { return false; }
static inline void ui_render_task(device_t *state) { (void)state; }

#endif
