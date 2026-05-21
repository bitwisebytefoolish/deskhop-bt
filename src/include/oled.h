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

/* Same as oled_text but with arbitrary pixel-Y position and a scale
 * factor.  scale=1 matches oled_text exactly (6x8 per char); scale=2
 * draws 12x16 "fat-pixel" glyphs for hero-sized text; scale=3 etc.
 * also work but consume the screen fast.  Each source-pixel becomes
 * a scale x scale block. */
void oled_text_at(int x_pixels, int y_pixels, int scale, const char *s);

/* Convenience: clear a single row (8 px high) to black. */
void oled_clear_row(int row);

/* Draw a packed monochrome bitmap at (x, y).  Format matches the
 * SSD1306 framebuffer layout:
 *   bm[col + page*w] = vertical 8-pixel slice, bit 0 = top pixel
 *                      within that slice.
 * For h <= 8, the bitmap is one page tall and bm is w bytes total.
 * For h > 8, stack pages: e.g. h=16 needs 2 pages × w bytes.  Set
 * bits draw on; clear bits are no-ops (icons composite cleanly over
 * existing content). */
void oled_draw_icon(int x, int y, int w, int h, const uint8_t *bm);

/* Public 8x8 icons.  All column-major, bit 0 = top. */
extern const uint8_t oled_icon_kbd[8];
extern const uint8_t oled_icon_mouse[8];
extern const uint8_t oled_icon_keypad[8];
extern const uint8_t oled_icon_generic[8];
extern const uint8_t oled_icon_dot_full[8];
extern const uint8_t oled_icon_dot_empty[8];

/* Push the in-RAM framebuffer to the panel.  ~25 ms at 400 kHz.  No-op
 * if the panel isn't present (oled_init returned false). */
void oled_flush(void);

/* 0..255; SSD1306 contrast register.  Used for inactivity dim. */
void oled_set_contrast(uint8_t level);

#endif /* DH_OLED_UI */
