// uixx/widget.hpp -- the C++ layer's base classes: ui::Item, anything a
// layout can hold, and ui::Widget, a widget written in C++ by overriding
// virtuals (gtkmm's Gtk::Widget::on_draw shape). The C toolkit stays the
// implementation: an Item IS a `struct uui_item` the C router walks, and
// a Widget is one shared `uui_widget_ops` table whose slots call back
// into the object. docs/uixx-design.md.
#ifndef UIXX_WIDGET_HPP
#define UIXX_WIDGET_HPP

#if !defined(__cplusplus) || __cplusplus < 201703L
#error "uixx needs C++17 or later"
#endif

#include <exception>
#include <string>
#include <utility>
#include <vector>

#include "ui/ugfx.h"
#include "ui/uui_widget.h"

struct uapp;

namespace ui {

class Box;
class Container;
class Item;
class Window;

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    bool contains(int px, int py) const { return px >= x && py >= y && px < x + w && py < y + h; }
};

struct Size {
    int w = 0, h = 0;
};

namespace detail {

// What an Item reports to: the Window that holds it.
class Host {
public:
    virtual void redraw() = 0;
    virtual void fail(std::exception_ptr e) = 0;
    virtual uapp *app() const = 0;   // null outside run()
    // An item that animates (a busy bar) asks to be ticked, or stops.
    virtual void animate(Item *it, bool on) = 0;

protected:
    ~Host() = default;
};

// AN EXCEPTION MUST NEVER UNWIND INTO THE C TOOLKIT: its frames have no
// unwind tables (-fno-asynchronous-unwind-tables), so the unwinder would
// call std::terminate there. Every way back into C++ code runs through
// this; the Window keeps the first exception, quits, and rethrows it from
// run().
template <class R, class F>
R guard(Host *h, R fallback, F &&f) noexcept {
    try {
        return f();
    } catch (...) {
        if (!h) std::terminate();
        h->fail(std::current_exception());
        return fallback;
    }
}

template <class F>
void guard_void(Host *h, F &&f) noexcept {
    guard(h, 0, [&] { f(); return 0; });
}

}  // namespace detail

// Anything a layout holds. NOT COPYABLE OR MOVABLE, because the C side
// keeps its address: a placed Item's `struct uui_item` lives in its
// container's array, and the widget pointer in it points into this
// object. Items are members of a Window subclass, which is what keeps
// them where they are.
class Item {
public:
    Item(const Item &) = delete;
    Item &operator=(const Item &) = delete;
    virtual ~Item() = default;

    // Stretch across the container (UUI_FILL_W/UUI_FILL_H).
    Item &fill_w() & { set_flag(UUI_FILL_W, true); return *this; }
    Item &fill_h() & { set_flag(UUI_FILL_H, true); return *this; }
    Item &fill() & { return fill_w().fill_h(); }
    Item &center() & { set_flag(UUI_CENTER, true); return *this; }
    Item &control_height() & { set_flag(UUI_CONTROL_H, true); return *this; }

    // The name in the layout log, which is how a test finds the widget.
    Item &name(std::string n) & {
        name_ = std::move(n);
        item().name = name_.empty() ? nullptr : name_.c_str();
        return *this;
    }

    void show(bool on = true) {
        item().hidden = !on;
        request_redraw();
    }
    void hide() { show(false); }
    bool visible() const { return !item().hidden; }

    // The id the Window gave it, 0 until it is in a Window's tree.
    int id() const { return item().id; }

protected:
    Item(const uui_widget_ops *ops, void *widget) {
        proto_.ops = ops;
        proto_.widget = widget;
    }

    // For Box alone, whose temporaries are moved into their parent BEFORE
    // they are placed: nothing points at an unplaced Item but itself.
    Item(Item &&o) noexcept : proto_(o.proto_), name_(std::move(o.name_)) {
        proto_.name = name_.empty() ? nullptr : name_.c_str();
    }
    void rebind(void *widget) { item().widget = widget; }

    detail::Host *host() const { return host_; }
    void request_redraw() const {
        if (host_) host_->redraw();
    }

    // The Window's ways into an Item: it was given its id, a control
    // committed (uapp_desc.on_action, a button's code), a widget changed
    // (on_widget, with an enum uui_reason), a key reached the window
    // before its own on_key (a menu bar's F10 and shortcuts; true takes
    // it), and a tick while it animates (false: done animating).
    virtual void placed(int id) { (void)id; }
    virtual void action(int code) { (void)code; }
    virtual void changed(int reason) { (void)reason; }
    virtual bool window_key(int key, unsigned mods) { (void)key; (void)mods; return false; }
    virtual bool tick() { return false; }

    // A container's children, for the Window's walk; and whether they are
    // PAGES, one shown at a time (the Tab ring skips a hidden page's).
    virtual std::vector<Item *> *children() { return nullptr; }
    virtual bool pages() const { return false; }

private:
    friend class Container;
    friend class Window;

