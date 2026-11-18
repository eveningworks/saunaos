// uixx/menu.hpp -- ui::MenuBar, a uui_menubar written as a tree in
// place, each entry carrying its own lambdas:
//
//     menu_ = ui::MenuBar{
//         ui::Menu{"File", {
//             ui::MenuItem{"Save", "Ctrl+S", [this] { save(); }}.enabled_when([this] { return dirty_; }),
//             ui::Separator{},
//             ui::MenuItem{"Quit", "Ctrl+Q", [this] { quit(); }}}},
//         ui::Menu{"View", {
//             ui::MenuCheck{"Word wrap", [this] { return wrap_; }, [this](bool on) { set_wrap(on); }}}}};
//
// THE SHORTCUT SHOWN IS THE SHORTCUT BOUND: "Ctrl+S" is parsed by
// keycombo_parse(), the grammar the compositor and System Settings use,
// and the key runs the item -- greyed items too refuse it -- before the
// window's own on_key. `enabled_when` and a check's state are asked each
// time the menu draws, so a command is never offered when it cannot act
// (docs/gui-guidelines.md).
#ifndef UIXX_MENU_HPP
#define UIXX_MENU_HPP

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "keycombo.h"
#include "ui/uapp.h"
#include "ui/uui_menubar.h"
#include "uixx/widget.hpp"

namespace ui {

class MenuBar;

// One entry of a menu: a command, a check, a separator, or a submenu.
// The classes below only construct it.
struct MenuEntry {
    std::string label, shortcut;
    std::function<void()> run;
    std::function<bool()> enabled;
    std::function<bool()> checked;
    std::function<void(bool)> toggle;
    std::vector<MenuEntry> sub;
    bool separator = false;
    keycombo combo{};
};

struct MenuItem : MenuEntry {
    MenuItem(std::string label, std::function<void()> fn) : MenuItem(std::move(label), {}, std::move(fn)) {}
    MenuItem(std::string label, std::string shortcut, std::function<void()> fn) {
        this->label = std::move(label);
        this->shortcut = std::move(shortcut);
        run = std::move(fn);
    }
    // Greyed, and its shortcut refused, while this says false.
    MenuItem &&enabled_when(std::function<bool()> fn) && {
        enabled = std::move(fn);
        return std::move(*this);
    }
};

// A ticked item: `get` says whether it is ticked, a click hands `set` the
// opposite.
struct MenuCheck : MenuEntry {
    MenuCheck(std::string label, std::function<bool()> get, std::function<void(bool)> set)
        : MenuCheck(std::move(label), {}, std::move(get), std::move(set)) {}
    MenuCheck(std::string label, std::string shortcut, std::function<bool()> get, std::function<void(bool)> set) {
        this->label = std::move(label);
        this->shortcut = std::move(shortcut);
        checked = std::move(get);
        toggle = std::move(set);
    }
    MenuCheck &&enabled_when(std::function<bool()> fn) && {
        enabled = std::move(fn);
        return std::move(*this);
    }
};

struct Separator : MenuEntry {
    Separator() { separator = true; }
};

// A title on the bar, or a submenu inside a menu.
struct Menu : MenuEntry {
    Menu(std::string label, std::vector<MenuEntry> entries) {
        this->label = std::move(label);
        sub = std::move(entries);
    }
};

namespace detail {

// EVERY MENU CODE IN THE PROCESS, so the C bar's item_flags -- a plain
// `unsigned (*)(int code)`, with no context pointer -- can find the entry
// and its bar from the code alone. A bar's codes are cleared when it goes.
struct MenuCode {
    MenuBar *bar;
    MenuEntry *entry;
};
inline std::vector<MenuCode> &menu_codes() {
    static std::vector<MenuCode> codes;
    return codes;
}

}  // namespace detail

class MenuBar : public Item {
public:
    explicit MenuBar(std::vector<MenuEntry> menus) : Item(&ops(), &n_), menus_(std::move(menus)) {
        n_.self = this;
        std::vector<uui_menu_item> &top = level(menus_.size());
        build(menus_, top);
        uui_menubar_init(&n_.c, top.data(), static_cast<int>(top.size()));
        n_.c.item_flags = flags;
    }
    ~MenuBar() override {
        for (detail::MenuCode &c : detail::menu_codes())
            if (c.bar == this) c = detail::MenuCode{nullptr, nullptr};
    }

private:
    // The C bar first, so the widget pointer is the bar and a slot handed
    // it can still find this object.
    struct Native {
        uui_menubar c;
        MenuBar *self;
    };

