# Window files: a window's widgets declared in a `.ui` file

**Status: stages 1 and 4a BUILT, and 4b's menus, `menubar` and
`segmented`** -- the language (`userland/libs/base/udecl.h`), the
builder (`userland/libs/ui/uui_builder.h`), five programs' windows on
it -- Crash Reports, the Theme Editor, Calculator, the Windows installer
and Log Viewer -- and seven more programs' menus (each `<program>.ui`
beside its source). Stage 2 is deferred
for a host tool, `tools/dev/ui_push.py`. The rest of the stages below are PLANNED. Why it is shaped like this, against the
other ways it could have gone, is `docs/decisions/gui.md`, "A window's
widgets can live in a .ui file, read when the program starts".

## The problem

Every GUI program built its window in C: a static struct per widget,
an `init` call per widget, a `struct uui_item` array per container
with positional flags, a `struct uui_layout` per container, an
`on_size` callback for the window's size, a focus array. Changing a
gap, moving a panel or renaming a button meant editing that C,
rebuilding and reseeding the disk. The toolkit already did the hard
part -- `uui_layout` places widgets from their natural sizes, events
come back as an `id` through one `on_widget` switch -- so the C that
remained was a DESCRIPTION written as code.

## What real systems do

| System | Format | Built into widgets | Code finds a widget by |
|---|---|---|---|
| Win32 | `.rc` DIALOG template | compiled into the PE, built at `CreateDialog` | control id, `GetDlgItem` |
| Qt Widgets | Designer `.ui` (XML) | at BUILD time: `uic` generates C++ | generated member fields |
| GTK 4 | GtkBuilder `.ui` (XML); Blueprint compiles to it | at RUN time | `gtk_builder_get_object()`; templates bind to struct fields |
| Android | layout XML, compiled to binary XML | at run time ("inflate") | `findViewById(R.id.x)` |
| QML / XAML | their own languages, with expressions and script | at run time | bindings, code-behind |

Menus are a resource of their own in the first two: Win32's `.rc` has
`MENU` beside `DIALOG` (`LoadMenu()`), and GtkBuilder a top-level
`<menu>` (a GMenuModel) beside its widgets; Qt Designer puts a
QMenuBar inside the QMainWindow it designs.

All of them split it the same way: **the file holds structure and
properties, the code holds behaviour, and names join the two.** QML and
XAML go further and put logic in the file -- a second language, a
script engine, a second place behaviour lives.