    uui_item &item() { return slot_ ? *slot_ : proto_; }
    const uui_item &item() const { return slot_ ? *slot_ : proto_; }
    void set_flag(unsigned f, bool on) {
        if (on) item().flags |= f;
        else item().flags &= ~f;
    }

    uui_item proto_{};          // until placed
    uui_item *slot_ = nullptr;  // the container's copy, once placed
    std::string name_;
    detail::Host *host_ = nullptr;
};

// A widget written in C++: override what it does, leave the rest. Every
// virtual has a harmless default -- no size, draws nothing, consumes
// nothing, takes no focus -- as every NULL slot in the C table does.
// Coordinates are content-relative, as the C toolkit's are; rect() is
// where the layout put it.
class Widget : public Item {
public:
    Widget() : Item(&ops(), static_cast<void *>(this)) {}

    const Rect &rect() const { return rect_; }
    bool focused() const { return focused_; }
    void redraw() const { request_redraw(); }

protected:
    virtual Size natural_size() const { return {}; }
    virtual void draw(ugfx_surface *s) const { (void)s; }
    virtual bool hit(int x, int y) const { return rect_.contains(x, y); }
    // A press that returns true takes the pointer grab: every motion and
    // the release then come here, wherever the pointer goes.
    virtual bool press(int x, int y, unsigned mods) { (void)x; (void)y; (void)mods; return false; }
    virtual bool motion(int x, int y, unsigned buttons) { (void)x; (void)y; (void)buttons; return false; }
    virtual bool release(int x, int y) { (void)x; (void)y; return false; }
    virtual bool wheel(int notches) { (void)notches; return false; }
    // Keys come only to a widget that accepts focus, through the
    // Window's Tab ring.
    virtual bool accepts_focus() const { return false; }
    virtual bool key(int key, unsigned mods) { (void)key; (void)mods; return false; }
    virtual void focus_changed(bool on) { (void)on; }

private:
    static Widget *of(void *w) { return static_cast<Widget *>(w); }
    static const Widget *of(const void *w) { return static_cast<const Widget *>(w); }
    static const uui_widget_ops &ops();

    Rect rect_;
    bool focused_ = false;
};

inline const uui_widget_ops &Widget::ops() {
    static const uui_widget_ops table = [] {
        uui_widget_ops o{};
        o.natural_size = [](const void *w, int *ow, int *oh) {
            const Widget *self = of(w);
            Size s = detail::guard(self->host(), Size{}, [&] { return self->natural_size(); });
            *ow = s.w;
            *oh = s.h;
        };
        o.set_geometry = [](void *w, int x, int y, int width, int height) {
            of(w)->rect_ = Rect{x, y, width, height};
        };
        o.bounds = [](const void *w, int *x, int *y, int *ow, int *oh) {
            const Rect &r = of(w)->rect_;
            *x = r.x; *y = r.y; *ow = r.w; *oh = r.h;
        };
        o.draw = [](ugfx_surface *s, const void *w) {
            const Widget *self = of(w);
            detail::guard_void(self->host(), [&] { self->draw(s); });
        };
        o.hit = [](const void *w, int cx, int cy) -> int {
            const Widget *self = of(w);
            return detail::guard(self->host(), false, [&] { return self->hit(cx, cy); });
        };
        o.press = [](void *w, int cx, int cy, unsigned mods) -> int {
            Widget *self = of(w);
            return detail::guard(self->host(), false, [&] { return self->press(cx, cy, mods); });
        };
        o.motion = [](void *w, int cx, int cy, unsigned buttons) -> int {
            Widget *self = of(w);
            return detail::guard(self->host(), false, [&] { return self->motion(cx, cy, buttons); });
        };
        o.release = [](void *w, int cx, int cy) -> int {
            Widget *self = of(w);
            return detail::guard(self->host(), false, [&] { return self->release(cx, cy); });
        };
        o.wheel = [](void *w, int notches) -> int {
            Widget *self = of(w);
            return detail::guard(self->host(), false, [&] { return self->wheel(notches); });
        };
        o.key = [](void *w, int key, unsigned mods) -> int {
            Widget *self = of(w);
            return detail::guard(self->host(), false, [&] { return self->key(key, mods); });
        };
        o.accepts_focus = [](const void *w) -> int {
            const Widget *self = of(w);
            return detail::guard(self->host(), false, [&] { return self->accepts_focus(); });
        };
        o.set_focused = [](void *w, int on) {
            Widget *self = of(w);
            self->focused_ = on != 0;
            detail::guard_void(self->host(), [&] { self->focus_changed(on != 0); });
        };
        return o;
    }();
    return table;
}

}  // namespace ui

#endif