    std::vector<uui_menu_item> &level(size_t n) {
        levels_.push_back(std::make_unique<std::vector<uui_menu_item>>(n));
        return *levels_.back();
    }
    void build(std::vector<MenuEntry> &entries, std::vector<uui_menu_item> &out) {
        for (size_t i = 0; i < entries.size(); i++) {
            MenuEntry &e = entries[i];
            uui_menu_item &m = out[i];
            m = uui_menu_item{};
            if (e.separator) continue;   // a NULL label is a separator
            m.label = e.label.c_str();
            m.accel = e.shortcut.empty() ? nullptr : e.shortcut.c_str();
            if (!e.shortcut.empty()) keycombo_parse(e.shortcut.c_str(), &e.combo);
            if (!e.sub.empty()) {
                std::vector<uui_menu_item> &sub = level(e.sub.size());
                build(e.sub, sub);
                m.sub = sub.data();
                m.sub_count = static_cast<int>(sub.size());
                continue;
            }
            detail::menu_codes().push_back(detail::MenuCode{this, &e});
            m.code = static_cast<int>(detail::menu_codes().size());
        }
    }

    static const detail::MenuCode *find(int code) {
        auto &codes = detail::menu_codes();
        if (code < 1 || code > static_cast<int>(codes.size()) || !codes[code - 1].bar) return nullptr;
        return &codes[code - 1];
    }
    static unsigned flags(int code) {
        const detail::MenuCode *c = find(code);
        if (!c) return 0;
        const MenuEntry &e = *c->entry;
        return detail::guard(c->bar->host(), 0u, [&] {
            unsigned f = 0;
            if (e.enabled && !e.enabled()) f |= UUI_MI_DISABLED;
            if (e.checked && e.checked()) f |= UUI_MI_CHECKED;
            return f;
        });
    }
    static bool can_run(const MenuEntry &e) { return !e.enabled || e.enabled(); }
    void activate(int code) {
        const detail::MenuCode *c = find(code);
        if (!c || c->bar != this || !can_run(*c->entry)) return;
        MenuEntry &e = *c->entry;
        if (e.toggle) e.toggle(!(e.checked && e.checked()));
        else if (e.run) e.run();
    }

    // An open menu is clamped to the window, when it is drawn inside it.
    void fit() {
        if (uapp *a = host() ? host()->app() : nullptr)
            uui_menubar_set_bounds(&n_.c, 0, 0, uapp_width(a), uapp_height(a));
    }

    // A commit, from the mouse or from keys the open menu took.
    void changed(int) override {
        int code = uui_menubar_take_code(&n_.c);
        if (code > 0) activate(code);
    }
    // F10 and the arrows while the bar is shut, then the shortcuts.
    bool window_key(int key, unsigned mods) override {
        fit();
        int code;
        if (uui_menubar_key(&n_.c, key, &code)) {
            if (code > 0) activate(code);
            return true;
        }
        auto &codes = detail::menu_codes();
        for (size_t i = 0; i < codes.size(); i++) {
            if (codes[i].bar != this || !codes[i].entry->combo.key) continue;
            if (!keycombo_matches(&codes[i].entry->combo, key, static_cast<uint8_t>(mods))) continue;
            activate(static_cast<int>(i + 1));
            return true;   // a greyed item's shortcut is spent, not passed on
        }
        return false;
    }

    // uui_menubar's own table, with a press that fits the bounds first.
    static const uui_widget_ops &ops() {
        static const uui_widget_ops table = [] {
            uui_widget_ops o = uui_menubar_ops;
            o.press = [](void *w, int cx, int cy, unsigned mods) -> int {
                static_cast<Native *>(w)->self->fit();
                return uui_menubar_ops.press(w, cx, cy, mods);
            };
            return o;
        }();
        return table;
    }

    Native n_{};
    std::vector<MenuEntry> menus_;
    std::vector<std::unique_ptr<std::vector<uui_menu_item>>> levels_;
};

}  // namespace ui

#endif
