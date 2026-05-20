/*
 * SSD1306 128x64 mono I2C OLED driver for deskhop-bt's board A UI.
 * Issue #22.
 *
 * Minimal in-tree driver — no external submodule.  The SSD1306 command
 * set we touch is small (~12 commands) so vendoring something larger
 * isn't justified.
 *
 * Threading: all functions assume single-threaded access from core0.
 * Render builds the framebuffer in RAM; oled_flush() pushes it over
 * I2C in one ~25 ms transaction at 400 kHz.  Flush only on dirty so
 * we don't blast the bus.
 *
 * Build gating: this header is only meaningful when DH_OLED_UI is
 * defined.  When undefined, src/oled.c is excluded from the build and
 * callers should guard their includes.
 */

#pragma once

#ifdef DH_OLED_UI

#include <stdbool.h>
#include <stdint.h>

#define OLED_W 128
#define OLED_H 64

/* Initialise I2C, probe for the panel, send the boot command sequence.
 * Returns false if the panel doesn't ACK on the configured address —
 * callers should treat that as "no OLED wired up" and skip further
 * oled_* calls.  The UI render task uses this to no-op cleanly. */
bool oled_init(void);

/* True after a successful oled_init().  Safe to call repeatedly. */
bool oled_is_present(void);

/* Framebuffer accessors. */
void oled_clear(void);
void oled_set_pixel(int x, int y, bool on);
void oled_invert_rect(int x, int y, int w, int h);

/* Text rendering.  Font is 6x8 monospace; x is in pixels, y is a row
 * index 0..7 (8 rows of 8 pixels each — matches the SSD1306's page
 * layout, so vertical text alignment is always pixel-aligned).  Text
 * past the right edge is clipped, not wrapped. */
void oled_text(int x_pixels, int row, const char *s);

/* Convenience: clear a single row (8 px high) to black. */
void oled_clear_row(int row);

/* Push the in-RAM framebuffer to the panel.  ~25 ms at 400 kHz.  No-op
 * if the panel isn't present (oled_init returned false). */
void oled_flush(void);

/* 0..255; SSD1306 contrast register.  Used for inactivity dim. */
void oled_set_contrast(uint8_t level);

#endif /* DH_OLED_UI */
