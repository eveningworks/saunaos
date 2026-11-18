// uixx/box.hpp -- ui::Row and ui::Column, a uui_layout whose item array
// is built from the children named in the constructor.
//
//     ui::Column{label, bar.fill(), ui::Row{ok, cancel}}
//
// A NAMED child (an lvalue) is referenced and must outlive the box -- a
// member of the same Window does. A nested Row or Column written in place
// (an rvalue) is MOVED INTO the box and owned by it. Any other temporary
// is a compile error: a temporary Label would be gone before the first
// draw.
#ifndef UIXX_BOX_HPP
#define UIXX_BOX_HPP

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ui/uui_layout.h"
#include "uixx/widget.hpp"

namespace ui {

// What holds children: the `struct uui_item` array the C side walks,
// the Items behind it, and the boxes it owns. Each child writes through
// to its slot in `items_`, so when the array moves every slot is
// re-pointed -- and a container may only grow before its Window's
// set_root(), which hands the array to C.
class Container : public Item {
public:
    std::vector<Item *> *children() override { return &kids_; }

protected:
    Container(const uui_widget_ops *ops, void *widget, size_t expect) : Item(ops, widget) {
        items_.reserve(expect);
        kids_.reserve(expect);
    }
    // The vectors' buffers move with them, so every child's slot stays.
    Container(Container &&o) noexcept
        : Item(std::move(o)), items_(std::move(o.items_)), kids_(std::move(o.kids_)), owned_(std::move(o.owned_)) {}

    void add(Item &kid) { place(kid); }
    void add(Box &&kid);
    // A temporary that is not a box dies before the first draw.
    void add(Item &&kid) = delete;

    void place(Item &kid) {
        const uui_item *was = items_.data();
        items_.push_back(kid.proto_);
        kids_.push_back(&kid);
        if (items_.data() != was)
            for (size_t i = 0; i < kids_.size(); i++) kids_[i]->slot_ = &items_[i];
        else
            kid.slot_ = &items_.back();
        placed_items();
    }
    // The array may have moved: point the C struct at it again.
    virtual void placed_items() {}

    std::vector<uui_item> items_;
    std::vector<Item *> kids_;
    std::vector<std::unique_ptr<Box>> owned_;
};

class Box : public Container {
public:
    template <class... Kids>
    explicit Box(uui_dir dir, Kids &&...kids) : Container(&uui_layout_ops, &c_, sizeof...(Kids)) {
        c_.dir = dir;
        (add(std::forward<Kids>(kids)), ...);
    }

    Box(Box &&o) noexcept : Container(std::move(o)), c_(o.c_) {
        rebind(&c_);
        placed_items();
    }

    // The fluent setters again, for a box written in place:
    // `ui::Row{a, b}.name("row")` must stay an rvalue so the parent owns it.
    using Item::fill;
    using Item::fill_h;
    using Item::fill_w;
    using Item::name;
    Box &&fill() && { Item::fill(); return std::move(*this); }
    Box &&fill_w() && { Item::fill_w(); return std::move(*this); }
    Box &&fill_h() && { Item::fill_h(); return std::move(*this); }
    Box &&name(std::string n) && { Item::name(std::move(n)); return std::move(*this); }

private:
    friend class Window;

    void placed_items() override {
        c_.items = items_.data();
        c_.count = static_cast<int>(items_.size());
    }

    uui_layout c_{};
};

inline void Container::add(Box &&kid) {
    owned_.push_back(std::make_unique<Box>(std::move(kid)));
    place(*owned_.back());
}

class Row : public Box {
public:
    template <class... Kids>
    explicit Row(Kids &&...kids) : Box(UUI_ROW, std::forward<Kids>(kids)...) {}
};

class Column : public Box {
public:
    template <class... Kids>
    explicit Column(Kids &&...kids) : Box(UUI_COLUMN, std::forward<Kids>(kids)...) {}
};

}  // namespace ui

#endif
