# Window files: a window's widgets declared in a `.ui` file

**Status: stage 1 BUILT** -- the language (`userland/libs/base/udecl.h`),
the builder (`userland/libs/ui/uui_builder.h`) and the first program to
use them, Crash Reports (`userland/apps/apps/crashreports/crashreports.ui`).
Stages 2-5 below are PLANNED. Why it is shaped like this, against the
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

One node per line; a block of children in braces.

```
// a comment, to the end of the line
window #list title="Crash Reports" size=120x30 min=640x320 resizable focus=reports {
    row margin=1 gap=1 {
        table #reports fill {
            column "Program" width=16
            column "What happened"
        }
        label #details fill-h width=40
    }
}
```

- A line is `TYPE`, then in any order at most one `#name`, at most one
  `"text"`, and attributes: `key=value` or a bare `flag`. A value is a
  bare word or a quoted string with `\"`, `\\`, `\n` and `\t`.
- `{` ending a line opens the node's children; `}` alone on a line
  closes them. Nothing spans lines.
- **A FLAG AND A VALUE ARE NOT INTERCHANGEABLE**: `resizable=no` is an
  error (leave the flag out), and so is a bare `title`.
- **`margin=0` and `gap=0` are refused**: 0 is `uui_layout`'s "the
  font-derived default", so it cannot be said; give 1 for a hairline.
- **A READER REJECTS RATHER THAN GUESSES.** An attribute nothing read,
  a type that does not exist, a command the program does not know,
  `width=` on a column's child, a name used twice in a window, two
  widgets answering to one id (uapp would refuse to start with only a
  log line): each is
  an error naming the file and line (`crashreports.ui:18: table takes no
  'fil'`), never a widget silently left unstretched or a button
  silently dead.

### Windows

The file holds only `window #name` nodes, each with exactly one `row`,
`column` or `grid` as its root. Its attributes go to `struct uapp_desc`:

| Attribute | Means |
|---|---|
| `title="..."` | the title; a program may override it (Crash Reports' "Notepad crashed") |
| `app-id=word` | `uapp_desc.app_id` -- the WM remembers a size per id |
| `size=WxH` | the opening size in CHARACTER CELLS (`uapp_desc.cells_w/h`: advances of `n` by lines), Win32's dialog units |
| `min=WxH` | the smallest content size, in pixels (`min_w/h`) -- NOT scaled by `display.scale`, as the C field is not |
| `resizable` | `UAPP_RESIZABLE` |
| `focus="a b c"` | the Tab order, by name; each must take keys. A widget hidden when the program asks for the window is skipped |

### Every item

| Attribute | Means |
|---|---|
| `#name` | the name the program looks it up by, and the layout log's (`uui_describe.h`) |
| `fill`, `fill-w`, `fill-h` | `UUI_FILL_W`/`UUI_FILL_H` |
| `width=N` | in a ROW, pin the item's width to N cells (`uui_item.main_cells`) |
| `height=N` | in a COLUMN, pin its height to N lines |
| `hidden` | not drawn, not hit, not focused |
| `track-hover` | `UUI_TRACK_HOVER` |

### Types

| Type | Text | Attributes | Children |
|---|---|---|---|
| `row`, `column` | -- | `margin=`, `gap=` (pixels; absent = font-derived) | items |
| `grid` | -- | `margin=`, `gap=`, `cols=` | items |
| `label` | its text | `dim`, `wrap`, `rows=` | -- |
| `stretch` | -- | -- (Qt's `addStretch()`: an empty label filling a row or column) | -- |
| `button` | its label | `command=` (else the `#name`), `default` (the accent face), `outlined`, `disabled` | -- |
| `toolbar` | -- | -- | `item "label" icon= tip= command= role= accel= menu end`, `separator` |
| `table` | -- | -- | `column "title" width= align=left\|right id=` |
| `markdown` | -- | -- | -- |

`role=` is a theme action role: `nav`, `view`, `create`, `edit`,
`danger`, `arrange`, `media`. A table's rows are the program's: it sets
`cell` on the table it looked up. A widget joins the language by a row
in `uui_builder.c`'s `TYPES` table.

## The program's side

```c
static const struct uui_builder_name NAMES[] = {
    { "reports", ID_TABLE }, { "open", CMD_OPEN }, ...
};
g_ui = uui_builder_load("crashreports", NAMES, n);       // /usr/share/ui/crashreports.ui
g_table = uui_builder_widget(g_ui, "list", "reports", &uui_table_ops);
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
- **Hide before `uui_builder_window()`**: the focus ring is built then.
- **The whole file is built at load**, every window in it, so a mistake
  in one face is reported whichever face opened.
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
equal but for the clock and the build string.

The cost is reading and building the file, once per start, logged as
`uui_builder: crashreports.ui: 3 windows, 17 widgets, N us`. Measured
2026-11-05 under TCG (every automated test): 0.8-2.3 ms warm, 7.9 ms on
a cold first read of the disk; not yet measured on hardware. Memory: the
source, the parsed tree (kept: the strings point into it) and the
widgets, a few KB for Crash Reports.

## Stages

1. **BUILT: read at run time** -- the language, the builder, nine
   types, Crash Reports.
2. **Live reload** (GTK Inspector's and Qt Quick's hot reload): the
   window watches its file and rebuilds in place when it is saved. The
   builder keeps all its state in `struct uui_builder`, never statics,
   so a second build beside the first is possible; the program's
   pointers into the old tree are what needs care (a rebuild callback
   that re-runs its lookups).
3. **Compiled, as an option** (Qt's `uic`, Win32's `rc`): a host tool
   turning `.ui` into C, for a program that wants the file's errors at
   build time and no read at start. Same language, same names table.
4. **More types**, a row each in `TYPES`: `checkbox`, `edit`,
   `dropdown`, `listbox`, `tabs`, `menubar`, `scrollview`, `splitter`.
5. **Options pages**: `uprefs`/`uui_prefs` are already tables, so a
   `.ui` file could describe a whole Options dialog with each row bound
   to its setting -- the shape `xscreensaver-settings` takes from a
   saver's descriptor.
