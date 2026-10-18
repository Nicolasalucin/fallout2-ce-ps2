/*
 * On-screen keyboard for the Fallout PS2 port.
 *
 * The OSK is a virtual input driver: every keystroke reaches the engine as
 * a synthetic SDL key event through the same shim the pad bindings use
 * (ps2_sdl_post_key). The engine text widgets (get_input_str2 in
 * loadsave.cc, the editor prompts) therefore see nothing special - they
 * just receive typed keys, exactly as if a USB keyboard had sent them.
 *
 * Rendering is a software composite into the CT32 frame that video_gs.c
 * builds every present (filled rects + a compact 5x7 bitmap font). It
 * costs nothing while the OSK is closed and never touches an engine
 * buffer: the overlay is painted on top of the converted frame right
 * before the texture upload.
 *
 * Grid (D-pad / left stick move, Cross types):
 *
 *     A B C D E F G H I J
 *     K L M N O P Q R S T
 *     U V W X Y Z 0 1 2 3
 *     4 5 6 7 8 9 _ - . space
 *
 * Square = backspace, Start = submit (Return), Circle = cancel (Escape),
 * L3 = close. The cursor auto-repeats when a direction is held.
 */

#include "ps2_osk.h"
#include "ps2_time.h"

#include "sdl/ps2_sdl_internal.h"

#include <stdio.h>
#include <string.h>

#include <tamtypes.h>
#include <libpad.h>

#define OSK_COLS 10
#define OSK_ROWS 4

/* Hold-to-repeat tuning for the grid cursor. */
#define OSK_NAV_DELAY_MS 400
#define OSK_NAV_RATE_MS 110
#define OSK_STICK_DEADZONE 56

/* Panel geometry (640x480 surface, lower half so the game's own text
 * widget - drawn near the top of the save box - stays visible). */
#define OSK_PANEL_X 40
#define OSK_PANEL_Y 284
#define OSK_PANEL_W 560
#define OSK_PANEL_H 178
#define OSK_BORDER 2
#define OSK_GRID_X 60
#define OSK_GRID_Y 344
#define OSK_CELL_W 52
#define OSK_CELL_H 28

/* Colors are GS CT32 words (A B G R, alpha 0x80 = fully opaque in the
 * engine's palette convention, see video_gs.c). */
#define OSK_GS(r, g, b) ((u32)0x80u << 24 | (u32)(b) << 16 | (u32)(g) << 8 | (u32)(r))
#define OSK_COLOR_BG OSK_GS(12, 14, 30)
#define OSK_COLOR_FRAME OSK_GS(110, 120, 140)
#define OSK_COLOR_HINT OSK_GS(150, 160, 175)
#define OSK_COLOR_TEXT OSK_GS(196, 204, 214)
#define OSK_COLOR_CELL OSK_GS(26, 22, 44)
#define OSK_COLOR_SELECTED OSK_GS(222, 186, 92)
#define OSK_COLOR_ON_SELECTED OSK_GS(24, 18, 8)

