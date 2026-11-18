// uixx/tabs.hpp -- ui::Tabs, a notebook: a strip of tabs over pages, one
// page shown (QTabWidget's and GtkNotebook's shape), over uui_tabs and a
// uui_stack sized to the largest page.
//
//     tabs_.add("General", ui::Column{name_, hidden_});
//     tabs_.add("Advanced", advanced_box_);
//     set_root(ui::Column{tabs_.fill()});
//
// PAGES ARE ADDED BEFORE set_root(), which hands the arrays to C. A page
// written in place is owned; a named one must outlive the window. The Tab
// ring skips the controls of a hidden page. on_change hears a tab the
// person chose, not set_selected() (ui/values.hpp's rule).
#ifndef UIXX_TABS_HPP
#define UIXX_TABS_HPP

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "ui/uui_layout.h"
#include "ui/uui_stack.h"
#include "ui/uui_tabs.h"
#include "uixx/box.hpp"
#include "uixx/values.hpp"

namespace ui {

class Tabs : public Container {
public:
    Tabs() : Container(&uui_layout_ops, &c_, 2) {
        c_.dir = UUI_COLUMN;
        strip_.stack_ = &stack_;
        strip_.fill_w();
        stack_.fill();
        add(strip_);
        add(stack_);
    }

    void add(std::string label, Box &page) {
        strip_.add_tab(std::move(label));
        stack_.add_page(page);
    }
    void add(std::string label, Box &&page) {
        strip_.add_tab(std::move(label));
        stack_.add_page(std::move(page));
    }

    int count() const { return strip_.count(); }
    int selected() const { return strip_.selected(); }
    void set_selected(int index) { strip_.select(index); }
    void on_change(std::function<void(const int &)> fn) { strip_.on_change(std::move(fn)); }

private:
    using Container::add;

    class Pages : public Container {
    public:
        Pages() : Container(&uui_stack_ops, &c_, 0) {}
        bool pages() const override { return true; }
        void add_page(Box &page) { Container::add(page); }
        void add_page(Box &&page) { Container::add(std::move(page)); }
        void show(int index) {
            uui_stack_set(&c_, index);
            request_redraw();
        }

    private:
        void placed_items() override {
            int current = c_.current;
            uui_stack_init(&c_, items_.data(), static_cast<int>(items_.size()));
            uui_stack_set(&c_, current);
        }
        uui_stack c_{};
    };

    class Strip : public Valued<int> {
    public:
        Strip() : Valued(&uui_tabs_ops, &c_) {
            uui_tabs_init(&c_, nullptr, 0, this);
            c_.on_select = [](void *ctx, int index) { static_cast<Strip *>(ctx)->stack_->show(index); };
            settled();
        }
        void add_tab(std::string label) {
            labels_.push_back(std::move(label));
            tabs_.assign(labels_.size(), uui_tab{});
            for (size_t i = 0; i < labels_.size(); i++) tabs_[i].label = labels_[i].c_str();
            c_.tabs = tabs_.data();
            c_.count = static_cast<int>(tabs_.size());
        }
        int count() const { return c_.count; }
        int selected() const { return c_.selected; }
        void select(int index) {
            uui_tabs_select(&c_, index);   // on_select shows the page
            settled();
            request_redraw();
        }
        Pages *stack_ = nullptr;

    private:
        int current() const override { return c_.selected; }
        uui_tabs c_{};
        std::vector<std::string> labels_;
        std::vector<uui_tab> tabs_;
    };

    void placed_items() override {
        c_.items = items_.data();
        c_.count = static_cast<int>(items_.size());
    }

    uui_layout c_{};
    Strip strip_;
    Pages stack_;
};

}  // namespace ui

#endif
