/*
 * SSD1306 128x64 mono I2C OLED driver.  See src/include/oled.h.
 *
 * Memory layout: the SSD1306 organises its 128x64 display as 8 "pages"
 * of 128 columns, each page being 8 vertically-stacked pixels.  Our
 * framebuffer mirrors that exact layout so a flush is a single linear
 * 1024-byte I2C write after a "set column 0, set page 0" preamble.
 *
 *   byte offset = page * 128 + x   (page = y / 8)
 *   bit  index  = y % 8
 *
 * Init sequence is the canonical Adafruit recipe — well-known and
 * documented in the SSD1306 datasheet.  Variants for 0.96" panels
 * differ mainly in COMPINS (0x12 vs 0x02) and the charge pump enable
 * which we always turn on (we're not driving an external VCC rail).
 */

#include "pico/stdlib.h"

#ifdef DH_OLED_UI

#include "hardware/i2c.h"
#include "hardware/gpio.h"
#include "oled.h"
#include "pinout.h"
#include <string.h>

/* ---- Framebuffer -----------------------------------------------------
 *
 * Allocated with ONE extra byte at the front so the I2C "data" control
 * byte (0x40) can be written into fb_raw[0] and the whole {header, fb}
 * blob can be transmitted in a single i2c_write_blocking() call.
 *
 * This trick is from the pico_ssd1306 library David Schramm
 * (https://github.com/daschr/pico-ssd1306) — proven against many
 * 0.96" panels including the ones in the DonCon2040 project, which
 * use the exact same Hosyond-style modules we're targeting here.
 * The alternative — two writes back-to-back with nostop=true — works
 * on some panels and not others; some controllers can't recover the
 * I2C bus state cleanly when the "address+control" and "data" come
 * in separate START-less segments.
 *
 * Public framebuffer accessors operate on `fb`, which is byte 1
 * onwards of `fb_raw`.  Byte 0 is owned by oled_flush(). */
#define OLED_PAGES (OLED_H / 8)
static uint8_t fb_raw[1 + OLED_PAGES * OLED_W];
#define fb (&fb_raw[1])
static bool    panel_present = false;

/* ---- 6x8 monospace font, ASCII 0x20..0x7E ----------------------------
 * Public-domain "Tom Thumb" / standard 5x7-in-6x8 layout.  Each char is
 * 6 bytes wide; each byte's low 8 bits are one vertical column (LSB on
 * top row).  Tightly packed; ~570 bytes total in flash.
 *
 * The data was generated from the canonical 8x8 ROM font with the right
 * column blanked to give 1 px of inter-character spacing.  ASCII below
 * 0x20 and above 0x7E render as blank (handled in oled_text).
 */
