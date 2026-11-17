/* SaunaOS's SDL video driver: keyboard, mouse and cursor. See
 * SDL_saunaos_video.h.
 *
 * KEYS GO BY POSITION: uapp's on_phys_key hands the evdev keycode, and
 * SDL's own table turns it into a scancode -- what every SDL game reads
 * for its controls. The CHARACTER a key typed (on_key, the layout
 * applied) is SDL's text input, sent only while the program has text
 * input on. */
#include "SDL_internal.h"

#ifdef SDL_VIDEO_DRIVER_PRIVATE

#include "SDL_saunaos_video.h"
#include "events/SDL_windowevents_c.h"
#include "events/SDL_keyboard_c.h"
#include "events/SDL_mouse_c.h"
#include "events/scancodes_linux.h"
#include "keyboard.h"
#include "win_proto.h"
#include "ui/uapp.h"

static SDL_WindowData *data_of(struct uapp_window *uw)
{
    return (SDL_WindowData *)uapp_window_state(uw);
}

/* Window pixels -> SDL's points, by the window's actual size (the
 * desktop may have clamped it), so the far edge is the far edge. */
static void to_points(struct uapp_window *uw, SDL_Window *window, int x, int y, float *px, float *py)
{
    int cw = uapp_window_width(uw), ch = uapp_window_height(uw);
    *px = cw > 0 ? (float)x * window->w / cw : (float)x;
    *py = ch > 0 ? (float)y * window->h / ch : (float)y;
}

static void motion(struct uapp_window *uw, int x, int y)
{
    SDL_Window *window = data_of(uw)->window;
    float px, py;
    to_points(uw, window, x, y, &px, &py);
    if (SDL_GetMouseFocus() != window) {
        SDL_SetMouseFocus(window);
    }
    SDL_SendMouseMotion(0, window, SDL_DEFAULT_MOUSE_ID, false, px, py);
}

/* uapp reports the buttons DOWN; SDL wants each change. */
static void buttons_now(struct uapp_window *uw, unsigned now)
{
    static const struct { unsigned bit; Uint8 sdl; } map[] = {
        { WIN_MOUSE_BTN_PRIMARY, SDL_BUTTON_LEFT },
        { WIN_MOUSE_BTN_SECONDARY, SDL_BUTTON_RIGHT },
        { WIN_MOUSE_BTN_MIDDLE, SDL_BUTTON_MIDDLE },
        { WIN_MOUSE_BTN_SIDE, SDL_BUTTON_X1 },
        { WIN_MOUSE_BTN_EXTRA, SDL_BUTTON_X2 },
    };
    SDL_WindowData *data = data_of(uw);
    for (int i = 0; i < (int)SDL_arraysize(map); i++) {
        bool was = (data->buttons & map[i].bit) != 0, is = (now & map[i].bit) != 0;
        if (was != is) {
            SDL_SendMouseButton(0, data->window, SDL_DEFAULT_MOUSE_ID, map[i].sdl, is);
        }
    }
    data->buttons = now;
}

static void on_motion(struct uapp_window *uw, int x, int y, unsigned buttons)
{
    (void)buttons;
    motion(uw, x, y);
}

/* A PRESS names the button just pressed, not the others held; a RELEASE
 * names those STILL held, never the one let go (abi/win_proto.h). So a
 * press is added to what is known to be down and a release replaces it --
 * whichever callback each took. */
/* SDL_SetWindowHitTest's answer at a primary press: 0 for a normal
 * press, else it started a move (-1) or a resize (WIN_EDGE_* bits) and
 * the program hears no press -- X11's and Windows' rule. A BORDERLESS
 * window's only: toywm moves only a window that draws its own frame. */
static int hit_test(struct uapp_window *uw, int x, int y)
{
    static const unsigned edges[] = {
        [SDL_HITTEST_RESIZE_TOPLEFT] = WIN_EDGE_TOP | WIN_EDGE_LEFT,
        [SDL_HITTEST_RESIZE_TOP] = WIN_EDGE_TOP,
        [SDL_HITTEST_RESIZE_TOPRIGHT] = WIN_EDGE_TOP | WIN_EDGE_RIGHT,
        [SDL_HITTEST_RESIZE_RIGHT] = WIN_EDGE_RIGHT,
        [SDL_HITTEST_RESIZE_BOTTOMRIGHT] = WIN_EDGE_BOTTOM | WIN_EDGE_RIGHT,
        [SDL_HITTEST_RESIZE_BOTTOM] = WIN_EDGE_BOTTOM,
        [SDL_HITTEST_RESIZE_BOTTOMLEFT] = WIN_EDGE_BOTTOM | WIN_EDGE_LEFT,
        [SDL_HITTEST_RESIZE_LEFT] = WIN_EDGE_LEFT,
    };
    SDL_Window *window = data_of(uw)->window;
    if (!window->hit_test || !(window->flags & SDL_WINDOW_BORDERLESS)) {
        return 0;
    }
    float px, py;
    to_points(uw, window, x, y, &px, &py);
    SDL_Point pt = { (int)px, (int)py };
    SDL_HitTestResult r = window->hit_test(window, &pt, window->hit_test_data);
    if (r == SDL_HITTEST_DRAGGABLE) {
        uapp_window_move_start(uw, 0);
        return -1;
    }
    if (r >= SDL_HITTEST_RESIZE_TOPLEFT && r <= SDL_HITTEST_RESIZE_LEFT &&
        (window->flags & SDL_WINDOW_RESIZABLE)) {
        uapp_window_move_start(uw, edges[r]);
        return (int)edges[r];
    }
    return 0;
}

