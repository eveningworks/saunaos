// Widgets in C++ -- one window holding every value widget of
// uixx/values.hpp, each on_change logging what it was handed: a form, as
// an Options page is one. Reset puts every value back with set_*(),
// which reports nothing (on_change is user input only).
//
//     x86_64-saunaos-g++ -std=c++20 -O2 widgets.cpp -o widgets++ -lui -lmedia -lnet -lbase
#include <string>

#include "base/ulog.h"
#include "uixx/app.hpp"

namespace {

class Widgets : public ui::Window {
public:
    Widgets() : Window("Widgets", "widgetsxx") {
        set_root(ui::Column{
            ui::Row{name_label_.center(), name_.fill_w().name("name")}.fill_w(),
            hidden_.name("hidden"),
            ui::Row{dark_label_.center(), dark_.name("dark")},
            ui::Row{size_label_.center(), size_.name("size")},
            ui::Row{quality_label_.center(), quality_.fill_w().name("quality")}.fill_w(),
            recent_.fill().name("recent"),
            progress_.fill_w().name("progress"),
            reset_.name("reset")});
        name_.set_placeholder("Your name");

        name_.on_change([](const std::string &s) { ulogf("widgets: name %s\n", s.c_str()); });
        hidden_.on_change([](bool on) { ulogf("widgets: hidden %d\n", on); });
        dark_.on_change([](bool on) { ulogf("widgets: dark %d\n", on); });
        size_.on_change([](int i) { ulogf("widgets: size %d\n", i); });
        quality_.on_change([this](int i) {
            ulogf("widgets: quality %d\n", i);
            show_quality(i);
        });
        recent_.on_change([](int i) { ulogf("widgets: recent %d\n", i); });
        reset_.on_click([this] { reset(); });

        reset();
        set_resizable(true);
    }

private:
    void show_quality(int i) { progress_.set_value(i * 1000 / (quality_.count() - 1)); }

    // Every value back to its default, through set_*(): no on_change runs.
    void reset() {
        name_.set_text("");
        hidden_.set_checked(false);
        dark_.set_on(false);
        size_.set_selected(1);
        quality_.set_selected(2);
        recent_.set_selected(-1);
        show_quality(quality_.selected());
        ulogf("widgets: reset\n");
    }

    ui::Label name_label_{"Name"}, dark_label_{"Dark mode"}, size_label_{"Size"}, quality_label_{"Quality"};
    ui::Textbox name_;
    ui::Checkbox hidden_{"Show hidden files"};
    ui::Switch dark_;
    ui::Dropdown size_{{"Small", "Medium", "Large"}};
    ui::Slider quality_{{"Draft", "Low", "Normal", "High", "Best"}};
    ui::Listbox recent_{{"notes.txt", "photo.jpg", "report.pdf", "song.ogg"}};
    ui::Progress progress_;
    ui::Button reset_{"Reset"};
};

}  // namespace

int main() {
    return ui::run<Widgets>();
}
