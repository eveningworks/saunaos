# A C++ layer over the toolkit: `uixx`

C++ classes over libui, in gtkmm's shape: a window is a class whose
widgets are its members, a button's click is a lambda, and a widget of
your own is a subclass that overrides `draw()`. The C library stays the
implementation; the layer is headers that build `struct uui_item` arrays
and fill `uapp_desc` for it.

**STAGES 1 TO 3 ARE BUILT (2026-11-18)**: `ui::Window` and
`ui::run<W>()`, `ui::Label`, `ui::Button`, `ui::Row`/`ui::Column`,
`ui::Widget`, the value widgets of `uixx/values.hpp`, and
`ui::MenuBar`, `ui::Tabs`, `ui::Timer` and `ui::message_box`.
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
           ├─ values.hpp   Checkbox ... Progress      ──>  uui_checkbox ... uui_progress
           ├─ menu.hpp     ui::MenuBar                ──>  uui_menubar (+ keycombo, libc)
           ├─ tabs.hpp     ui::Tabs                   ──>  uui_tabs + uui_stack
           ├─ dialog.hpp   ui::message_box            ──>  uui_msgbox_open
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

## Stage 2: the value widgets -- BUILT

`ui::Checkbox`, `ui::Switch`, `ui::Textbox`, `ui::Dropdown`,
`ui::Slider` and `ui::Listbox`, each with ONE `on_change` slot handed the
new value, and `ui::Progress`, which only shows one
(`userland/libs/uixx/examples/widgets/` is a form of all of them).
The picks (2026-11-18):

- **`on_change` is USER INPUT ONLY.** A click, drag, key or paste that
  left the value different calls it; a `set_*()` never does -- Qt's
  `textEdited`, not its `textChanged`, and the C toolkit's own rule
  (`on_widget` reports input). Two handlers that set each other's
  widgets cannot loop, which is what GTK's signal blocking exists for.
  Not chosen: every change, Qt's `valueChanged` and GTK's
  `value-changed`.
- **An option list is strings the widget owns, its value an index**
  (-1 for none), as the C widgets are. Not chosen: a typed
  `ui::Dropdown<T>` -- a template per widget over a C widget that knows
  only indices.

**How a change is found.** The router names a widget to its app on ANY
input it consumed -- a press that only moved a text field's caret too --
so `ui::Valued<T>` keeps the value it last reported or was set to, and
`changed()` reports only when the C struct now holds something else. A
`set_*()` records the value it set, so the next input is compared with
that. A text field of any length: `ui::Textbox` grows the C field past
its 128-byte inline buffer (`uui_textbox_set_max`) before setting a
longer text, and frees it.

`ui::Progress` gained its busy mode in stage 3, with the timers.

**Testing.** `widgetsxx_app` logs what each slot was handed and reads it
back after real input, and asserts that the constructor's and Reset's
`set_*()` calls report nothing; it was seen red with `set_*()` reporting,
with `set_*()` not recording its value, and with the comparison removed
(a click in the field that moved only the caret then reported).
`throwxx_app` throws from a button's `on_click`: the host toolkit is
built with the guest's `-fno-asynchronous-unwind-tables`, so with the
guard's catch removed the process terminates, and with it main() catches
the rethrown exception.

## Stage 3: menus, tabs, timers, a message box -- BUILT

The picks (2026-11-18):

- **A menu bar is a TREE WRITTEN IN PLACE**, each entry carrying its
  own lambdas -- `ui::MenuItem` (run, `.enabled_when`), `ui::MenuCheck`
  (get, set), `ui::Separator`, `ui::Menu` for a title or a submenu --
  gtkmm's `Gio::Menu` read as data, without action names. Not chosen:
  `ui::Action` objects shared by a menu and a toolbar (QAction's shape),
  which pays off once there is a toolbar.
- **THE SHORTCUT SHOWN IS THE SHORTCUT BOUND.** The text ("Ctrl+S",
  "F1") is parsed by `keycombo_parse()` -- the grammar the compositor and
  System Settings already share -- and the key runs the entry, greyed
  ones refusing it, before the window's `on_key` (Qt's
  `QAction::setShortcut` does both). Not chosen: display-only text, the C
  toolkit's way, where the shown and handled shortcuts can disagree.