static const uint8_t font6x8[96][6] = {
    {0x00,0x00,0x00,0x00,0x00,0x00}, /* 0x20 ' ' */
    {0x00,0x00,0x5F,0x00,0x00,0x00}, /* 0x21 '!' */
    {0x00,0x07,0x00,0x07,0x00,0x00}, /* 0x22 '"' */
    {0x14,0x7F,0x14,0x7F,0x14,0x00}, /* 0x23 '#' */
    {0x24,0x2A,0x7F,0x2A,0x12,0x00}, /* 0x24 '$' */
    {0x23,0x13,0x08,0x64,0x62,0x00}, /* 0x25 '%' */
    {0x36,0x49,0x55,0x22,0x50,0x00}, /* 0x26 '&' */
    {0x00,0x05,0x03,0x00,0x00,0x00}, /* 0x27 ''' */
    {0x00,0x1C,0x22,0x41,0x00,0x00}, /* 0x28 '(' */
    {0x00,0x41,0x22,0x1C,0x00,0x00}, /* 0x29 ')' */
    {0x14,0x08,0x3E,0x08,0x14,0x00}, /* 0x2A '*' */
    {0x08,0x08,0x3E,0x08,0x08,0x00}, /* 0x2B '+' */
    {0x00,0x50,0x30,0x00,0x00,0x00}, /* 0x2C ',' */
    {0x08,0x08,0x08,0x08,0x08,0x00}, /* 0x2D '-' */
    {0x00,0x60,0x60,0x00,0x00,0x00}, /* 0x2E '.' */
    {0x20,0x10,0x08,0x04,0x02,0x00}, /* 0x2F '/' */
    {0x3E,0x51,0x49,0x45,0x3E,0x00}, /* 0x30 '0' */
    {0x00,0x42,0x7F,0x40,0x00,0x00}, /* 0x31 '1' */
    {0x42,0x61,0x51,0x49,0x46,0x00}, /* 0x32 '2' */
    {0x21,0x41,0x45,0x4B,0x31,0x00}, /* 0x33 '3' */
    {0x18,0x14,0x12,0x7F,0x10,0x00}, /* 0x34 '4' */
    {0x27,0x45,0x45,0x45,0x39,0x00}, /* 0x35 '5' */
    {0x3C,0x4A,0x49,0x49,0x30,0x00}, /* 0x36 '6' */
    {0x01,0x71,0x09,0x05,0x03,0x00}, /* 0x37 '7' */
    {0x36,0x49,0x49,0x49,0x36,0x00}, /* 0x38 '8' */
    {0x06,0x49,0x49,0x29,0x1E,0x00}, /* 0x39 '9' */
    {0x00,0x36,0x36,0x00,0x00,0x00}, /* 0x3A ':' */
    {0x00,0x56,0x36,0x00,0x00,0x00}, /* 0x3B ';' */
    {0x00,0x08,0x14,0x22,0x41,0x00}, /* 0x3C '<' */
    {0x14,0x14,0x14,0x14,0x14,0x00}, /* 0x3D '=' */
    {0x41,0x22,0x14,0x08,0x00,0x00}, /* 0x3E '>' */
    {0x02,0x01,0x51,0x09,0x06,0x00}, /* 0x3F '?' */
    {0x32,0x49,0x79,0x41,0x3E,0x00}, /* 0x40 '@' */
    {0x7E,0x11,0x11,0x11,0x7E,0x00}, /* 0x41 'A' */
    {0x7F,0x49,0x49,0x49,0x36,0x00}, /* 0x42 'B' */
    {0x3E,0x41,0x41,0x41,0x22,0x00}, /* 0x43 'C' */
    {0x7F,0x41,0x41,0x22,0x1C,0x00}, /* 0x44 'D' */
    {0x7F,0x49,0x49,0x49,0x41,0x00}, /* 0x45 'E' */
    {0x7F,0x09,0x09,0x09,0x01,0x00}, /* 0x46 'F' */
    {0x3E,0x41,0x49,0x49,0x7A,0x00}, /* 0x47 'G' */
    {0x7F,0x08,0x08,0x08,0x7F,0x00}, /* 0x48 'H' */
    {0x00,0x41,0x7F,0x41,0x00,0x00}, /* 0x49 'I' */
    {0x20,0x40,0x41,0x3F,0x01,0x00}, /* 0x4A 'J' */
    {0x7F,0x08,0x14,0x22,0x41,0x00}, /* 0x4B 'K' */
    {0x7F,0x40,0x40,0x40,0x40,0x00}, /* 0x4C 'L' */
    {0x7F,0x02,0x0C,0x02,0x7F,0x00}, /* 0x4D 'M' */
    {0x7F,0x04,0x08,0x10,0x7F,0x00}, /* 0x4E 'N' */
    {0x3E,0x41,0x41,0x41,0x3E,0x00}, /* 0x4F 'O' */
    {0x7F,0x09,0x09,0x09,0x06,0x00}, /* 0x50 'P' */
    {0x3E,0x41,0x51,0x21,0x5E,0x00}, /* 0x51 'Q' */
    {0x7F,0x09,0x19,0x29,0x46,0x00}, /* 0x52 'R' */
    {0x46,0x49,0x49,0x49,0x31,0x00}, /* 0x53 'S' */
    {0x01,0x01,0x7F,0x01,0x01,0x00}, /* 0x54 'T' */
    {0x3F,0x40,0x40,0x40,0x3F,0x00}, /* 0x55 'U' */
    {0x1F,0x20,0x40,0x20,0x1F,0x00}, /* 0x56 'V' */
    {0x3F,0x40,0x38,0x40,0x3F,0x00}, /* 0x57 'W' */
    {0x63,0x14,0x08,0x14,0x63,0x00}, /* 0x58 'X' */
    {0x07,0x08,0x70,0x08,0x07,0x00}, /* 0x59 'Y' */
    {0x61,0x51,0x49,0x45,0x43,0x00}, /* 0x5A 'Z' */
    {0x00,0x7F,0x41,0x41,0x00,0x00}, /* 0x5B '[' */
    {0x02,0x04,0x08,0x10,0x20,0x00}, /* 0x5C '\' */
    {0x00,0x41,0x41,0x7F,0x00,0x00}, /* 0x5D ']' */
    {0x04,0x02,0x01,0x02,0x04,0x00}, /* 0x5E '^' */
    {0x40,0x40,0x40,0x40,0x40,0x00}, /* 0x5F '_' */
    {0x00,0x01,0x02,0x04,0x00,0x00}, /* 0x60 '`' */
    {0x20,0x54,0x54,0x54,0x78,0x00}, /* 0x61 'a' */
    {0x7F,0x48,0x44,0x44,0x38,0x00}, /* 0x62 'b' */
    {0x38,0x44,0x44,0x44,0x20,0x00}, /* 0x63 'c' */
    {0x38,0x44,0x44,0x48,0x7F,0x00}, /* 0x64 'd' */
    {0x38,0x54,0x54,0x54,0x18,0x00}, /* 0x65 'e' */
    {0x08,0x7E,0x09,0x01,0x02,0x00}, /* 0x66 'f' */
    {0x0C,0x52,0x52,0x52,0x3E,0x00}, /* 0x67 'g' */
    {0x7F,0x08,0x04,0x04,0x78,0x00}, /* 0x68 'h' */
    {0x00,0x44,0x7D,0x40,0x00,0x00}, /* 0x69 'i' */
    {0x20,0x40,0x44,0x3D,0x00,0x00}, /* 0x6A 'j' */
    {0x7F,0x10,0x28,0x44,0x00,0x00}, /* 0x6B 'k' */
    {0x00,0x41,0x7F,0x40,0x00,0x00}, /* 0x6C 'l' */
    {0x7C,0x04,0x18,0x04,0x78,0x00}, /* 0x6D 'm' */
    {0x7C,0x08,0x04,0x04,0x78,0x00}, /* 0x6E 'n' */
    {0x38,0x44,0x44,0x44,0x38,0x00}, /* 0x6F 'o' */
    {0x7C,0x14,0x14,0x14,0x08,0x00}, /* 0x70 'p' */
    {0x08,0x14,0x14,0x18,0x7C,0x00}, /* 0x71 'q' */
    {0x7C,0x08,0x04,0x04,0x08,0x00}, /* 0x72 'r' */
    {0x48,0x54,0x54,0x54,0x20,0x00}, /* 0x73 's' */
    {0x04,0x3F,0x44,0x40,0x20,0x00}, /* 0x74 't' */
    {0x3C,0x40,0x40,0x20,0x7C,0x00}, /* 0x75 'u' */
    {0x1C,0x20,0x40,0x20,0x1C,0x00}, /* 0x76 'v' */
    {0x3C,0x40,0x30,0x40,0x3C,0x00}, /* 0x77 'w' */
    {0x44,0x28,0x10,0x28,0x44,0x00}, /* 0x78 'x' */
    {0x0C,0x50,0x50,0x50,0x3C,0x00}, /* 0x79 'y' */
    {0x44,0x64,0x54,0x4C,0x44,0x00}, /* 0x7A 'z' */
    {0x00,0x08,0x36,0x41,0x00,0x00}, /* 0x7B '{' */
    {0x00,0x00,0x7F,0x00,0x00,0x00}, /* 0x7C '|' */
    {0x00,0x41,0x36,0x08,0x00,0x00}, /* 0x7D '}' */
    {0x08,0x04,0x08,0x10,0x08,0x00}, /* 0x7E '~' */
    {0x00,0x00,0x00,0x00,0x00,0x00}, /* 0x7F (blank) */
};

