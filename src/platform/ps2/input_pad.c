/*
 * DualShock 2 input backend for the Fallout PS2 port.
 *
 * Polls port 0 with libpad and translates the pad state into the SDL shim's
 * synthetic event stream (relative mouse motion + mouse buttons + keys).
 *
 * Button mapping (see README_PS2.txt, shipped with the release):
 *
 *   Left stick   cursor, smooth acceleration curve
 *   Right stick  scroll wheel
 *   D-pad        arrow keys
 *   Cross        left click
 *   Circle       right click / interact
 *   Square       inventory (I)
 *   Triangle     character sheet (C)
 *   Select       Pip-Boy (P)
 *   Start        main menu / close (Escape)
 *   L1           change active hand (B)
 *   L2           cycle item/weapon mode (N)
 *   R1           attack / enter combat (A); end-combat attempt (Return)
 *                while a battle is running
 *   R2           end turn / skip dialog (Space)
 *   R3           automap (Tab)
 *   L3           toggle the on-screen keyboard
 *
 * The on-screen keyboard (ps2_osk.c) is modal: while it is open the whole
 * pad feeds its grid (D-pad/stick move, Cross types, Square backspaces,
 * Start submits, Circle cancels) and no binding fires.
 */

#include "ps2_input.h"

#include "ps2_osk.h"
#include "ps2_time.h"
#include "sdl/ps2_sdl_internal.h"

#include <math.h>
#include <stdio.h>

#include <tamtypes.h>
#include <loadfile.h>
#include <libpad.h>
#include <sifrpc.h>

#define PAD_PORT 0
#define PAD_SLOT 0

/* Pad report mode byte for digital pads (libpad.h has no name for it). */
#define PAD_REPORT_MODE_DIGITAL 0x41

/* Cursor tuning. */
#define STICK_DEADZONE 0.22f
#define CURSOR_MAX_SPEED 450.0f /* px/s at full deflection */
#define CURSOR_LINEAR_TERM 0.15f /* weight of the linear term (precision) */
#define CURSOR_SMOOTHING 0.35f /* velocity low-pass per poll */
#define WHEEL_MAX_SPEED 14.0f /* wheel steps/s at full deflection */
#define DEBUG_MOVE_PERIOD_MS 200 /* rate limit for move debug lines */

typedef enum InputActionKind {
    ACT_KEY,
    ACT_MOUSE,
} InputActionKind;

typedef struct InputBinding {
    u16 pad_bit;
    u8 kind;
    int code; /* SDL scancode (ACT_KEY) or button number (ACT_MOUSE) */
    const char* name;
} InputBinding;

static const InputBinding s_bindings[] = {
    { PAD_UP, ACT_KEY, SDL_SCANCODE_UP, "UP" },
    { PAD_DOWN, ACT_KEY, SDL_SCANCODE_DOWN, "DOWN" },
    { PAD_LEFT, ACT_KEY, SDL_SCANCODE_LEFT, "LEFT" },
    { PAD_RIGHT, ACT_KEY, SDL_SCANCODE_RIGHT, "RIGHT" },
    { PAD_CROSS, ACT_MOUSE, SDL_BUTTON_LEFT, "CROSS" },
    { PAD_CIRCLE, ACT_MOUSE, SDL_BUTTON_RIGHT, "CIRCLE" },
    { PAD_SQUARE, ACT_KEY, SDL_SCANCODE_I, "SQUARE" },
    { PAD_TRIANGLE, ACT_KEY, SDL_SCANCODE_C, "TRIANGLE" },
    { PAD_SELECT, ACT_KEY, SDL_SCANCODE_P, "SELECT" },
    { PAD_START, ACT_KEY, SDL_SCANCODE_ESCAPE, "START" },
    { PAD_L1, ACT_KEY, SDL_SCANCODE_B, "L1" },
    { PAD_L2, ACT_KEY, SDL_SCANCODE_N, "L2" },
    { PAD_R1, ACT_KEY, SDL_SCANCODE_A, "R1" },
    { PAD_R2, ACT_KEY, SDL_SCANCODE_SPACE, "R2" },
    { PAD_R3, ACT_KEY, SDL_SCANCODE_TAB, "R3" },
};
#define NUM_BINDINGS ((int)(sizeof(s_bindings) / sizeof(s_bindings[0])))

