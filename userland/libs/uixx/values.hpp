// uixx/values.hpp -- the widgets that hold a value a person changes:
// ui::Checkbox, ui::Switch, ui::Textbox, ui::Dropdown, ui::Slider and
// ui::Listbox, each with ONE on_change slot, and ui::Progress, which only
// shows one.
//
// ON_CHANGE IS USER INPUT ONLY: a click, drag, key or paste that changed
// the value calls it, with the new value; a set_*() call never does
// (Qt's textEdited, not textChanged), so two handlers that set each
// other's widgets cannot loop. Option lists are strings the widget owns,
// and their value is an index, -1 for none, as the C widgets' are.
#ifndef UIXX_VALUES_HPP
#define UIXX_VALUES_HPP

#include <cstdlib>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "ui/utheme.h"
#include "ui/uui_checkbox.h"
#include "ui/uui_dropdown.h"
#include "ui/uui_listbox.h"
#include "ui/uui_progress.h"
#include "ui/uui_slider.h"
#include "ui/uui_switch.h"
#include "ui/uui_textbox.h"
#include "uixx/widget.hpp"

namespace ui {

namespace detail {

// A list of strings the C side reads as `const char *const *`. The
// pointer array is rebuilt with the strings, which own the bytes.
class Strings {
public:
    explicit Strings(std::vector<std::string> s = {}) { assign(std::move(s)); }
    void assign(std::vector<std::string> s) {
        strs_ = std::move(s);
        ptrs_.clear();
        ptrs_.reserve(strs_.size());
        for (const std::string &x : strs_) ptrs_.push_back(x.c_str());
    }
    const char *const *data() const { return ptrs_.empty() ? nullptr : ptrs_.data(); }
    int count() const { return static_cast<int>(ptrs_.size()); }
    const std::vector<std::string> &strings() const { return strs_; }

private:
    std::vector<std::string> strs_;
    std::vector<const char *> ptrs_;
};

}  // namespace detail

// What every value widget shares: the slot, and the value it last
// reported or was set to. The router tells the Window about ANY input on
// the widget (a press, a hover with tracking, a key); a change is that
// input leaving the value different.
template <class T>
class Valued : public Item {
public:
    void on_change(std::function<void(const T &)> fn) { change_ = std::move(fn); }

protected:
    Valued(const uui_widget_ops *ops, void *widget) : Item(ops, widget) {}
    virtual T current() const = 0;
    // A set_*() records the value it set, so the next input compares
    // against it and the set itself is never reported.
    void settled() { last_ = current(); }

private:
    void changed(int) override {
        T now = current();
        if (now == last_) return;
        last_ = std::move(now);
        if (change_) change_(last_);
    }

    std::function<void(const T &)> change_;
    T last_{};
};

class Checkbox : public Valued<bool> {
public:
    explicit Checkbox(std::string label, bool checked = false)
        : Valued(&uui_checkbox_ops, &c_), label_(std::move(label)) {
        uui_checkbox_init(&c_, 0, 0, 0, label_.c_str(), UUI_COLOR_UNSET, UUI_COLOR_UNSET);
        c_.checked = checked;
        settled();
    }
    bool checked() const { return c_.checked != 0; }
    void set_checked(bool on) {
        c_.checked = on;
        settled();
        request_redraw();
    }
    void set_enabled(bool on) {
        c_.disabled = !on;
        request_redraw();
    }

private:
    bool current() const override { return checked(); }
    uui_checkbox c_{};
    std::string label_;
};

class Switch : public Valued<bool> {
public:
    explicit Switch(bool on = false) : Valued(&uui_switch_ops, &c_) {
        uui_switch_init(&c_, on);
        settled();
    }
    bool on() const { return c_.on != 0; }
    void set_on(bool on) {
        c_.on = on;
        settled();
        request_redraw();
    }
    void set_enabled(bool on) {
        c_.disabled = !on;
        request_redraw();
    }

private:
    bool current() const override { return on(); }
    uui_switch c_{};
};

// One line of text. Holds text of any length: the C field's inline
// buffer is 128 bytes, so a longer text grows it to fit first
// (uui_textbox_set_max). Its natural width is "no preference" -- give it
// fill_w() or a row with room.
class Textbox : public Valued<std::string> {
public:
    explicit Textbox(const std::string &text = {}) : Valued(&uui_textbox_ops, &c_) {
        uui_textbox_init(&c_, "");
        set_text(text);
    }
    ~Textbox() override { std::free(c_.big); }