/* ---- I2C primitives -------------------------------------------------- */

static bool i2c_cmd(uint8_t c) {
    /* 0x00 = control byte: Co=0, D/C#=0 → next byte is a command. */
    uint8_t buf[2] = {0x00, c};
    int n = i2c_write_timeout_us(OLED_I2C_INSTANCE, OLED_I2C_ADDR,
                                  buf, 2, false, 10000);
    return n == 2;
}

static bool i2c_cmd_seq(const uint8_t *cmds, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (!i2c_cmd(cmds[i])) return false;
    }
    return true;
}

/* ---- Public API ------------------------------------------------------ */

bool oled_init(void) {
    panel_present = false;

    /* Pad I2C bus init.  The pico-sdk i2c API guarantees these are
     * idempotent — if some other code path (e.g. a future Phase 2
     * button matrix sharing I2C) initialised them already, this
     * re-sets clean state. */
    i2c_init(OLED_I2C_INSTANCE, OLED_I2C_BAUD);
    gpio_set_function(OLED_PIN_SDA, GPIO_FUNC_I2C);
    gpio_set_function(OLED_PIN_SCL, GPIO_FUNC_I2C);
    /* External pull-ups on the breadboard expected, but enable the
     * internal pulls too as a belt-and-braces measure for floating-bus
     * scenarios during boot. */
    gpio_pull_up(OLED_PIN_SDA);
    gpio_pull_up(OLED_PIN_SCL);

    /* Probe: a 0-byte write returns the number of bytes written
     * (i.e. 0) on ACK, or PICO_ERROR_GENERIC on no-ACK.  If no panel
     * is wired, this returns < 0 in well under a millisecond and we
     * report not-present so the UI task can no-op. */
    uint8_t probe = 0;
    int rc = i2c_write_timeout_us(OLED_I2C_INSTANCE, OLED_I2C_ADDR,
                                   &probe, 1, false, 5000);
    if (rc < 0) {
        return false;
    }

    /* Boot sequence — SSD1306 128x64 init for charge-pump powered modules.
     *
     * Panel-variant overrides: cheap 0.96" SSD1306 modules from different
     * vendors solder the COM-pin matrix differently and expect different
     * register values.  Three knobs are exposed as build-time overrides
     * so we can iterate on display orientation without touching this
     * file:
     *
     *   -DOLED_COMPINS=0x02  — sequential COM pin config (vs. 0x12 alt)
     *   -DOLED_SEGREMAP=0xA0 — column 0 → SEG0 (vs. 0xA1 column 127→SEG0)
     *   -DOLED_COMSCAN=0xC0  — COM scan increasing (vs. 0xC8 decreasing)
     *
     * Defaults below work for most Adafruit / generic 128x64 panels.
     * Symptoms of mismatch:
     *   - rows displayed in wrong order  → try OLED_COMPINS=0x02
     *   - text upside-down               → flip OLED_COMSCAN
     *   - text mirrored left-right       → flip OLED_SEGREMAP
     *   - rows split into two halves     → try OLED_COMPINS=0x22 or 0x32
     */
#ifndef OLED_COMPINS
/* 0x02 chosen as default after the Phase 1 hardware test on the Hosyond
 * 0.96" panel rendered with the rows in a circular-shifted order under
 * the previous 0x12 default — exactly the symptom of an alternative-
 * vs-sequential COM pin mismatch.  0x02 (sequential) is also the most
 * common value for cheap 0.96" boards from generic AliExpress / Amazon
 * vendors.  Override to 0x12, 0x22, or 0x32 if your specific panel
 * needs the alternative or remapped variants. */
#define OLED_COMPINS 0x02
#endif
#ifndef OLED_SEGREMAP
#define OLED_SEGREMAP 0xA1
#endif
#ifndef OLED_COMSCAN
#define OLED_COMSCAN 0xC8
#endif
    /* MEMORYMODE: 0x00 = horizontal addressing.  Set column-window
     * 0..127 + page-window 0..7 once per flush, then stream all
     * 1024 framebuffer bytes in one I2C transaction.  Auto-increment
     * goes across the row first, then wraps to the next page.  This
     * matches what the upstream pico_ssd1306 library does and what
     * the DonCon2040 project verified works on the Hosyond-style
     * 0.96" panels we're targeting.
     *
     * A previous version of this driver experimented with page-mode
     * addressing + an SH1106 column offset of 2.  That worked on
     * paper for true SH1106 controllers, but it turned out our panel
     * is a genuine SSD1306 — DonCon's horizontal-mode-with-blocking-
     * write is correct for it. */
    static const uint8_t init_seq[] = {
        0xAE,             /* DISPLAYOFF */
        0xD5, 0x80,       /* SETDISPLAYCLOCKDIV: oscillator freq */
        0xA8, 0x3F,       /* SETMULTIPLEX: 64-1 = 0x3F (height-1) */
        0xD3, 0x00,       /* SETDISPLAYOFFSET: 0 */
        0x40,             /* SETSTARTLINE | 0 */
        0x8D, 0x14,       /* CHARGEPUMP: enable */
        0x20, 0x00,       /* MEMORYMODE: horizontal addressing */
        OLED_SEGREMAP,    /* segment remap (orientation knob) */
        OLED_COMSCAN,     /* COM scan direction (orientation knob) */
        0xDA, OLED_COMPINS, /* SET COMPINS (panel-variant knob) */
        0x81, 0xCF,       /* SETCONTRAST: ~80% */
        0xD9, 0xF1,       /* SETPRECHARGE: high VCC */
        0xDB, 0x40,       /* SETVCOMDETECT */
        0xA4,             /* DISPLAYALLON_RESUME: follow RAM */
        0xA6,             /* NORMALDISPLAY: 1 = pixel on */
        0x2E,             /* DEACTIVATE_SCROLL */
        0xAF,             /* DISPLAYON */
    };
    if (!i2c_cmd_seq(init_seq, sizeof(init_seq))) {
        return false;
    }

    /* Start with a black framebuffer + push it to clear any garbage
     * the panel might be holding in RAM from a previous session. */
    panel_present = true;
    memset(fb, 0, (size_t)(OLED_PAGES * OLED_W));
    oled_flush();
    return true;
}