**SaunaOS follows GtkBuilder and Android**: read at run time, so an
edit is seen by reopening the program, with no rebuild (Qt's `uic` and
Win32's `rc` are stage 3, an option beside it). The syntax is
**Blueprint's and `.rc`'s shape, not XML**: there is no XML parser in
`libbase` (expat is a port linked by the programs that need it), and a
hand-edited file wants to be short. **No expressions and no script**:
a value is a word, a number or a string, and anything that depends on
state stays in C.

## The language

**THE REFERENCE IS `data/usr/share/doc/guide/ui-files.md`** -- a page
for a person editing a window file, shipped to `/usr/share/doc/guide`
(Help, `doc ui-files`): every type, every attribute, and every error
message with its fix. Change it with the language. What it does not
say, because only this file's readers need it:

- One node per line; `{` ending a line opens its children, `}` alone on
  a line closes them. Nothing spans lines, which is what lets a bare
  word be a flag and a node's type at once without ambiguity.
- **A READER REJECTS RATHER THAN GUESSES** (`base/udecl.h`): every getter
  marks what it read and an unread attribute names its line; a flag
  given a value and a value written bare are errors; `margin=0`/`gap=0`
  are refused, because 0 is `uui_layout`'s "font-derived default"; two
  widgets answering to one id are caught at load, where uapp would only
  have refused to start with a log line.
- **Units**: `size=`, `width=`, `height=` are CHARACTER CELLS (an advance
  of `n` by a line, `uapp_desc.cells_w/h` and `uui_item.main_cells`,
  resolved at each layout); `min=` is pixels; `margin=` and `gap=` are
  pixels, or lines and characters with CSS's `lh` and `ch` (`0.5lh`,
  `1ch`, at most two decimals), kept as hundredths in `uui_layout`'s
  `margin_unit`/`gap_unit` and resolved at each layout like the
  defaults -- so a converted `ugfx_char_h() / 2` stays exactly that at
  every font size. **Two values** are top-and-bottom then the sides
  (CSS's order), kept as `uui_layout.margin_x`; **`none`** is an
  explicit zero (`UTHEME_STEP_NONE`), the one spacing a plain 0 cannot
  say. **Named steps** -- `small`, `medium`, `large` -- are
  `utheme_step()`'s ladder of the theme's pad (half, one, two), kept as
  `UUI_STEP` in the same unit fields and resolved at each layout too:
  Fluent's spacing tokens, libadwaita's 6/12/18. None of the existing
  files' pixel values was a step at the default font, so they kept their
  numbers rather than move a pixel. Pixels are NOT scaled by
  `display.scale`, as the C fields are not (a roadmap item for both).
- **A type is a row in `uui_builder.c`'s `TYPES`**: its word, ops, size,
  build function, and -- when Tab reaches it through other ops -- a
  focus-only table (`dropdown`, `listbox`, `textbox`) or a composite's
  own list of focusables (`colourpick`, six).
- **A COMPOSITE'S OBJECT IS NOT ITS ITEM'S WIDGET**: the colour picker
  puts its inner layout in the tree, so a lookup returns the object a
  type allocated, checked by the type's WORD, never by comparing ops.
  Its controls take ids from its `#name`'s value up, so its own item
  answers to none.
- **A CHOICE IS KNOWN BY ITS `id=`**, not its place: a dropdown's or
  listbox's `item "..." id=word`, read with `uui_builder_choice()` /
  `uui_builder_choice_id()` (GtkComboBox's active-id, Qt's itemData), so
  a person may reorder and relabel items. A program asks for every id it
  means at start, so a missing one is the error window.
- **A `stack` (`ui/uui_stack.h`) places EVERY page in its rect** whenever
  it is placed, and is as large as its largest page -- Qt's
  QStackedWidget, GtkStack's homogeneous default. Hiding siblings in a
  column is not the same: layout runs at open and on a resize only, so
  a page shown later keeps stale places (Theme Editor's frame page drew
  its three rows on one line until it moved to a stack), and laid out
  again the column narrows to each page and its neighbours jump.

## The program's side

```c
static const struct uui_builder_name NAMES[] = {
    { "reports", ID_TABLE }, { "open", CMD_OPEN }, ...
};
g_ui = uui_builder_load("crashreports", NAMES, n);       // /usr/share/ui/crashreports.ui
g_table = uui_builder_widget(g_ui, "list", "reports", "table");   // the type's word
if (uui_builder_error(g_ui)) return uui_builder_run_error(g_ui);
g_table->cell = cell;
struct uapp_desc desc = { .on_widget = on_widget, .on_key = on_key };
if (!uui_builder_window(g_ui, "list", &desc)) return uui_builder_run_error(g_ui);
return uapp_run(&desc);
```

- **THE NAMES TABLE IS THE CONTRACT.** A `#name` in it is that widget's
  id; every `command=` must be in it. A name not in it is still a name
  -- looked up, and logged.
- **Check the error ONCE, after the lookups and before using what they
  returned**: a lookup of a name the file lacks, or of the wrong type,
  returns NULL and records the error.
  `uui_builder_run_error()` then opens a window saying which file and
  line, which is what a person who just edited it needs.
- **Hide before `uui_builder_window()`**: the focus ring is built then,
  and starts on its first only for a window that says `autofocus`.
- **Switched pages are a `stack`**, shown with `uui_stack_show(stack,
  uui_builder_page(...))` -- never `hidden` on siblings after the window
  is open. `uui_builder_page()` checks the page is the stack's, at load.
  The builder gives the focus ring each control's page (`uui_focus.page`),
  so Tab, a click and a key skip a page that is not shown -- the pages
  share one rect, so a click would otherwise focus a control under the
  shown one.
- **A menu is a top-level `menu #name`**, beside the windows, read with
  `uui_builder_menu(b, "bar", &count)` and handed to
  `uui_menubar_init()` -- or `uui_menubar_open_at()` for a context menu.
  Its rows are the same `struct uui_menu_item` tree the C's `UUI_MENU`
  arrays were, allocated by the builder, so **the builder lives as long
  as the bar**. A program may declare only its menus and keep its window
  in C, which is how Log Viewer took them first: every program with a
  menu bar lays its window out by hand. Nesting is refused past
  `UUI_MENU_MAX_DEPTH`, counted as a context menu opens it (one level
  deeper than a bar's), so a file that loads opens to its last level
  either way.
- **A part shown or hidden while the window is open that should give
  its room to the rest** (Log Viewer's details panel, its Unmute button)
  is `hidden` and the program runs `uui_layout_run()` on the window's
  layout at the top of `on_draw` -- uapp lays out at open and on a
  resize only, and paints after `on_draw`, so the parts land before
  anything is drawn. Pages that share one place are still a `stack`.
- **Popups' room is the program's**: a `menubar`'s and a `toolbar`'s
  in-window fallback is bounded by `uui_menubar_set_bounds()` /
  `uui_toolbar_set_bounds()`, which the program calls with the window's
  size (Log Viewer, in `on_draw`).
- **A ROUTED LAYOUT IS PAINTED ONCE.** A builder window hands uapp the
  same items as `layout` and `widgets`, and `top_paint()` drew both --
  every widget twice, invisible but for antialiased corners darkened by
  the second pass (3x3 pixels at each rounded control; the cause of Log
  Viewer's only pixel difference). It now skips the layout's own paint
  when the router already draws it -- an item holding the layout, or
  every visible item the layout holds (`uui_router_covers_layout()`,
  tested in `uibuilder_test`).
- **An app-drawn `custom` area is sized by the app**, which owns what it
  draws (Theme Editor's role list and contrast lines).
- **The whole file is built at load**, every window in it, so a mistake
  in one face is reported whichever face opened. Menus are NAMED first, so a
  window's `menubar menu=` finds one declared below it, and everything is
  then BUILT IN FILE ORDER: the error kept (the first) is the one nearest
  the top.
- The program's file is TRACKED BESIDE ITS SOURCE, named after the
  program, and seeded flat into `/usr/share/ui/`; two with one name are
  refused at seed time. `check_control_states.py` counts a `.ui` file's
  bars against the directory's `item_flags` assignments, so a declared
  toolbar still has to say when a command cannot act.

## What it costs

The builder makes the SAME structs the C made -- laid out, drawn and
routed by the same code -- so an open window costs nothing more. Crash
Reports' three faces were measured identical before and after the
conversion: every widget rect and window size equal, and the pixels
equal but for the clock and the build string. The Theme Editor's colour
page is pixel-identical; its other two pages moved, because they were
broken (the stack, above).

The cost is reading and building the file, once per start, logged as
`uui_builder: crashreports.ui: 3 windows, 0 menus, 17 widgets, N us`. Measured
2026-11-05: on the ASUS (bare metal) about 170 us warm and 1.4-3.4 ms on
the first read after a boot; under TCG (every automated test) 0.8-2.3 ms
warm and 7.9 ms cold. Memory: the
source, the parsed tree (kept: the strings point into it) and the
widgets, a few KB for Crash Reports.

## Stages

1. **BUILT: read at run time** -- the language, the builder, nine
   types, Crash Reports.
2. **Live reload** -- DEFERRED 2026-11-05: restarting the program is
   what GtkBuilder, `.rc` and `uic` all need, and `tools/dev/ui_push.py`
   does it in about a second (copy, close, wait for the window to go,
   start, print the builder's line). Kept for if editing on the OS
   itself becomes common. The shape it would take (GTK Inspector's and
   Qt Quick's hot reload): the
   window watches its file and rebuilds in place when it is saved. The
   builder keeps all its state in `struct uui_builder`, never statics,
   so a second build beside the first is possible; the program's
   pointers into the old tree are what needs care (a rebuild callback
   that re-runs its lookups).
3. **Compiled, as an option** (Qt's `uic`, Win32's `rc`): a host tool
   turning `.ui` into C, for a program that wants the file's errors at
   build time and no read at start. Same language, same names table.
4. **More types**, a row each in `TYPES`.
   a. **BUILT**: `dropdown`, `listbox`, `checkbox`, `textbox`,
      `progress`, `custom`, `stack`, `colourpick`, `themepreview`; the
      Theme Editor. Then `wizard` (`step` children, each holding one
      page; the program sets the callbacks in the wizard's `d`, and its
      steps' ORDER is the program's, checked by `uui_builder_wizard()`;
      a page's `#name` is the wizard's own copy of the item,
      `uui_wizard_page()`), a label's `bold`, a checkbox's `accent`,
      `lh`/`ch` spacing, and `dialog` -- a line BESIDE the window's root
      that is routed and never laid out, as `desc.widgets` = [root,
      dialogs] (uapp draws an open one over the window); Calculator and
      the Windows installer.
   b. **BUILT 2026-11-06: menus**, a top-level `menu #name` (`item`,
      `separator`, nested `menu`; `accel=`, `icon=`, `role=`, `strip`),
      `uui_builder_menu()`, and Log Viewer's bar on it -- its four menus
      measured identical to the C's, every row's rect and every pixel.
      **BUILT 2026-11-06, for Log Viewer's whole window:**
      `menubar menu=NAME`, `segmented`, `statusbar`, `loglist`, and
      `space size=` (`uui_space`, QBoxLayout::addSpacing()); the item
      words `center` (`UUI_CENTER`, Qt::AlignVCenter) and
      `height=control` (`UUI_CONTROL_H`, the theme's one-line control
      height); two-value margins and `none`. Its window measured
      identical to the hand layout in three states -- every rect in its
      report and every pixel -- once uapp stopped painting a routed
      layout twice (below). Then `tabs`, `scrollview`, `splitter`,
      `spinbox`, `tree`.
5. **Options pages**: `uprefs`/`uui_prefs` are already tables, so a
   `.ui` file could describe a whole Options dialog with each row bound
   to its setting -- the shape `xscreensaver-settings` takes from a
   saver's descriptor.
