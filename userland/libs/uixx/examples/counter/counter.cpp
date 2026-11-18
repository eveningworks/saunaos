// Counter in C++ -- userland/apps/demos/counter/counter.c rewritten over
// the C++ layer (uixx/app.hpp), the worked example of
// docs/gui-app-tutorial.md's C++ section: a label, a bar drawn by a
// ui::Widget subclass, and two buttons in a row. Same window, same log
// line, same keys.
//
//     x86_64-saunaos-g++ -std=c++20 -O2 counter.cpp -o counter++ -lui -lmedia -lnet -lbase
#include <string>

#include "base/ulog.h"
#include "ui/ugfx.h"
#include "ui/utheme.h"
#include "uixx/app.hpp"

namespace {

constexpr int kGoal = 10;  // the bar is full at this count

// An outlined track, filled in the accent in proportion to the count --
// in the widget's own rect, wherever the layout put it.
class Bar : public ui::Widget {
public:
    void set_count(int n) {
        count_ = n;
        redraw();
    }

protected:
    // Font-derived, never a pixel constant.
    ui::Size natural_size() const override {
        return {ugfx_text_width("Clicked 10 times") * 2, ugfx_char_h() * 2};
    }
    void draw(ugfx_surface *s) const override {
        const ui::Rect &r = rect();
        ugfx_fill_rect(s, r.x, r.y, r.w, r.h, UTHEME_OUTLINE);
        ugfx_fill_rect(s, r.x + 1, r.y + 1, r.w - 2, r.h - 2, UTHEME_PANEL_BG);
        int n = count_ > kGoal ? kGoal : count_;
        int fill = (r.w - 4) * n / kGoal;
        if (fill > 0) ugfx_fill_rect(s, r.x + 2, r.y + 2, fill, r.h - 4, UTHEME_ACCENT);
    }

private:
    int count_ = 0;
};

class Counter : public ui::Window {
public:
    Counter() : Window("Counter", "counterxx") {
        add_.set_outlined(true);
        reset_.set_outlined(true);
        set_root(ui::Column{label_.name("label"), bar_.fill().name("bar"),
                            ui::Row{add_.name("add"), reset_.name("reset")}.name("row")});
        add_.on_click([this] { set_count(count_ + 1); });
        reset_.on_click([this] { set_count(0); });
        set_resizable(true);
    }

protected:
    void on_key(int key, unsigned) override {
        if (key == '+' || key == ' ') set_count(count_ + 1);
        else if (key == 'r' || key == 'R') set_count(0);
    }

private:
    void set_count(int n) {
        count_ = n;
        label_.set_text("Clicked " + std::to_string(n) + (n == 1 ? " time" : " times"));
        bar_.set_count(n);
        ulogf("counter: count %d\n", n);  // what a test reads
    }

    int count_ = 0;
    ui::Label label_{"Clicked 0 times"};
    Bar bar_;
    ui::Button add_{"+1"}, reset_{"Reset"};
};

}  // namespace

int main() {
    return ui::run<Counter>();
}