static char s_pad_buf[256] __attribute__((aligned(64)));
static int s_pad_ready;
static int s_analog_ok;
static int s_mode_logged;
static int s_first_read = 1;
static u16 s_prev_buttons = 0xFFFF;
static int s_last_state = -1;
static u32 s_last_poll;
static u32 s_last_move_log;
static float s_vel_x;
static float s_vel_y;
static float s_cursor_x;
static float s_cursor_y;
static float s_wheel_x;
static float s_wheel_y;
static int s_r1_code = SDL_SCANCODE_A;

void ps2_input_init(void)
{
    int ret;

    /* ROM modules may already be resident; failures are harmless. */
    SifInitRpc(0);
    SifLoadModule("rom0:SIO2MAN", 0, NULL);
    SifLoadModule("rom0:PADMAN", 0, NULL);

    ret = padInit(0);
    if (ret == 0) {
        printf("[ps2] pad: padInit failed, no pad input\n");
        return;
    }

    ret = padPortOpen(PAD_PORT, PAD_SLOT, s_pad_buf);
    if (ret == 0) {
        printf("[ps2] pad: padPortOpen failed, no pad input\n");
        return;
    }

    s_pad_ready = 1;
    printf("[ps2] pad: libpad up, probing port %d\n", PAD_PORT);
}

/* Deadzone + rescale to 0..1 preserving sign. */
static float stick_axis(unsigned char value)
{
    float f = ((float)value - 128.0f) / 127.0f;
    float m = fabsf(f);

    if (m <= STICK_DEADZONE) {
        return 0.0f;
    }

    m = (m - STICK_DEADZONE) / (1.0f - STICK_DEADZONE);
    if (m > 1.0f) {
        m = 1.0f;
    }

    return f < 0.0f ? -m : m;
}

/* Smooth acceleration: linear term for precision + quadratic ramp. */
static float cursor_speed(float axis)
{
    float m = fabsf(axis);
    float factor = m * (1.0f - CURSOR_LINEAR_TERM) + CURSOR_LINEAR_TERM;

    return axis * factor * CURSOR_MAX_SPEED;
}

/* Resolve the SDL code a binding emits for a press or release. R1 is
 * context sensitive: 'a' attacks (enters combat mode) outside a battle,
 * Return is the end-combat attempt inside one. The code is latched on
 * press so the matching release always uses the same key. */
static int binding_code(const InputBinding* b, int down)
{
    if (b->pad_bit == PAD_R1) {
        if (down) {
            s_r1_code = ps2_platform_in_combat() ? SDL_SCANCODE_RETURN : SDL_SCANCODE_A;
        }
        return s_r1_code;
    }

    return b->code;
}

static void release_all(void)
{
    int i;

    if (s_prev_buttons == 0xFFFF) {
        return;
    }

    for (i = 0; i < NUM_BINDINGS; i++) {
        const InputBinding* b = &s_bindings[i];
        if ((s_prev_buttons & b->pad_bit) == 0) {
            continue;
        }
        if (b->kind == ACT_KEY) {
            ps2_sdl_post_key(binding_code(b, 0), 0);
        } else {
            ps2_sdl_post_mouse_button(b->code, 0);
        }
    }

    s_prev_buttons = 0xFFFF;
}

static void process_buttons(u16 pressed)
{
    int i;

    for (i = 0; i < NUM_BINDINGS; i++) {
        const InputBinding* b = &s_bindings[i];
        int was = (s_prev_buttons & b->pad_bit) != 0;
        int now = (pressed & b->pad_bit) != 0;

        if (was == now) {
            continue;
        }

        if (b->kind == ACT_KEY) {
            ps2_sdl_post_key(binding_code(b, now), now);
        } else {
            ps2_sdl_post_mouse_button(b->code, now);
        }

        printf("[ps2] pad: %s %s\n", b->name, now ? "down" : "up");
    }

    s_prev_buttons = pressed;
}

