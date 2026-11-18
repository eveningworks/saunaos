// uixx/window.hpp -- ui::Window, one uapp window whose widgets are its
// subclass's MEMBERS, ui::Timer, and ui::run<W>(), which opens it.
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
// that takes keys, in tree order -- the ring a C app declares by hand --
// telling the ring which page of a ui::Tabs each one is on.
#ifndef UIXX_WINDOW_HPP
#define UIXX_WINDOW_HPP

#include <algorithm>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "rt/sys.h"
#include "ui/uapp.h"
#include "ui/ugfx.h"
#include "ui/uui_focus.h"
#include "uixx/box.hpp"
#include "uixx/widget.hpp"

namespace ui {

class Timer;
struct Message;
bool message_box(Window &w, const Message &m, std::function<void(int)> done);

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
    // The content size to open at, in CHARACTER CELLS -- advances of 'n'
    // by lines, so it follows the font (Win32's dialog units). 0 is the
    // layout's natural size. uapp ranks a pixel w/h BELOW a layout, so a
    // window with a tree has no pixel size to set.
    void set_size_cells(int cols, int lines) { cols_ = cols; lines_ = lines; }

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
    // A key no focused widget, menu or shortcut took (a lone button's
    // Space/Enter is spent on the click and does not come here).
    virtual void on_key(int key, unsigned mods) { (void)key; (void)mods; }

private:
    friend class Timer;
    friend bool message_box(Window &, const Message &, std::function<void(int)>);

    // Milliseconds on the toolkit's own clock -- the one uapp's tick runs
    // on, and the one the host harness moves.
    static uint64_t now_ms() { return sys_monotonic_ns() / 1000000ull; }
    static constexpr unsigned kIdleTickMs = 3600000;   // nothing due: an hour
    static constexpr unsigned kAnimateMs = 40;          // a busy bar's step

    void fail(std::exception_ptr e) override {
        if (!error_) error_ = std::move(e);
        quit(1);
    }
    uapp *app() const override { return app_; }
    void animate(Item *it, bool on) override {
        auto at = std::find(animating_.begin(), animating_.end(), it);
        if (on && at == animating_.end()) animating_.push_back(it);
        if (!on && at != animating_.end()) animating_.erase(at);
        rearm();
    }

    void attach(Box &root) {
        root_ = &root;
        by_id_.clear();
        ring_.clear();
        ring_page_.clear();
        walk(root, nullptr);
        focus_ = uui_focus{};
        if (!ring_.empty()) {
            uui_focus_init(&focus_, ring_.data(), static_cast<int>(ring_.size()));
            focus_.page = ring_page_.data();
        }
    }
    void walk(Item &parent, const uui_item *page) {
        std::vector<Item *> *kids = parent.children();
        if (!kids) return;
        for (Item *kid : *kids) {
            kid->host_ = this;
            by_id_.push_back(kid);
            int id = static_cast<int>(by_id_.size());
            kid->item().id = id;
            kid->placed(id);
            // A page's controls are on that page: the ring reads its
            // `hidden` flag, which the page stack sets.
            const uui_item *on = parent.pages() ? &kid->item() : page;
            const uui_item &it = kid->item();
            if (it.ops->key) {
                ring_.push_back(uui_focusable{it.widget, it.ops});
                ring_page_.push_back(on);
            }
            walk(*kid, on);
        }
    }
    Item *item_by_id(int id) const {
        return id >= 1 && id <= static_cast<int>(by_id_.size()) ? by_id_[id - 1] : nullptr;
    }

    // --- timers and animation: one uapp tick, re-armed to the next due ---
    void rearm();
    void tick();

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
    std::vector<const uui_item *> ring_page_;   // parallel to ring_
    uui_focus focus_{};
    std::vector<Timer *> timers_;
    std::vector<Item *> animating_;
    uint64_t anim_due_ = 0;
    unsigned flags_ = 0;
    int cols_ = 0, lines_ = 0;
    unsigned tick_ms_ = kIdleTickMs;
    uapp *app_ = nullptr;
    std::exception_ptr error_;
};