bool oled_is_present(void) {
    return panel_present;
}

void oled_clear(void) {
    memset(fb, 0, (size_t)(OLED_PAGES * OLED_W));
}

void oled_clear_row(int row) {
    if (row < 0 || row >= OLED_PAGES) return;
    memset(&fb[row * OLED_W], 0, OLED_W);
}

void oled_set_pixel(int x, int y, bool on) {
    if (x < 0 || x >= OLED_W || y < 0 || y >= OLED_H) return;
    uint8_t *p = &fb[(y / 8) * OLED_W + x];
    uint8_t  m = (uint8_t)(1u << (y & 7));
    if (on) *p |= m;
    else    *p &= (uint8_t)~m;
}

void oled_invert_rect(int x, int y, int w, int h) {
    /* Inclusive on (x,y), exclusive on the far edge. */
    for (int yy = y; yy < y + h; yy++) {
        if (yy < 0 || yy >= OLED_H) continue;
        for (int xx = x; xx < x + w; xx++) {
            if (xx < 0 || xx >= OLED_W) continue;
            uint8_t *p = &fb[(yy / 8) * OLED_W + xx];
            uint8_t  m = (uint8_t)(1u << (yy & 7));
            *p ^= m;
        }
    }
}

void oled_text(int x_pixels, int row, const char *s) {
    if (row < 0 || row >= OLED_PAGES) return;
    int x = x_pixels;
    while (*s) {
        unsigned char c = (unsigned char)*s++;
        if (c < 0x20 || c > 0x7F) c = 0x7F;  /* render as blank */
        const uint8_t *glyph = font6x8[c - 0x20];
        for (int col = 0; col < 6; col++) {
            if (x < 0 || x >= OLED_W) { x++; continue; }
            fb[row * OLED_W + x] = glyph[col];
            x++;
        }
        if (x >= OLED_W) break;  /* clip — don't wrap */
    }
}