static const char s_grid[OSK_ROWS][OSK_COLS] = {
    { 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J' },
    { 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T' },
    { 'U', 'V', 'W', 'X', 'Y', 'Z', '0', '1', '2', '3' },
    { '4', '5', '6', '7', '8', '9', '_', '-', '.', ' ' },
};

typedef struct OskCharKey {
    char ch;
    int scancode;
    int shift;
} OskCharKey;

/* Characters the grid can type, mapped onto SDL scancodes. Letters (and
 * '_', the shifted minus) need the shift modifier to come out uppercase /
 * as an underscore in the engine's keyboard tables. */
static const OskCharKey s_char_keys[] = {
    { 'A', SDL_SCANCODE_A, 1 },
    { 'B', SDL_SCANCODE_B, 1 },
    { 'C', SDL_SCANCODE_C, 1 },
    { 'D', SDL_SCANCODE_D, 1 },
    { 'E', SDL_SCANCODE_E, 1 },
    { 'F', SDL_SCANCODE_F, 1 },
    { 'G', SDL_SCANCODE_G, 1 },
    { 'H', SDL_SCANCODE_H, 1 },
    { 'I', SDL_SCANCODE_I, 1 },
    { 'J', SDL_SCANCODE_J, 1 },
    { 'K', SDL_SCANCODE_K, 1 },
    { 'L', SDL_SCANCODE_L, 1 },
    { 'M', SDL_SCANCODE_M, 1 },
    { 'N', SDL_SCANCODE_N, 1 },
    { 'O', SDL_SCANCODE_O, 1 },
    { 'P', SDL_SCANCODE_P, 1 },
    { 'Q', SDL_SCANCODE_Q, 1 },
    { 'R', SDL_SCANCODE_R, 1 },
    { 'S', SDL_SCANCODE_S, 1 },
    { 'T', SDL_SCANCODE_T, 1 },
    { 'U', SDL_SCANCODE_U, 1 },
    { 'V', SDL_SCANCODE_V, 1 },
    { 'W', SDL_SCANCODE_W, 1 },
    { 'X', SDL_SCANCODE_X, 1 },
    { 'Y', SDL_SCANCODE_Y, 1 },
    { 'Z', SDL_SCANCODE_Z, 1 },
    { '0', SDL_SCANCODE_0, 0 },
    { '1', SDL_SCANCODE_1, 0 },
    { '2', SDL_SCANCODE_2, 0 },
    { '3', SDL_SCANCODE_3, 0 },
    { '4', SDL_SCANCODE_4, 0 },
    { '5', SDL_SCANCODE_5, 0 },
    { '6', SDL_SCANCODE_6, 0 },
    { '7', SDL_SCANCODE_7, 0 },
    { '8', SDL_SCANCODE_8, 0 },
    { '9', SDL_SCANCODE_9, 0 },
    { '_', SDL_SCANCODE_MINUS, 1 },
    { '-', SDL_SCANCODE_MINUS, 0 },
    { '.', SDL_SCANCODE_PERIOD, 0 },
    { ' ', SDL_SCANCODE_SPACE, 0 },
};

static int s_open;
static int s_cx;
static int s_cy;

/* Pad edge tracking. s_have_prev makes the first poll after opening adopt
 * the current buttons, so a pad already held down (the L3 that opened the
 * OSK, a Cross held during an auto-summon) cannot fire immediately. */
static int s_have_prev;
static u16 s_prev_input;

/* Held-direction auto-repeat. */
static int s_hold_dx;
static int s_hold_dy;
static u32 s_repeat_at;

/* Characters typed during this OSK session (display only). */
static char s_text[40];
static int s_text_len;

void ps2_osk_open(void)
{
    if (s_open) {
        return;
    }

    s_open = 1;
    s_cx = 0;
    s_cy = 0;
    s_have_prev = 0;
    s_prev_input = 0;
    s_hold_dx = 0;
    s_hold_dy = 0;
    s_repeat_at = 0;
    s_text_len = 0;
    s_text[0] = '\0';

    printf("[ps2] osk: open\n");
    fflush(stdout);
}

void ps2_osk_close(void)
{
    if (!s_open) {
        return;
    }

    s_open = 0;
    printf("[ps2] osk: close\n");
    fflush(stdout);
}

int ps2_osk_is_open(void)
{
    return s_open;
}

/* Press + release in the same pump: the engine samples modifiers when it
 * processes the keydown, so a shift held only for this burst still lands
 * as an uppercase / shifted character. */
static void osk_tap_key(int scancode)
{
    ps2_sdl_post_key(scancode, 1);
    ps2_sdl_post_key(scancode, 0);
}

static void osk_type_selected(void)
{
    char ch = s_grid[s_cy][s_cx];
    int i;

    for (i = 0; i < (int)(sizeof(s_char_keys) / sizeof(s_char_keys[0])); i++) {
        const OskCharKey* key = &s_char_keys[i];
        if (key->ch != ch) {
            continue;
        }

        if (key->shift) {
            ps2_sdl_post_key(SDL_SCANCODE_LSHIFT, 1);
        }
        ps2_sdl_post_key(key->scancode, 1);
        ps2_sdl_post_key(key->scancode, 0);
        if (key->shift) {
            ps2_sdl_post_key(SDL_SCANCODE_LSHIFT, 0);
        }

        if (s_text_len < (int)sizeof(s_text) - 1) {
            s_text[s_text_len++] = ch;
            s_text[s_text_len] = '\0';
        }

        printf("[ps2] osk: type '%c'\n", ch);
        return;
    }
}

static void osk_move(int dx, int dy)
{
    s_cx += dx;
    s_cy += dy;

    if (s_cx < 0) {
        s_cx = 0;
    } else if (s_cx >= OSK_COLS) {
        s_cx = OSK_COLS - 1;
    }

    if (s_cy < 0) {
        s_cy = 0;
    } else if (s_cy >= OSK_ROWS) {
        s_cy = OSK_ROWS - 1;
    }
}

void ps2_osk_input(unsigned int pressed, int lx, int ly, unsigned int now)
{
    u16 cur = (u16)pressed;
    u16 edge;
    int want_x = 0;
    int want_y = 0;

    if (!s_have_prev) {
        s_have_prev = 1;
        s_prev_input = cur;
        return;
    }

    edge = (u16)(cur & ~s_prev_input);
    s_prev_input = cur;

    /* Grid navigation: D-pad first, then the left stick (dominant axis). */
    if ((cur & PAD_LEFT) != 0) {
        want_x = -1;
    } else if ((cur & PAD_RIGHT) != 0) {
        want_x = 1;
    }

    if ((cur & PAD_UP) != 0) {
        want_y = -1;
    } else if ((cur & PAD_DOWN) != 0) {
        want_y = 1;
    }

    if (want_x == 0 && want_y == 0) {
        int sx = lx - 128;
        int sy = ly - 128;
        int ax = sx < 0 ? -sx : sx;
        int ay = sy < 0 ? -sy : sy;

        if (ax >= OSK_STICK_DEADZONE || ay >= OSK_STICK_DEADZONE) {
            if (ax >= ay) {
                want_x = sx >= 0 ? 1 : -1;
            } else {
                want_y = sy >= 0 ? 1 : -1;
            }
        }
    }

    if (want_x != s_hold_dx || want_y != s_hold_dy) {
        /* Direction change (or new press): one immediate step. */
        s_hold_dx = want_x;
        s_hold_dy = want_y;

        if (want_x != 0 || want_y != 0) {
            osk_move(want_x, want_y);
            s_repeat_at = now + OSK_NAV_DELAY_MS;
        }
    } else if ((want_x != 0 || want_y != 0) && (int)(now - s_repeat_at) >= 0) {
        osk_move(want_x, want_y);
        s_repeat_at = now + OSK_NAV_RATE_MS;
    }

    if ((edge & PAD_CROSS) != 0) {
        osk_type_selected();
    }

    if ((edge & PAD_SQUARE) != 0) {
        osk_tap_key(SDL_SCANCODE_BACKSPACE);
        if (s_text_len > 0) {
            s_text_len--;
            s_text[s_text_len] = '\0';
        }
    }

    if ((edge & PAD_START) != 0) {
        osk_tap_key(SDL_SCANCODE_RETURN);
        ps2_osk_close();
    }

    if ((edge & PAD_CIRCLE) != 0) {
        osk_tap_key(SDL_SCANCODE_ESCAPE);
        ps2_osk_close();
    }

    if ((edge & PAD_L3) != 0) {
        ps2_osk_close();
    }
}

/* ---------------------------------------------------------------- font -- */

/* Compact 5x7 bitmap font: 7 rows per glyph, 5 MSB-first bits per row. */
static const unsigned char s_font_blank[7] = { 0, 0, 0, 0, 0, 0, 0 };

static const unsigned char s_font[][7] = {
    { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 }, /* A */
    { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E }, /* B */
    { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E }, /* C */
    { 0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E }, /* D */
    { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F }, /* E */
    { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 }, /* F */
    { 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F }, /* G */
    { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 }, /* H */
    { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E }, /* I */
    { 0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C }, /* J */
    { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 }, /* K */
    { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F }, /* L */
    { 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 }, /* M */
    { 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11 }, /* N */
    { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E }, /* O */
    { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 }, /* P */
    { 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D }, /* Q */
    { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 }, /* R */
    { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E }, /* S */
    { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 }, /* T */
    { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E }, /* U */
    { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04 }, /* V */
    { 0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A }, /* W */
    { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 }, /* X */
    { 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04 }, /* Y */
    { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F }, /* Z */
    { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E }, /* 0 */
    { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E }, /* 1 */
    { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F }, /* 2 */
    { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E }, /* 3 */
    { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 }, /* 4 */
    { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E }, /* 5 */
    { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E }, /* 6 */
    { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 }, /* 7 */
    { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E }, /* 8 */
    { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C }, /* 9 */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F }, /* _ */
    { 0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00 }, /* - */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C }, /* . */
    { 0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00 }, /* : */
    { 0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08 }, /* > */
};