// A callback after a delay, again and again or once, on the window's
// own loop -- never on another thread. A member of the Window it is
// built with; destroying it stops it, so a lambda capturing `this` can
// never run after the object is gone (QTimer with a parent).
class Timer {
public:
    explicit Timer(Window &w) : w_(w) { w_.timers_.push_back(this); }
    Timer(const Timer &) = delete;
    Timer &operator=(const Timer &) = delete;
    ~Timer() {
        auto &v = w_.timers_;
        v.erase(std::remove(v.begin(), v.end(), this), v.end());
        w_.rearm();
    }

    // Every `ms` from now until stop(); a second start() replaces it.
    void start(unsigned ms, std::function<void()> fn) { arm(ms, std::move(fn), false); }
    // Once, `ms` from now.
    void start_once(unsigned ms, std::function<void()> fn) { arm(ms, std::move(fn), true); }
    void stop() {
        active_ = false;
        w_.rearm();
    }
    bool active() const { return active_; }

private:
    friend class Window;
    void arm(unsigned ms, std::function<void()> fn, bool once) {
        fn_ = std::move(fn);
        every_ = ms ? ms : 1;
        once_ = once;
        due_ = Window::now_ms() + every_;
        active_ = true;
        w_.rearm();
    }

    Window &w_;
    std::function<void()> fn_;
    uint64_t due_ = 0;
    unsigned every_ = 0;
    bool once_ = false, active_ = false;
};

inline void Window::rearm() {
    uint64_t now = now_ms(), next = UINT64_MAX;
    for (const Timer *t : timers_)
        if (t->active_) next = std::min(next, t->due_);
    if (!animating_.empty()) {
        if (anim_due_ <= now) anim_due_ = now + kAnimateMs;
        next = std::min(next, anim_due_);
    }
    uint64_t ms = next == UINT64_MAX ? kIdleTickMs : (next > now ? next - now : 1);
    tick_ms_ = static_cast<unsigned>(std::min<uint64_t>(ms, kIdleTickMs));
    if (app_) uapp_set_tick(app_, tick_ms_);
}

inline void Window::tick() {
    uint64_t now = now_ms();
    // A copy: a callback may start or stop timers, or destroy one.
    std::vector<Timer *> due;
    for (Timer *t : timers_)
        if (t->active_ && t->due_ <= now) due.push_back(t);
    for (Timer *t : due) {
        if (std::find(timers_.begin(), timers_.end(), t) == timers_.end() || !t->active_) continue;
        if (t->once_) t->active_ = false;
        else t->due_ = std::max(t->due_ + t->every_, now + 1);
        std::function<void()> fn = t->fn_;
        detail::guard_void(this, [&] { fn(); });
    }
    if (!animating_.empty() && anim_due_ <= now) {
        anim_due_ = now + kAnimateMs;
        std::vector<Item *> still;
        for (Item *it : animating_)
            if (detail::guard(this, false, [&] { return it->tick(); })) still.push_back(it);
        animating_ = std::move(still);
    }
    rearm();
}

inline int Window::run() {
    uapp_desc d{};
    d.title = title_.c_str();
    d.app_id = app_id_.c_str();
    d.state = this;
    d.flags = flags_;
    d.cells_w = cols_;
    d.cells_h = lines_;
    if (root_) {
        d.layout = &root_->c_;
        d.widgets = root_->items_.data();
        d.widget_count = static_cast<int>(root_->items_.size());
    }
    if (!ring_.empty()) d.focus = &focus_;
    rearm();
    d.tick_ms = tick_ms_;
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
        bool taken = false;
        for (Item *it : w.by_id_)
            if (!taken) taken = detail::guard(&w, false, [&] { return it->window_key(key, mods); });
        if (!taken) detail::guard_void(&w, [&] { w.on_key(key, mods); });
        w.redraw();
    };
    d.on_tick = [](uapp *a) -> int {
        Window &w = self(a);
        w.tick();
        return 1;
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