    std::string text() const { return uui_textbox_text(&c_); }
    void set_text(const std::string &text) {
        fit(text.size());
        uui_textbox_set_text(&c_, text.c_str());
        settled();
        request_redraw();
    }
    // Grey text shown while the field is empty.
    void set_placeholder(std::string text) {
        placeholder_ = std::move(text);
        c_.placeholder = placeholder_.empty() ? nullptr : placeholder_.c_str();
        request_redraw();
    }
    // At most this many bytes may be typed or pasted (0: the default).
    void set_max_length(int bytes) {
        if (bytes > 0) uui_textbox_set_max(&c_, bytes);
    }
    // A password field. A MASKED FIELD NEVER HAS A HEAP BUFFER (ui/uui_textbox.h:
    // its stars are drawn from the inline one), so masking a field that grew
    // one moves its text back inline, cut to fit as typing would cut it.
    void set_masked(bool on) {
        if (on && c_.big) {
            std::string keep = text();
            std::free(c_.big);
            c_.big = nullptr;
            c_.max = 0;
            c_.len = 0;
            c_.buf[0] = '\0';
            c_.masked = 1;
            uui_textbox_set_text(&c_, keep.c_str());
        }
        c_.masked = on;
        request_redraw();
    }
    void set_enabled(bool on) {
        c_.disabled = !on;
        request_redraw();
    }

private:
    std::string current() const override { return text(); }
    void fit(size_t bytes) {
        if (c_.masked) return;   // a masked field stays inline: set_masked()
        int need = static_cast<int>(bytes);
        int cap = c_.max > 0 ? c_.max : UUI_TEXTBOX_MAX - 1;
        if (need > cap) uui_textbox_set_max(&c_, need);
    }

    uui_textbox c_{};
    std::string placeholder_;
};

// The shared half of the three option lists: the strings, and the index.
template <class C>
class Choice : public Valued<int> {
public:
    int count() const { return names_.count(); }
    const std::vector<std::string> &options() const { return names_.strings(); }

protected:
    Choice(const uui_widget_ops *ops, C *c, std::vector<std::string> options)
        : Valued(ops, c), names_(std::move(options)) {}
    detail::Strings names_;
};

class Dropdown : public Choice<uui_dropdown> {
public:
    explicit Dropdown(std::vector<std::string> options, int selected = 0)
        : Choice(&uui_dropdown_ops, &c_, std::move(options)) {
        uui_dropdown_init(&c_, 0, 0, 0, 0, names_.data(), names_.count());
        set_selected(selected);
    }
    int selected() const { return uui_dropdown_selected(&c_); }
    void set_selected(int index) {
        uui_dropdown_set_selected(&c_, index);
        settled();
        request_redraw();
    }
    // New options; the selection goes back to the first (or none).
    void set_options(std::vector<std::string> options) {
        names_.assign(std::move(options));
        uui_dropdown_set_items(&c_, names_.data(), names_.count());
        settled();
        request_redraw();
    }
    void set_enabled(bool on) {
        c_.disabled = !on;
        request_redraw();
    }

private:
    int current() const override { return selected(); }
    uui_dropdown c_{};
};

// A row of named stops, one of them chosen (a quality, a size) -- not a
// continuous range: uui_slider has stops.
class Slider : public Choice<uui_slider> {
public:
    explicit Slider(std::vector<std::string> stops, int selected = 0)
        : Choice(&uui_slider_ops, &c_, std::move(stops)) {
        uui_slider_init(&c_, names_.data(), names_.count());
        set_selected(selected);
    }
    int selected() const { return c_.selected; }
    void set_selected(int index) {
        c_.selected = index >= 0 && index < count() ? index : (count() ? 0 : -1);
        settled();
        request_redraw();
    }
    void set_options(std::vector<std::string> stops) {
        names_.assign(std::move(stops));
        uui_slider_set_options(&c_, names_.data(), names_.count());
        settled();
        request_redraw();
    }
    void set_enabled(bool on) {
        c_.disabled = !on;
        request_redraw();
    }

private:
    int current() const override { return selected(); }
    uui_slider c_{};
};

// Rows of text, one selected, scrolled when they do not fit.
class Listbox : public Choice<uui_listbox> {
public:
    explicit Listbox(std::vector<std::string> rows = {}, int selected = -1)
        : Choice(&uui_listbox_ops, &c_, std::move(rows)) {
        uui_listbox_init(&c_, 0, 0, 0, 0, names_.data(), names_.count());
        set_selected(selected);
    }
    int selected() const { return c_.selected; }
    void set_selected(int index) {
        c_.selected = index >= 0 && index < count() ? index : -1;
        settled();
        request_redraw();
    }
    void set_rows(std::vector<std::string> rows) {
        names_.assign(std::move(rows));
        uui_listbox_set_items(&c_, names_.data(), names_.count());
        settled();
        request_redraw();
    }

private:
    int current() const override { return selected(); }
    uui_listbox c_{};
};

// A bar from 0 to 1000 per mille, or BUSY: a block that slides, for
// work whose total is unknown (Windows' PBS_MARQUEE). The window ticks
// it while it is busy.
class Progress : public Item {
public:
    explicit Progress(int per_mille = 0) : Item(&uui_progress_ops, &c_) {
        uui_progress_init(&c_);
        set_value(per_mille);
    }
    int value() const { return c_.value < 0 ? 0 : c_.value; }
    bool busy() const { return c_.value < 0; }
    void set_value(int per_mille) {
        uui_progress_set(&c_, per_mille < 0 ? 0 : per_mille);
        if (host()) host()->animate(this, false);
        request_redraw();
    }
    void set_busy() {
        uui_progress_set_busy(&c_);
        if (host()) host()->animate(this, true);
        request_redraw();
    }

private:
    void placed(int) override {
        if (busy()) host()->animate(this, true);
    }
    bool tick() override {
        if (!uui_progress_tick(&c_)) return false;
        request_redraw();
        return true;
    }

    uui_progress c_{};
};

}  // namespace ui

#endif
