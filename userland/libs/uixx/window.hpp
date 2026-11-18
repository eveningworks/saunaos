// uixx/window.hpp -- ui::Window, one uapp window whose widgets are its
// subclass's MEMBERS, and ui::run<W>(), which opens it.
//
//     struct Hello : ui::Window {
//         ui::Label  text{"Hello"};
//         ui::Button ok{"OK"};
//         Hello() : Window{"Hello", "hello"} {
//             set_root(ui::Column{text, ok});
//             ok.on_click([this] { quit(); });
//         }
//     };
//     int main() { return ui::run<Hello>(); }
//
// set_root() walks the tree once: it gives every item an id (uapp
// routes a click back by it), and builds the Tab ring from every widget
// that takes keys, in tree order -- the ring a C app declares by hand.
#ifndef UIXX_WINDOW_HPP
#define UIXX_WINDOW_HPP

#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ui/uapp.h"
#include "ui/ugfx.h"
#include "ui/uui_focus.h"
#include "uixx/box.hpp"
#include "uixx/widget.hpp"

namespace ui {

class Window : private detail::Host {
public:
    Window(std::string title, std::string app_id) : title_(std::move(title)), app_id_(std::move(app_id)) {}
    Window(const Window &) = delete;
    Window &operator=(const Window &) = delete;
    virtual ~Window() = default;

    // The widget tree. A box written in place is owned by the Window; a
    // named one must outlive it, as every child does.
    void set_root(Box &&root) {
        owned_root_ = std::make_unique<Box>(std::move(root));
        attach(*owned_root_);
    }
    void set_root(Box &root) {
        owned_root_.reset();
        attach(root);
    }

    void set_resizable(bool on) {
        if (on) flags_ |= UAPP_RESIZABLE;
        else flags_ &= ~UAPP_RESIZABLE;
    }
    // The content size to open at; 0 is the layout's natural size.
    void set_size(int w, int h) { w_ = w; h_ = h; }

    // Repaint soon (coalesced). A widget's setters ask for this already.
    void redraw() override {
        if (app_) uapp_redraw(app_);
    }
    void quit(int status = 0) {
        if (app_) uapp_quit(app_, status);
    }

    // The C handle, for a uapp_* call this layer does not wrap. Null
    // outside run().
    uapp *native() const { return app_; }

    // Runs until the window closes; the exit status quit() set. An
    // exception a handler threw ends the loop and is rethrown here.
    int run();

protected:
    // The window exists and is about to draw for the first time.
    virtual void on_open() {}
    // A key no focused widget took (a lone button's Space/Enter is spent
    // on the click and does not come here).
    virtual void on_key(int key, unsigned mods) { (void)key; (void)mods; }

private:
    void fail(std::exception_ptr e) override {
        if (!error_) error_ = std::move(e);
        quit(1);
    }

    void attach(Box &root) {
        root_ = &root;
        by_id_.clear();
        ring_.clear();
        walk(root);
        focus_ = uui_focus{};
        if (!ring_.empty()) uui_focus_init(&focus_, ring_.data(), static_cast<int>(ring_.size()));
    }
    void walk(Box &box) {
        for (Item *kid : box.kids_) {
            kid->host_ = this;
            by_id_.push_back(kid);
            int id = static_cast<int>(by_id_.size());
            kid->item().id = id;
            kid->placed(id);
            const uui_item &it = kid->item();
            if (it.ops->key) ring_.push_back(uui_focusable{it.widget, it.ops});
            if (it.ops == &uui_layout_ops) walk(*static_cast<Box *>(kid));
        }
    }
    Item *item_by_id(int id) const {
        return id >= 1 && id <= static_cast<int>(by_id_.size()) ? by_id_[id - 1] : nullptr;
    }

    static Window &self(uapp *a) {
        Window &w = *static_cast<Window *>(uapp_state(a));
        w.app_ = a;
        return w;
    }

    std::string title_, app_id_;
    std::unique_ptr<Box> owned_root_;
    Box *root_ = nullptr;
    std::vector<Item *> by_id_;            // id - 1 -> item
    std::vector<uui_focusable> ring_;
    uui_focus focus_{};
    unsigned flags_ = 0;
    int w_ = 0, h_ = 0;
    uapp *app_ = nullptr;
    std::exception_ptr error_;
};

inline int Window::run() {
    uapp_desc d{};
    d.title = title_.c_str();
    d.app_id = app_id_.c_str();
    d.state = this;
    d.flags = flags_;
    d.w = w_;
    d.h = h_;
    if (root_) {
        d.layout = &root_->c_;
        d.widgets = root_->items_.data();
        d.widget_count = static_cast<int>(root_->items_.size());
    }
    if (!ring_.empty()) d.focus = &focus_;
    d.on_open = [](uapp *a) {
        Window &w = self(a);
        detail::guard_void(&w, [&] { w.on_open(); });
    };
    d.on_action = [](uapp *a, int code) {
        Window &w = self(a);
        if (Item *it = w.item_by_id(code)) detail::guard_void(&w, [&] { it->action(code); });
        w.redraw();
    };
    d.on_widget = [](uapp *a, int id, int reason) {
        Window &w = self(a);
        if (Item *it = w.item_by_id(id)) detail::guard_void(&w, [&] { it->changed(reason); });
        w.redraw();
    };
    d.on_key = [](uapp *a, int key, unsigned mods) {
        Window &w = self(a);
        detail::guard_void(&w, [&] { w.on_key(key, mods); });
        w.redraw();
    };
    int rc = uapp_run(&d);
    app_ = nullptr;
    if (error_) std::rethrow_exception(std::exchange(error_, nullptr));
    return rc;
}

// Load the session font, which every widget measures with, then build W
// and run it. A Window built before this (a global) measures nothing.
template <class W, class... Args>
int run(Args &&...args) {
    if (!ugfx_font_init()) return 2;
    W w(std::forward<Args>(args)...);
    return w.run();
}

}  // namespace ui

#endif
