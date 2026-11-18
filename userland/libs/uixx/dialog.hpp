// uixx/dialog.hpp -- ui::message_box, a question in its own small window
// over uui_msgbox_open: it returns at once, and `done` hears which button
// was chosen (its index), or -1 when the box was closed. No nested event
// loop runs inside the call, so nothing re-enters the window underneath
// (GTK4's shape; it removed gtk_dialog_run for that reason).
//
//     ui::message_box(*this, {"Save changes?", "notes.txt was changed.",
//                             ui::MessageKind::Warning, {"Save", "Don't Save", "Cancel"}},
//                     [this](int answer) { if (answer == 0) save(); });
//
// The FIRST button answers Enter and the LAST answers Esc. One box at a
// time per program: a second open returns false, and its `done` never runs.
#ifndef UIXX_DIALOG_HPP
#define UIXX_DIALOG_HPP

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ui/uui_msgbox.h"
#include "uixx/window.hpp"

namespace ui {

enum class MessageKind { Plain = UUI_MSGBOX_PLAIN, Info = UUI_MSGBOX_INFO,
                         Warning = UUI_MSGBOX_WARNING, Error = UUI_MSGBOX_ERROR };

struct Message {
    std::string title, text;
    MessageKind kind = MessageKind::Info;
    std::vector<std::string> buttons{"OK"};
};

inline bool message_box(Window &w, const Message &m, std::function<void(int)> done = {}) {
    if (!w.app_ || m.buttons.empty()) return false;
    std::vector<uui_msgbox_button> buttons(m.buttons.size());
    for (size_t i = 0; i < buttons.size(); i++) {
        buttons[i].label = m.buttons[i].c_str();
        buttons[i].code = static_cast<int>(i);
        if (i == 0) buttons[i].flags |= UUI_MSGBOX_RETURN;
        if (i + 1 == buttons.size()) buttons[i].flags |= UUI_MSGBOX_ESCAPE;
    }
    uui_msgbox box{};
    box.title = m.title.c_str();
    box.message = m.text.c_str();
    box.kind = static_cast<int>(m.kind);
    box.buttons = buttons.data();
    box.count = static_cast<int>(buttons.size());

    struct Pending {
        Window *w;
        std::function<void(int)> done;
    };
    auto pending = std::make_unique<Pending>(Pending{&w, std::move(done)});
    // uui_msgbox_open copies `box`; the answer comes later, on the loop.
    auto answered = [](void *ctx, int code) {
        std::unique_ptr<Pending> p(static_cast<Pending *>(ctx));
        Window &win = *p->w;
        if (p->done) detail::guard_void(&win, [&] { p->done(code == UUI_MSGBOX_CLOSED ? -1 : code); });
        win.redraw();
    };
    if (!uui_msgbox_open(w.app_, &box, answered, pending.get())) return false;
    pending.release();
    return true;
}

}  // namespace ui

#endif