- **A timer is a MEMBER built with its Window** -- `start(ms, fn)`,
  `start_once`, `stop()`, stopped when destroyed, so a lambda capturing
  `this` cannot outlive it (QTimer with a parent). Not chosen:
  `every()`/`cancel(id)`, GLib's `g_timeout_add` shape.
- **`ui::message_box` is ASYNC**: it returns at once and a lambda hears
  the answer (GTK4's only shape; it removed `gtk_dialog_run`). Not
  chosen: a modal return over `uui_msgbox_run`, which pumps the loop
  inside the call and must never run from a callback.
- **`ui::Tabs` is a notebook** -- the strip and the pages in one widget
  (QTabWidget, GtkNotebook) -- over `uui_tabs` and a `uui_stack`.

**How each maps onto C.**

- **Menu codes are process-wide.** The C bar's `item_flags` is a plain
  `unsigned (*)(int code)` with no context pointer, so every entry gets a
  code unique in the process, and a registry turns a code back into its
  entry and bar; a destroyed bar clears its codes. The flags are asked
  each time the menu draws, from the entry's lambdas.
- **The menu's bounds are the window's.** A menu drawn inside the window
  (when the compositor grants no popup surface) is clamped to the bar's
  bounds, which default to the bar's own rect and push it up over the
  bar. The layer wraps `uui_menubar_ops.press`, and every key, to set
  them to the window's content first.
- **F10 and the arrows while the bar is shut** reach it through a hook
  every Item has, `window_key()`, asked before `on_key`; an open menu
  takes keys through the router before both.
- **Timers share uapp's one tick**, re-armed with `uapp_set_tick()` to
  the nearest due time (an hour when nothing is due), on the toolkit's
  own clock (`sys_monotonic_ns()`), which the host harness moves. A
  `ui::Progress` that is busy asks its Window to tick it every 40 ms
  until it is not.
- **A page is a container whose children are PAGES**: the Window's walk
  records, for every focusable control, the `uui_item` of the page it is
  on (`uui_focus.page`), so the Tab ring and the keys skip a hidden
  page's controls. Pages are added before `set_root()`, which hands the
  arrays to C; a container re-points its children's slots whenever its
  array grows before then.
- **A window's size is in CHARACTER CELLS** (`set_size_cells`, uapp's
  `cells_w`/`cells_h`): uapp ranks a layout's natural size above a pixel
  `w`/`h`, so stage 1's pixel `set_size()` set nothing and is gone.

**Testing.** `notesxx_app` drives the Notes example: a greyed item's
shortcut does nothing and runs once enabled; F10, Right, Down, Enter
commits from the keyboard; a 1-second timer has not fired at 400 ms,
fires at 1.1 s and 2.1 s, and stops; a busy bar's block moves between
ticks; keys typed while the focused field's page is hidden do not reach
it; a tab click is reported and a menu-chosen page is not; and a click
on File, before any key, opens the menu below the bar with View's title
untouched. Seen red, one at a time: shortcuts unbound, a greyed
shortcut running, a timer firing before it is due, no animation tick,
the press not fitting the bounds, no page pointers in the ring, and a
tab choice not reported. The About box is a separate window, which the
host harness refuses: `cross_gxx_test.py` opens it on a guest with F1
and answers it with Enter.

## Stage 4

Not planned. Candidates when a program needs them: a toolbar sharing
the menu's entries (then `ui::Action`), `ui::Window::post()` over
`uapp_post` for a worker thread's results, context menus, and the
remaining widgets (tree, table, text view). Add a class against
`uui_widget.h` and the C widget's header -- never against another
wrapper.

## The case against

- **Two APIs to keep in step.** Every C widget change can break a
  wrapper. Header-only keeps the break a compile error in the SDK
  programs that use it, and the host case covers the classes that exist.
- **No in-tree user.** Until a C++ program ships in the tree, the layer
  is tested by its example alone.
