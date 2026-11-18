# A C++ layer over the toolkit: `uixx`

C++ classes over libui, in gtkmm's shape: a window is a class whose
widgets are its members, a button's click is a lambda, and a widget of
your own is a subclass that overrides `draw()`. The C library stays the
implementation; the layer is headers that build `struct uui_item` arrays
and fill `uapp_desc` for it.

**STAGE 1 IS BUILT (2026-11-18)**: `ui::Window` and `ui::run<W>()`,
`ui::Label`, `ui::Button`, `ui::Row`/`ui::Column`, and `ui::Widget`.
Counter is rewritten over it (`userland/libs/uixx/examples/counter/`),
tested headless on the host (`tools/hostchecks/uitest/counterxx_app_uitest.c`)
and clicked on a guest (`tools/tests/sweep/cross_gxx_test.py`). The
stage markers on the headings are the authority.

## What real systems do

- **gtkmm** (GTK) is a compiled library over GObject: each class wraps
  a C object, callbacks are sigc++ signals
  (`button.signal_clicked().connect(...)`), a custom widget overrides
  `on_draw()`, and a widget is either a member of its window or a heap
  object its container frees (`Gtk::make_managed`).
- **WTL** (Win32) is header-only templates over the C API with no
  runtime of its own; MFC and wxWidgets are thick frameworks with
  message maps.
- **Qt, Haiku, FLTK** are C++ all the way down -- no C library to sit
  on, so not this question.

SaunaOS follows gtkmm's API and WTL's packaging: header-only, so there
is no second library to version or build `-fpic` (libstdc++ is static
only, `docs/cc-design.md`), and a program pays for the classes it uses.

## The picks (2026-11-18)

- **SDK only, full C++.** The headers ship in `/usr/include/uixx/` for
  `x86_64-saunaos-g++`; `std::function` and `std::string` are used
  freely. The in-tree build compiles C, so no in-tree app is C++ yet
  (`docs/roadmap.md`). Not chosen: a WTL-style subset with no libstdc++
  (`-fno-exceptions`, its own callback type), which the in-tree gcc could
  build but which is not the C++ a port is written in.
- **Widgets are MEMBERS of a `ui::Window` subclass**, as the C toolkit's
  are static structs: nothing allocates per widget, and an Item is
  neither copyable nor movable because the C side holds its address. Not
  chosen: parent-owned heap widgets (`make_managed`, Qt's parent
  pointer) -- a second ownership rule for UI built at runtime, which
  nothing here builds yet.
- **One slot per event**: `on_click(fn)` replaces the previous handler,
  as the C toolkit has one `on_action`. Not chosen: sigc++-style signals
  with many listeners and connection objects -- machinery for a need no
  caller has.

## Stage 1: the bridge -- BUILT

```
  app.hpp ─┬─ window.hpp   ui::Window, ui::run<W>()  ──>  uapp_desc, uapp_run
           ├─ box.hpp      ui::Row, ui::Column        ──>  uui_layout + uui_item[]
           ├─ controls.hpp ui::Label, ui::Button      ──>  uui_label, uui_button (by value)
           └─ widget.hpp   ui::Item, ui::Widget       ──>  one uui_widget_ops of trampolines
```

- **An Item is a `uui_item` the C router walks.** Until it is placed it
  holds its own; a Box copies it into its array (reserved to the exact
  count before the first child, so it never reallocates) and the Item
  then writes through to that slot -- `hide()`, `name()`, `fill()`.
- **A Box named in place is moved into its parent and owned by it**;
  a named child is referenced. A temporary that is not a box is a compile
  error (`add(Item &&) = delete`), since it would be gone before the
  first draw. The fluent setters on a Box keep an rvalue an rvalue, so
  `ui::Row{a, b}.name("row")` is still owned.
- **`set_root()` walks the tree once**: it numbers every item (an id is
  how `on_action`/`on_widget` find the object again; a button's commit
  code is its id) and builds the Tab ring from every item whose ops take
  keys, in tree order -- what a C app declares by hand as
  `uapp_desc.focus`.
- **A `ui::Widget` is `this` behind one shared ops table**; every slot
  calls a virtual whose default is the NULL slot's behaviour (no size,
  draws nothing, consumes nothing, takes no focus).
- **No exception unwinds into C.** The toolkit is built with
  `-fno-asynchronous-unwind-tables`, so a throw crossing its frames
  terminates. Every trampoline catches; the Window keeps the first
  exception, quits the loop, and `run()` rethrows it.
- **Text is owned by the object.** The C structs keep a caller-owned
  `const char *`; `ui::Label` and `ui::Button` hold the `std::string` it
  points at.

**Testing.** The host harness builds a `.cpp` app with the host g++
against glibc and libstdc++ (`uitest_hostcheck.py`; tolibc's headers
cannot sit under libstdc++'s), so `counterxx_app` drives Counter++'s
real `main()` by widget name in a second, with every check a pixel round
trip. It was seen red with the Tab ring not built, a button's code not
set, and a nested Row dropped. `cross_gxx_test.py` builds the same source
against the SDK and clicks it on a guest.

## Stage 2: value widgets, menus, dialogs

On the roadmap, one line each: the value widgets with an `on_change`
slot (read back from the C struct, as an `on_widget` handler does), a
menu bar whose `item_flags` come from a lambda (rule 7 of
`userland/CLAUDE.md`), `ui::message_box`, and a timer. Add a class when a
program needs it, against `uui_widget.h` and the C widget's header --
never against another wrapper.

## The case against

- **Two APIs to keep in step.** Every C widget change can break a
  wrapper. Header-only keeps the break a compile error in the SDK
  programs that use it, and the host case covers the classes that exist.
- **No in-tree user.** Until a C++ program ships in the tree, the layer
  is tested by its example alone.