static void on_press(struct uapp_window *uw, int x, int y, unsigned buttons)
{
    if (hit_test(uw, x, y)) {
        return;
    }
    motion(uw, x, y);
    buttons_now(uw, data_of(uw)->buttons | buttons);
}

static void on_release(struct uapp_window *uw, int x, int y, unsigned buttons)
{
    motion(uw, x, y);
    buttons_now(uw, buttons);
}

static void on_button(struct uapp_window *uw, int x, int y, unsigned buttons, int up)
{
    motion(uw, x, y);
    buttons_now(uw, up ? buttons : data_of(uw)->buttons | buttons);
}

static void on_leave(struct uapp_window *uw)
{
    if (SDL_GetMouseFocus() == data_of(uw)->window) {
        SDL_SetMouseFocus(NULL);
    }
}

static void on_wheel(struct uapp_window *uw, int notches)
{
    SDL_SendMouseWheel(0, data_of(uw)->window, SDL_DEFAULT_MOUSE_ID, 0.0f, (float)notches,
                       SDL_MOUSEWHEEL_NORMAL);
}

static void on_phys_key(struct uapp_window *uw, int keycode, int down, unsigned mods)
{
    (void)uw; (void)mods;
    SDL_Scancode sc = SDL_SCANCODE_UNKNOWN;
    if (keycode >= 0 && keycode < (int)SDL_arraysize(linux_scancode_table)) {
        sc = linux_scancode_table[keycode];
    }
    SDL_SendKeyboardKey(0, SDL_DEFAULT_KEYBOARD_ID, keycode, sc, down != 0);
}

/* The typed character, as UTF-8 text. Control characters (Ctrl+letter
 * arrives as 0x01..0x1a) and the special keys (arrows, F-keys) are not
 * text. */
static void on_key(struct uapp_window *uw, int key, unsigned mods)
{
    (void)mods;
    SDL_Window *window = data_of(uw)->window;
    if (key < 0x20 || key == 0x7f || IS_SPECIAL_KEY(key) || key > 0x10ffff ||
        (key >= 0xd800 && key <= 0xdfff) || !SDL_TextInputActive(window)) {
        return;
    }
    char utf8[5];
    *SDL_UCS4ToUTF8((Uint32)key, utf8) = '\0';   /* it writes no terminator */
    SDL_SendKeyboardText(utf8);
}

/* Focus lost: SDL releases every key it thinks is down, which is what
 * uapp's on_focus(0) means (no releases come for keys still held). */
static void on_focus(struct uapp_window *uw, int focused)
{
    SDL_Window *window = data_of(uw)->window;
    if (focused) {
        SDL_SetKeyboardFocus(window);
    } else if (SDL_GetKeyboardFocus() == window) {
        SDL_SetKeyboardFocus(NULL);
    }
}

/* The person resized it, or it went fullscreen: SDL hears the new size
 * in points, and makes a new framebuffer for it. */
static void on_resize(struct uapp_window *uw, int width, int height)
{
    float s = SAUNAOS_Scale();
    int w = (int)SDL_lroundf(width / s), h = (int)SDL_lroundf(height / s);
    SDL_SendWindowEvent(data_of(uw)->window, SDL_EVENT_WINDOW_RESIZED, w > 0 ? w : 1, h > 0 ? h : 1);
}

/* The X, Alt+F4, the window menu: a REQUEST. The program decides, as on
 * every platform -- SDL turns the last window's into SDL_EVENT_QUIT. */
static void on_close(struct uapp_window *uw)
{
    SDL_SendWindowEvent(data_of(uw)->window, SDL_EVENT_WINDOW_CLOSE_REQUESTED, 0, 0);
}

void SAUNAOS_SetInputCallbacks(struct uapp_window_desc *desc)
{
    desc->on_motion = on_motion;
    desc->on_press = on_press;
    desc->on_release = on_release;
    desc->on_button = on_button;
    desc->on_leave = on_leave;
    desc->on_wheel = on_wheel;
    desc->on_phys_key = on_phys_key;
    desc->on_key = on_key;
    desc->on_focus = on_focus;
    desc->on_resize = on_resize;
    desc->on_close = on_close;
}