void oled_text_at(int x_pixels, int y_pixels, int scale, const char *s) {
    if (scale < 1) scale = 1;
    /* Fast path: scale=1 with row-aligned y is what oled_text already
     * does cheaply (one byte per column write).  Defer to it. */
    if (scale == 1 && (y_pixels & 7) == 0) {
        oled_text(x_pixels, y_pixels >> 3, s);
        return;
    }
    /* General path: render each font pixel as a scale x scale block
     * via oled_set_pixel.  Slower (per-pixel writes) but supports any
     * (x, y) origin and any scale.  At 30 Hz with one or two scaled
     * strings per frame, the wallclock cost is negligible. */
    int x = x_pixels;
    while (*s) {
        unsigned char c = (unsigned char)*s++;
        if (c < 0x20 || c > 0x7F) c = 0x7F;
        const uint8_t *glyph = font6x8[c - 0x20];
        for (int gc = 0; gc < 6; gc++) {
            uint8_t col_bits = glyph[gc];
            for (int gr = 0; gr < 8; gr++) {
                if ((col_bits >> gr) & 1u) {
                    int px0 = x + gc * scale;
                    int py0 = y_pixels + gr * scale;
                    for (int dy = 0; dy < scale; dy++) {
                        for (int dx = 0; dx < scale; dx++) {
                            oled_set_pixel(px0 + dx, py0 + dy, true);
                        }
                    }
                }
            }
        }
        x += 6 * scale;
        if (x >= OLED_W) break;
    }
}

void oled_draw_icon(int x, int y, int w, int h, const uint8_t *bm) {
    int pages = (h + 7) / 8;
    for (int p = 0; p < pages; p++) {
        for (int col = 0; col < w; col++) {
            uint8_t b = bm[p * w + col];
            for (int bit = 0; bit < 8; bit++) {
                int py = y + p * 8 + bit;
                if (py >= y + h) break;
                if ((b >> bit) & 1u) {
                    oled_set_pixel(x + col, py, true);
                }
            }
        }
    }
}

