// uixx/controls.hpp -- ui::Label and ui::Button, the C toolkit's
// uui_label and uui_button held by value, with the text they point at
// owned by the object (the C structs keep a caller-owned pointer).
#ifndef UIXX_CONTROLS_HPP
#define UIXX_CONTROLS_HPP

#include <functional>
#include <string>
#include <utility>

#include "ui/utheme.h"
#include "ui/uui_button.h"
#include "ui/uui_label.h"
#include "uixx/widget.hpp"

namespace ui {

class Label : public Item {
public:
    explicit Label(std::string text = {}) : Item(&uui_label_ops, &c_), text_(std::move(text)) {
        uui_label_init(&c_, text_.c_str());
    }

    const std::string &text() const { return text_; }
    void set_text(std::string text) {
        text_ = std::move(text);
        uui_label_set_text(&c_, text_.c_str());
        request_redraw();
    }
    // Secondary text, in the theme's dim colour.
    void set_dim(bool on) {
        c_.dim = on;
        request_redraw();
    }
    // Wrap to at most `rows` lines (0: one line, never wrapped).
    void set_wrap(int rows) {
        uui_label_set_wrap(&c_, rows);
        request_redraw();
    }

private:
    uui_label c_{};
    std::string text_;
};

class Button : public Item {
public:
    explicit Button(std::string label) : Item(&uui_button_ops, &c_), label_(std::move(label)) {
        uui_button_init(&c_, 0, 0, 0, 0, label_.c_str(), UUI_ROLE(control_bg), UUI_ROLE(text), 0);
    }

    // ONE handler: a second call replaces the first. A click commits on
    // RELEASE over the same button (docs/gui-guidelines.md), and Space or
    // Enter on a focused button commits too; both arrive here.
    void on_click(std::function<void()> fn) { click_ = std::move(fn); }

    const std::string &label() const { return label_; }
    void set_label(std::string label) {
        label_ = std::move(label);
        c_.label = label_.c_str();
        request_redraw();
    }
    bool enabled() const { return !c_.disabled; }
    void set_enabled(bool on) {
        c_.disabled = !on;
        request_redraw();
    }
    void set_outlined(bool on) {
        c_.outlined = on;
        request_redraw();
    }
    // The dialog's default button: drawn in the accent, answers Enter.
    void set_default(bool on) {
        uui_button_set_default(&c_, on);
        request_redraw();
    }

private:
    // A button's commit code IS its item id, so on_action finds it.
    void placed(int id) override { c_.code = id; }
    void action(int) override {
        if (click_) click_();
    }

    uui_button c_{};
    std::string label_;
    std::function<void()> click_;
};

}  // namespace ui

#endif