/* --- cursors: the desktop's own shapes, or none ----------------------- */

static SDL_Cursor *make_cursor(int shape)
{
    SDL_Cursor *c = (SDL_Cursor *)SDL_calloc(1, sizeof(*c));
    if (c) {
        c->internal = (SDL_CursorData *)(uintptr_t)(shape + 1);   /* never NULL */
    }
    return c;
}

static SDL_Cursor *SAUNAOS_CreateSystemCursor(SDL_SystemCursor id)
{
    int shape;
    switch (id) {
    case SDL_SYSTEM_CURSOR_TEXT: shape = WIN_CURSOR_TEXT; break;
    case SDL_SYSTEM_CURSOR_WAIT:
    case SDL_SYSTEM_CURSOR_PROGRESS: shape = WIN_CURSOR_WAIT; break;
    case SDL_SYSTEM_CURSOR_CROSSHAIR: shape = WIN_CURSOR_CROSSHAIR; break;
    case SDL_SYSTEM_CURSOR_NWSE_RESIZE:
    case SDL_SYSTEM_CURSOR_NW_RESIZE:
    case SDL_SYSTEM_CURSOR_SE_RESIZE: shape = WIN_CURSOR_RESIZE_NWSE; break;
    case SDL_SYSTEM_CURSOR_NESW_RESIZE:
    case SDL_SYSTEM_CURSOR_NE_RESIZE:
    case SDL_SYSTEM_CURSOR_SW_RESIZE: shape = WIN_CURSOR_RESIZE_NESW; break;
    case SDL_SYSTEM_CURSOR_EW_RESIZE:
    case SDL_SYSTEM_CURSOR_E_RESIZE:
    case SDL_SYSTEM_CURSOR_W_RESIZE: shape = WIN_CURSOR_RESIZE_H; break;
    case SDL_SYSTEM_CURSOR_NS_RESIZE:
    case SDL_SYSTEM_CURSOR_N_RESIZE:
    case SDL_SYSTEM_CURSOR_S_RESIZE: shape = WIN_CURSOR_RESIZE_V; break;
    case SDL_SYSTEM_CURSOR_MOVE: shape = WIN_CURSOR_MOVE; break;
    case SDL_SYSTEM_CURSOR_NOT_ALLOWED: shape = WIN_CURSOR_NOT_ALLOWED; break;
    case SDL_SYSTEM_CURSOR_POINTER: shape = WIN_CURSOR_HAND; break;
    default: shape = WIN_CURSOR_DEFAULT; break;
    }
    return make_cursor(shape);
}

int SAUNAOS_CursorShape(void)
{
    SDL_Cursor *c = SDL_GetCursor();
    if (!SDL_CursorVisible() || !c || !c->internal) {
        return SDL_CursorVisible() ? WIN_CURSOR_DEFAULT : WIN_CURSOR_NONE;
    }
    return (int)(uintptr_t)c->internal - 1;
}

static void SAUNAOS_FreeCursor(SDL_Cursor *cursor)
{
    SDL_free(cursor);
}

/* NULL hides it (SDL_HideCursor). The shape is the window's, so every
 * SDL window gets it. */
static bool SAUNAOS_ShowCursor(SDL_Cursor *cursor)
{
    int shape = cursor ? (int)(uintptr_t)cursor->internal - 1 : WIN_CURSOR_NONE;
    SDL_VideoDevice *dev = SDL_GetVideoDevice();
    for (SDL_Window *w = dev ? dev->windows : NULL; w; w = w->next) {
        SDL_WindowData *data = w->internal;
        if (data && data->uw) {
            uapp_window_set_cursor(data->uw, shape);
        }
    }
    return true;
}

/* SDL's auto-capture while a button is held: the desktop already keeps
 * sending this window the pointer, past its edges too, until every button
 * is up (the implicit grab) -- so a drag off the edge is not a leave. A
 * capture with nothing held the desktop has no way to give. */
static bool SAUNAOS_CaptureMouse(SDL_Window *window)
{
    if (window && !SDL_GetMouseState(NULL, NULL)) {
        return SDL_Unsupported();
    }
    return true;
}

void SAUNAOS_InitInput(void)
{
    SDL_AddKeyboard(SDL_DEFAULT_KEYBOARD_ID, NULL);
    SDL_AddMouse(SDL_DEFAULT_MOUSE_ID, NULL);
    SDL_Mouse *mouse = SDL_GetMouse();
    mouse->CreateSystemCursor = SAUNAOS_CreateSystemCursor;
    mouse->ShowCursor = SAUNAOS_ShowCursor;
    mouse->FreeCursor = SAUNAOS_FreeCursor;
    mouse->CaptureMouse = SAUNAOS_CaptureMouse;
    SDL_SetDefaultCursor(make_cursor(WIN_CURSOR_DEFAULT));
}

#endif /* SDL_VIDEO_DRIVER_PRIVATE */