/* ---- Public 8x8 icons -----------------------------------------------
 *
 * Each icon is 8 columns x 8 rows, stored as 8 bytes in column-major
 * order with bit 0 = top pixel of the column.  Hand-drawn for the
 * Phase 1 status screen redesign (#22).  Visual reference rendered
 * top-to-bottom on the SSD1306 panel:
 *
 *   kbd:               mouse:             keypad:
 *   ........           .#####..           ........
 *   ########           #.....#.           .#.#.#..
 *   #......#           #.#.#.#.           ........
 *   #.#.#..#           #.....#.           .#.#.#..
 *   #......#           #.....#.           ........
 *   ########           #.....#.           .#.#.#..
 *   ........           #.....#.           ........
 *   ........           .#####..           ........
 *
 *   generic ("?"):     dot_full (●):      dot_empty (○):
 *   .#####..           ........           ........
 *   #.....#.           ..###...           ..###...
 *   #.....#.           .#####..           .#...#..
 *   ....#.#.           .#####..           .#...#..
 *   ...#....           .#####..           .#...#..
 *   ........           ..###...           ..###...
 *   ...#....           ........           ........
 *   ........           ........           ........
 */
const uint8_t oled_icon_kbd[8] = {
    0x3E, 0x22, 0x2A, 0x22, 0x2A, 0x22, 0x22, 0x3E,
};
const uint8_t oled_icon_mouse[8] = {
    0x7E, 0x81, 0x85, 0x81, 0x85, 0x81, 0x81, 0x7E,
};
const uint8_t oled_icon_keypad[8] = {
    0x00, 0x2A, 0x00, 0x2A, 0x00, 0x2A, 0x00, 0x00,
};
const uint8_t oled_icon_generic[8] = {
    0x02, 0x01, 0x01, 0x59, 0x09, 0x06, 0x00, 0x00,
};
const uint8_t oled_icon_dot_full[8] = {
    0x00, 0x1C, 0x3E, 0x3E, 0x3E, 0x1C, 0x00, 0x00,
};
const uint8_t oled_icon_dot_empty[8] = {
    0x00, 0x1C, 0x22, 0x22, 0x22, 0x1C, 0x00, 0x00,
};

void oled_flush(void) {
    if (!panel_present) return;

    /* Set column window [0, 127] and page window [0, 7].  Horizontal
     * addressing mode + this window means subsequent data bytes
     * auto-fill row-major across the entire display, wrapping back
     * to (page 0, col 0) after 1024 bytes.  Set explicitly every
     * flush — cheap (6 bytes), and defends against the panel having
     * been left in some other addressing mode by a stray command
     * (e.g. picotool's BOOTSEL handover, or a glitch on the bus). */
    static const uint8_t window[] = {
        0x21, 0x00, 0x7F,   /* SET COLUMN ADDR start=0 end=127 */
        0x22, 0x00, 0x07,   /* SET PAGE   ADDR start=0 end=7   */
    };
    if (!i2c_cmd_seq(window, sizeof(window))) return;

    /* Single-transaction data write.  fb_raw[0] is reserved for the
     * 0x40 control byte (Co=0, D/C#=1 → "all subsequent bytes are
     * GDDRAM data, no further control bytes").  fb_raw[1..1024]
     * holds the actual framebuffer, which the public oled_*
     * functions write through `fb`.
     *
     * Why blocking (no timeout): the prior timeout-based path with
     * 30 ms / 100 ms caps was demonstrably truncating the transfer
     * on the breadboard wiring — visible as stale pixels on the
     * right edge of the display.  i2c_write_blocking has no internal
     * cap; it returns when the bytes are clocked out OR the bus is
     * truly stuck.  For a 1025-byte transfer at 400 kHz the expected
     * wall time is ~23 ms with some jitter — comfortably bounded by
     * the 30 Hz UI render task budget. */
    fb_raw[0] = 0x40;
    i2c_write_blocking(OLED_I2C_INSTANCE, OLED_I2C_ADDR,
                       fb_raw, sizeof(fb_raw), false);
}

void oled_set_contrast(uint8_t level) {
    if (!panel_present) return;
    i2c_cmd(0x81);
    i2c_cmd(level);
}

#endif /* DH_OLED_UI */