/* Route one pad sample: the OSK swallows the whole pad while open, L3
 * toggles it, everything else runs through the normal bindings. */
static void handle_pad_state(u16 pressed, int lx, int ly, u32 now)
{
    if (ps2_osk_is_open()) {
        ps2_osk_input(pressed, lx, ly, now);
        s_vel_x = 0.0f;
        s_vel_y = 0.0f;
        s_prev_buttons = pressed;
        return;
    }

    if ((pressed & PAD_L3) != 0 && (s_prev_buttons & PAD_L3) == 0) {
        ps2_osk_open();
        s_prev_buttons = pressed;
        return;
    }

    process_buttons(pressed);
}

static void process_sticks(const struct padButtonStatus* status, u32 now)
{
    static u32 last_time;
    u32 elapsed = now - last_time;
    float dt;
    float speed_x;
    float speed_y;
    int dx;
    int dy;
    int wx;
    int wy;

    /* Digital reports carry no joystick data (bytes are zero); ignore. */
    if (status->mode == PAD_REPORT_MODE_DIGITAL) {
        s_vel_x = 0.0f;
        s_vel_y = 0.0f;
        return;
    }

    if (last_time == 0 || elapsed == 0) {
        elapsed = 16;
    } else if (elapsed > 50) {
        elapsed = 50; /* stall clamp; avoids cursor jumps after hitches */
    }
    last_time = now;
    dt = (float)elapsed / 1000.0f;

    /* Left stick -> cursor. libpad reports the vertical axis with 0xFF at
     * "up" (hardware and PCSX2 alike) while the cursor moves down for
     * positive dy, so the vertical axis is negated. */
    speed_x = cursor_speed(stick_axis(status->ljoy_h));
    speed_y = cursor_speed(-stick_axis(status->ljoy_v));

    s_vel_x += (speed_x - s_vel_x) * CURSOR_SMOOTHING;
    s_vel_y += (speed_y - s_vel_y) * CURSOR_SMOOTHING;

    /* Kill residual creep when the stick is centered. */
    if (speed_x == 0.0f && fabsf(s_vel_x) < 4.0f) {
        s_vel_x = 0.0f;
    }
    if (speed_y == 0.0f && fabsf(s_vel_y) < 4.0f) {
        s_vel_y = 0.0f;
    }

    s_cursor_x += s_vel_x * dt;
    s_cursor_y += s_vel_y * dt;

    dx = (int)s_cursor_x;
    dy = (int)s_cursor_y;
    if (dx != 0 || dy != 0) {
        s_cursor_x -= (float)dx;
        s_cursor_y -= (float)dy;
        ps2_sdl_post_mouse_move(dx, dy);

        if (now - s_last_move_log >= DEBUG_MOVE_PERIOD_MS) {
            s_last_move_log = now;
            printf("[ps2] pad: move %d,%d\n", dx, dy);
        }
    }

    /* Right stick -> scroll wheel. */
    s_wheel_x += stick_axis(status->rjoy_h) * WHEEL_MAX_SPEED * dt;
    s_wheel_y += stick_axis(status->rjoy_v) * WHEEL_MAX_SPEED * dt;

    wx = (int)s_wheel_x;
    wy = (int)s_wheel_y;
    if (wx != 0 || wy != 0) {
        s_wheel_x -= (float)wx;
        s_wheel_y -= (float)wy;
        ps2_sdl_post_mouse_wheel(wx, wy);
    }
}

/* Log a raw status line only when buttons or mode change (quiet otherwise). */
static void log_raw_change(const struct padButtonStatus* status)
{
    static int last_mode = -1;
    static int last_btns = -1;

    if (status->mode == last_mode && status->btns == last_btns) {
        return;
    }

    last_mode = status->mode;
    last_btns = status->btns;

    printf("[ps2] pad raw: mode=%02x btns=%04x l=(%d,%d) r=(%d,%d)\n",
        status->mode, status->btns,
        status->ljoy_h, status->ljoy_v, status->rjoy_h, status->rjoy_v);
}

