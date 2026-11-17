/* SDL_ShowMessageBox on SaunaOS: ui/uui_msgbox.h, a window of its own,
 * modal over the SDL window it names. Reached two ways: the video
 * device's hook while the driver is up, and the bootstrap's -- "can be
 * done without initializing backend" -- which opens a uapp of its own
 * for the length of the box. */
#include "SDL_internal.h"

#ifdef SDL_VIDEO_DRIVER_PRIVATE

#include "SDL_saunaos_video.h"
#include "ui/uapp.h"
#include "ui/uui_msgbox.h"

static bool show(struct uapp *a, const SDL_MessageBoxData *data, int *buttonID)
{
    if (uui_msgbox_is_open()) {
        return SDL_SetError("SaunaOS shows one message box at a time");
    }
    struct uui_msgbox_button buttons[UUI_MSGBOX_MAX_BUTTONS];
    int n = data->numbuttons;
    if (n > UUI_MSGBOX_MAX_BUTTONS) {
        return SDL_SetError("A message box has at most %d buttons", UUI_MSGBOX_MAX_BUTTONS);
    }
    bool rtl = (data->flags & SDL_MESSAGEBOX_BUTTONS_RIGHT_TO_LEFT) != 0;
    for (int i = 0; i < n; i++) {
        const SDL_MessageBoxButtonData *b = &data->buttons[rtl ? n - 1 - i : i];
        buttons[i].label = b->text;
        buttons[i].code = b->buttonID;
        buttons[i].flags = ((b->flags & SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT) ? UUI_MSGBOX_RETURN : 0) |
                           ((b->flags & SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT) ? UUI_MSGBOX_ESCAPE : 0);
    }
    if (n == 0) {   /* nothing to answer with: OK, which is a close */
        buttons[0] = (struct uui_msgbox_button){ "OK", -1, UUI_MSGBOX_RETURN | UUI_MSGBOX_ESCAPE };
        n = 1;
    }
    struct uui_msgbox m = {
        .title = data->title,
        .message = data->message,
        .kind = (data->flags & SDL_MESSAGEBOX_ERROR)         ? UUI_MSGBOX_ERROR
              : (data->flags & SDL_MESSAGEBOX_WARNING)       ? UUI_MSGBOX_WARNING
              : (data->flags & SDL_MESSAGEBOX_INFORMATION)   ? UUI_MSGBOX_INFO
              : UUI_MSGBOX_PLAIN,
        .buttons = buttons,
        .count = n,
        .owner = data->window && data->window->internal ? data->window->internal->uw : NULL,
    };
    int code;
    if (!uui_msgbox_run(a, &m, &code)) {
        return SDL_SetError("SaunaOS: the message box could not be shown");
    }
    *buttonID = code == UUI_MSGBOX_CLOSED ? -1 : code;
    return true;
}

bool SAUNAOS_ShowMessageBox(SDL_VideoDevice *_this, const SDL_MessageBoxData *data, int *buttonID)
{
    (void)_this;
    /* Its wait pumps the app, which must not happen inside a pump -- an
     * event watcher calling this (SAUNAOS_Pump's rule). */
    if (SAUNAOS_InPump()) {
        return SDL_SetError("SaunaOS: no message box from inside event handling");
    }
    return show(SAUNAOS_app, data, buttonID);
}

/* Before SDL_Init(VIDEO), or with another driver up: an app of our own,
 * no main window, for as long as the box is up. */
bool SAUNAOS_ShowMessageBoxAlone(const SDL_MessageBoxData *data, int *buttonID)
{
    if (SAUNAOS_app) {
        return SAUNAOS_ShowMessageBox(NULL, data, buttonID);
    }
    if (!uapp_has_desktop()) {
        return SDL_SetError("SaunaOS: no desktop to show a message box on");
    }
    static struct uapp_desc desc;
    desc.title = data->title ? data->title : "";
    desc.flags = UAPP_NO_TOPLEVEL | UAPP_NO_SETLOCALE;
    struct uapp *a = NULL;
    if (!uapp_open(&a, &desc)) {
        return SDL_SetError("SaunaOS: no desktop to show a message box on");
    }
    bool ok = show(a, data, buttonID);
    uapp_close(a);
    return ok;
}

#endif /* SDL_VIDEO_DRIVER_PRIVATE */
