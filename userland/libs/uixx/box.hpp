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

class Box : public Item {
public:
    template <class... Kids>
    explicit Box(uui_dir dir, Kids &&...kids) : Item(&uui_layout_ops, &c_) {
        c_.dir = dir;
        // EXACTLY sized before the first child: each child keeps a pointer
        // to its slot, so the array must never reallocate.
        items_.reserve(sizeof...(Kids));
        kids_.reserve(sizeof...(Kids));
        (add(std::forward<Kids>(kids)), ...);
        c_.items = items_.data();
        c_.count = static_cast<int>(items_.size());
    }

    // The vectors' buffers move with them, so every child's slot pointer
    // stays good; only the pointers at this object's own members move.
    Box(Box &&o) noexcept
        : Item(std::move(o)), c_(o.c_), items_(std::move(o.items_)), kids_(std::move(o.kids_)),
          owned_(std::move(o.owned_)) {
        rebind(&c_);
        c_.items = items_.data();
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

    void add(Item &kid) { place(kid); }
    void add(Box &&kid) {
        owned_.push_back(std::make_unique<Box>(std::move(kid)));
        place(*owned_.back());
    }
    // A temporary that is not a box dies before the first draw.
    void add(Item &&kid) = delete;

    void place(Item &kid) {
        items_.push_back(kid.proto_);
        kid.slot_ = &items_.back();
        kids_.push_back(&kid);
    }

    uui_layout c_{};
    std::vector<uui_item> items_;
    std::vector<Item *> kids_;
    std::vector<std::unique_ptr<Box>> owned_;
};

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