static const unsigned char* osk_glyph(char ch)
{
    if (ch >= 'A' && ch <= 'Z') {
        return s_font[ch - 'A'];
    }
    if (ch >= '0' && ch <= '9') {
        return s_font[26 + (ch - '0')];
    }

    switch (ch) {
    case '_':
        return s_font[36];
    case '-':
        return s_font[37];
    case '.':
        return s_font[38];
    case ':':
        return s_font[39];
    case '>':
        return s_font[40];
    default:
        return s_font_blank;
    }
}

/* -------------------------------------------------------------- render -- */

static void osk_fill(u32* fb, int width, int height, int x0, int y0, int x1, int y1, u32 color)
{
    int x;
    int y;

    if (x0 < 0) {
        x0 = 0;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    if (x1 >= width) {
        x1 = width - 1;
    }
    if (y1 >= height) {
        y1 = height - 1;
    }
    if (x0 > x1 || y0 > y1) {
        return;
    }

    for (y = y0; y <= y1; y++) {
        u32* row = fb + (size_t)y * width;
        for (x = x0; x <= x1; x++) {
            row[x] = color;
        }
    }
}

static void osk_glyph_draw(u32* fb, int width, int height, int x, int y, char ch, int scale, u32 color)
{
    const unsigned char* glyph = osk_glyph(ch);
    int row;
    int col;

    for (row = 0; row < 7; row++) {
        unsigned int bits = glyph[row];

        for (col = 0; col < 5; col++) {
            int py;
            int px;

            if ((bits & (1u << (4 - col))) == 0) {
                continue;
            }

            for (py = 0; py < scale; py++) {
                int yy = y + row * scale + py;
                u32* line;

                if (yy < 0 || yy >= height) {
                    continue;
                }

                line = fb + (size_t)yy * width;
                for (px = 0; px < scale; px++) {
                    int xx = x + col * scale + px;
                    if (xx >= 0 && xx < width) {
                        line[xx] = color;
                    }
                }
            }
        }
    }
}

static void osk_text_draw(u32* fb, int width, int height, int x, int y, const char* text, int scale, u32 color)
{
    for (; *text != '\0'; text++) {
        osk_glyph_draw(fb, width, height, x, y, *text, scale, color);
        x += 6 * scale;
    }
}

void ps2_osk_render(void* fbp, int width, int height)
{
    static const char hint[] = "D-PAD: MOVE  X: TYPE  SQUARE: DEL  START: OK  CIRCLE: BACK";
    u32* fb = (u32*)fbp;
    int preview_x;
    int r;
    int c;

    if (!s_open || fb == NULL) {
        return;
    }

    /* Panel with a 2-pixel frame. */
    osk_fill(fb, width, height,
        OSK_PANEL_X, OSK_PANEL_Y,
        OSK_PANEL_X + OSK_PANEL_W - 1, OSK_PANEL_Y + OSK_PANEL_H - 1,
        OSK_COLOR_FRAME);
    osk_fill(fb, width, height,
        OSK_PANEL_X + OSK_BORDER, OSK_PANEL_Y + OSK_BORDER,
        OSK_PANEL_X + OSK_PANEL_W - 1 - OSK_BORDER, OSK_PANEL_Y + OSK_PANEL_H - 1 - OSK_BORDER,
        OSK_COLOR_BG);

    /* Hint line. */
    osk_text_draw(fb, width, height, OSK_PANEL_X + 12, OSK_PANEL_Y + 7, hint, 1, OSK_COLOR_HINT);

    /* Typed text (this OSK session only) and a large preview of the
     * selected character. */
    osk_text_draw(fb, width, height, OSK_PANEL_X + 12, OSK_PANEL_Y + 22, ">", 2, OSK_COLOR_TEXT);
    osk_text_draw(fb, width, height, OSK_PANEL_X + 26, OSK_PANEL_Y + 22, s_text, 2, OSK_COLOR_TEXT);

    preview_x = OSK_PANEL_X + OSK_PANEL_W - 52;
    osk_fill(fb, width, height, preview_x, OSK_PANEL_Y + 18, preview_x + 35, OSK_PANEL_Y + 53, OSK_COLOR_SELECTED);
    osk_glyph_draw(fb, width, height, preview_x + 10, OSK_PANEL_Y + 25, s_grid[s_cy][s_cx], 3, OSK_COLOR_ON_SELECTED);

    /* Character grid. */
    for (r = 0; r < OSK_ROWS; r++) {
        for (c = 0; c < OSK_COLS; c++) {
            int x0 = OSK_GRID_X + c * OSK_CELL_W;
            int y0 = OSK_GRID_Y + r * OSK_CELL_H;
            int selected = (r == s_cy && c == s_cx);

            osk_fill(fb, width, height,
                x0, y0, x0 + OSK_CELL_W - 5, y0 + OSK_CELL_H - 5,
                selected ? OSK_COLOR_SELECTED : OSK_COLOR_CELL);
            osk_glyph_draw(fb, width, height,
                x0 + 19, y0 + 5, s_grid[r][c], 2,
                selected ? OSK_COLOR_ON_SELECTED : OSK_COLOR_TEXT);
        }
    }
}