#ifdef PS2_DEV_SMOKE_TEST
/* Smoke-test hooks: the pad path (bindings + OSK) driven without hardware.
 * The injected mask uses the same active-high PAD_* layout.
 * ps2_input_smoke_release() keeps injection active for one more poll
 * with buttons==0 so s_prev_buttons in both input_pad.c and ps2_osk.c
 * always resets to 0 between consecutive identical button taps. */
static int s_inject_active;
static int s_inject_release_pending;
static u16 s_inject_buttons;

void ps2_input_smoke_press(unsigned int buttons)
{
    s_inject_buttons = (u16)buttons;
    s_inject_release_pending = 0;
    s_inject_active = 1;
}

void ps2_input_smoke_release(void)
{
    s_inject_buttons = 0;
    s_inject_release_pending = 1;
}

int ps2_input_smoke_release_pending(void)
{
    return s_inject_release_pending;
}
#endif

void ps2_input_poll(void)
{
    struct padButtonStatus status;
    u32 now;
    int state;
    u16 pressed;

    now = ps2_time_ms();
    if (now == s_last_poll) {
        return; /* at most one hardware probe per ms */
    }
    s_last_poll = now;

#ifdef PS2_DEV_SMOKE_TEST
    if (s_inject_active) {
        /* Smoke-driven sample: ignores the hardware entirely. */
        u16 injected = s_inject_buttons;

        if (s_first_read) {
            s_first_read = 0;
            s_prev_buttons = injected;
        } else {
            handle_pad_state(injected, 128, 128, now);
        }
        s_prev_buttons = injected;
        s_vel_x = 0.0f;
        s_vel_y = 0.0f;

        if (s_inject_release_pending) {
            s_inject_release_pending = 0;
            s_inject_active = 0;
        }
        return;
    }
#endif

    if (!s_pad_ready) {
        return;
    }

    state = padGetState(PAD_PORT, PAD_SLOT);
    if (state != PAD_STATE_STABLE) {
        /* Unplugged, (re)searching or executing a command: idle safely. */
        release_all();
        s_first_read = 1;
        s_vel_x = 0.0f;
        s_vel_y = 0.0f;
        s_analog_ok = 0;

        if (state != s_last_state) {
            s_last_state = state;
            printf("[ps2] pad: state %d\n", state);
        }
        return;
    }

    if (s_last_state != state) {
        s_last_state = state;
        printf("[ps2] pad: stable\n");
    }

    /* Ask for DualShock (analog) mode; DS2 pads boot in digital mode and
     * PCSX2 emulates that faithfully. */
    if (!s_analog_ok) {
        int mode = padInfoMode(PAD_PORT, PAD_SLOT, PAD_MODECURID, 0);
        if (!s_mode_logged) {
            s_mode_logged = 1;
            printf("[ps2] pad: mode id %d\n", mode);
        }
        if (mode == PAD_TYPE_DIGITAL) {
            if (padSetMainMode(PAD_PORT, PAD_SLOT, PAD_MMODE_DUALSHOCK, PAD_MMODE_LOCK) > 0) {
                s_analog_ok = 1;
                printf("[ps2] pad: dualshock mode requested\n");
            }
        } else {
            s_analog_ok = 1;
        }
    }

    if (padRead(PAD_PORT, PAD_SLOT, &status) == 0) {
        return;
    }

    /* Raw transition log for the input bring-up diagnostics. */
    log_raw_change(&status);

    pressed = (u16)(0xFFFF ^ status.btns);

    if (s_first_read) {
        /* Adopt the first stable state without emitting transitions. */
        s_first_read = 0;
        s_prev_buttons = pressed;
    } else {
        handle_pad_state(pressed, status.ljoy_h, status.ljoy_v, now);
    }

    if (!ps2_osk_is_open()) {
        process_sticks(&status, now);
    }
}
