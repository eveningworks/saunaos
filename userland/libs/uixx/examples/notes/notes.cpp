// Notes in C++ -- uixx's stage 3 in one window: a menu bar whose
// shortcuts are bound from their own text, a notebook of two pages, a
// timer, a busy bar, and an About box. Every action logs a `notes:` line.
//
//     x86_64-saunaos-g++ -std=c++20 -O2 notes.cpp -o notes++ -lui -lmedia -lnet -lbase
#include <string>

#include "base/ulog.h"
#include "uixx/app.hpp"

namespace {

class Notes : public ui::Window {
public:
    Notes() : Window("Notes", "notesxx") {
        tabs_.add("Note", ui::Column{text_.fill_w().name("text")}.fill());
        tabs_.add("Clock", ui::Column{clock_.name("clock"), progress_.fill_w().name("progress")});
        tabs_.on_change([](int i) { ulogf("notes: tab %d\n", i); });
        set_root(ui::Column{menu_.fill_w().name("menu"), tabs_.fill().name("tabs")});
        text_.set_placeholder("Write something");
        set_size_cells(40, 10);
        set_resizable(true);
    }

private:
    void clear() {
        text_.set_text("");
        ulogf("notes: cleared\n");
    }
    void set_clock(bool on) {
        if (on) clock_timer_.start(1000, [this] { tick(); });
        else clock_timer_.stop();
        ulogf("notes: clock %d\n", on);
    }
    void tick() {
        seconds_++;
        clock_.set_text("Running for " + std::to_string(seconds_) + " s");
        ulogf("notes: tick %d\n", seconds_);
    }
    void set_busy(bool on) {
        if (on) progress_.set_busy();
        else progress_.set_value(0);
        ulogf("notes: busy %d\n", on);
    }
    void about() {
        bool shown = ui::message_box(*this, {"About Notes", "A C++ program over uixx.", ui::MessageKind::Info, {"OK"}},
                                     [](int answer) { ulogf("notes: about answered %d\n", answer); });
        ulogf("notes: about shown %d\n", shown);
    }

    ui::Textbox text_;
    ui::Label clock_{"Clock stopped"};
    ui::Progress progress_;
    ui::Tabs tabs_;
    ui::Timer clock_timer_{*this};
    int seconds_ = 0;

    ui::MenuBar menu_{{
        ui::Menu{"File", {
            ui::MenuItem{"Clear", "Ctrl+L", [this] { clear(); }}.enabled_when([this] { return !text_.text().empty(); }),
            ui::Separator{},
            ui::MenuItem{"Quit", "Ctrl+Q", [this] { quit(); }}}},
        ui::Menu{"View", {
            ui::MenuItem{"Note", "Ctrl+1", [this] { tabs_.set_selected(0); }},
            ui::MenuItem{"Clock", "Ctrl+2", [this] { tabs_.set_selected(1); }},
            ui::Separator{},
            ui::MenuCheck{"Run clock", "Ctrl+K", [this] { return clock_timer_.active(); }, [this](bool on) { set_clock(on); }},
            ui::MenuCheck{"Busy", "Ctrl+B", [this] { return progress_.busy(); }, [this](bool on) { set_busy(on); }}}},
        ui::Menu{"Help", {
            ui::MenuItem{"About Notes", "F1", [this] { about(); }}}}}};
};

}  // namespace

int main() {
    return ui::run<Notes>();
}
