# Window files

**Category:** Getting started

## Description

Some programs keep their windows in a **window file** instead of in
their code: a short text file that says which controls a window has,
how they are arranged and what they are called -- and, for some, what
their menus hold. The program reads it
every time it starts. So you can change a window -- move a panel, rename
a button, make a column wider -- by editing the file and opening the
program again. Nothing needs rebuilding.

The files live in `/usr/share/ui`, one per program, named after it:
`/usr/share/ui/crashreports.ui` is Crash Reports'. Today Crash Reports,
the Theme Editor, Calculator, the Windows installer, Log Viewer, Disk
Mark, Image Viewer and Help keep their windows there; Hex Viewer, the
Video and Audio Players, Boot Manager, Minesweeper, Device Manager and
Disks keep their menus there; and Notepad, the File Manager, the
Terminal, Screenshot and Retro Launcher keep their Options windows there.

The file says what the window LOOKS like. What the controls DO stays in
the program: a button names a command the program knows, and the
program decides what happens when it is clicked.

## A first look

Here is a small window: a list with a details panel beside it, and a
row of buttons underneath.

```
// Anything after two slashes is a comment.
window #main title="Reports" size=80x24 resizable {
    column {
        row fill {
            table #reports fill {
                column "Program" width=16
                column "What happened"
            }
            label #details fill-h width=30
        }
        row fill-w {
            stretch
            button #open "Open"
            button #close "Close" default
        }
    }
}
```

Read it from the outside in:

- `window #main` is the window. `#main` is its NAME -- the program asks
  for its windows and controls by name.
- `column` stacks what it holds top to bottom; `row` puts it side by
  side.
- `table`, `label` and `button` are controls. Text in quotes is what
  they show.
- `fill`, `fill-w` and `width=30` say how much room each one takes.
- `stretch` is empty space that pushes the buttons to the right.

## The rules of the language

**One thing per line.** Each line is one window, container or control.
It starts with its TYPE, then any of these, in any order:

- a `#name`, at most one;
- a `"quoted text"`, at most one;
- settings: `key=value`, or a single word like `fill`.

**Braces hold what is inside.** A `{` at the END of a line opens the
list of what the thing holds; a `}` alone on a line closes it.

```
row {
    label "Inside the row"
}
```

**Text goes in double quotes.** Inside quotes, `\"` is a quote mark,
`\\` a backslash, `\n` a new line and `\t` a tab.

**A setting is either a word or a value, never both.** `resizable` is
right; `resizable=yes` is a mistake (to turn it off, leave the word
out). And `title` alone, without `=...`, is a mistake too.

**Sizes come in character cells and pixels.**

- **Character cells** follow the text size, so a window grows with the
  font: a window's `size=`, and an item's `width=` and `height=`. One
  cell is as wide as the letter `n` and as tall as a line.
- **Pixels**: a window's `min=`.
- **Either**: `margin=` and `gap=` are a named step (`small`, `medium`,
  `large`), pixels as a plain number, or lines and characters with `lh`
  and `ch` (see "Arranging things").

## Windows

A file is a list of windows and menus (see Menus, below). A window holds exactly ONE `row`,
`column` or `grid`, and everything else goes inside that -- except a
`dialog #name` line beside it: the place the program's questions
appear ("These disks can't be used"), drawn over the window when one is
asked. It has no settings; keep its name. A control marked
**`floating`** may go beside it too: the program puts it where it
belongs itself (Image Viewer's slideshow buttons, in the bar it draws),
so it is never arranged with the rest.

| Setting | What it does |
|---|---|
| `#name` | the name the program opens it by (needed) |
| `title="..."` | the window's title (the program may replace it) |
| `size=80x24` | how big it opens, in character cells |
| `min=640x320` | the smallest it can be made, in pixels |
| `resizable` | it can be resized |
| `focus="a b c"` | the order Tab moves through, by name |
| `autofocus` | start with the first one in `focus=` already selected |
| `app-id=word` | the name the desktop remembers its size under |

## Arranging things

| Type | Arranges |
|---|---|
| `row` | side by side, left to right |
| `column` | one under another, top to bottom |
| `grid` | equal cells, `cols=` across, filled row by row |
| `stack` | pages in one place, one shown at a time |
| `wizard` | a program's steps, a page each, with Back and Next |

`margin=` is the space around the edge and `gap=` the space between
items. Leave them out for the usual spacing, which follows the text
size. **`small`, `medium` and `large`** are the desktop's own steps --
half the usual padding, the padding itself, and twice it -- and follow
the text size, so `gap=medium` is the right word for "the normal space
between controls". **`none`** is no space at all, for bars that meet
edge to edge. **Two values** give the top and bottom, then the sides:
`margin="medium large"` is one padding above and below and two at the
left and right. A plain number is pixels (`gap=8`); `lh` counts lines
and `ch` characters, so `gap=0.5lh` is half a line of text and `gap=1ch`
one character, and both follow the text size too. `0` is not allowed: it
already means "the usual spacing", so for a hairline write `1`.

A **stack** holds PAGES -- usually one `column` each -- and shows one
of them; the program switches between them, and Tab only visits what
is on the page you can see. It is always as big as its biggest page,
so switching never makes anything around it move. `shown=1` picks the
page it starts on (counting from 0); `hidden` is not used on a page.

A **wizard** holds STEPS: `step "Disks" note="Where are the disks?"`
with one page inside it, usually a `column`. `next="Install"` changes
the Next button's text on that step. On the wizard itself, `heading=`
is the line above the list of steps, `skip="Cancel"` adds a button
under it, and `finish=` is Next's text on the last step. The steps
belong to the program, in its order: you can change what is on each
page, but not add, remove or reorder steps. A `#name` goes on a step's
page, not on the `step` line.

## Settings every control takes

| Setting | What it does |
|---|---|
| `#name` | lets the program find it |
| `fill` | take all the spare room, both ways |
| `fill-w` / `fill-h` | take the spare width / height only |
| `width=N` | in a `row`: exactly N cells wide |
| `height=N` | in a `column`: exactly N lines tall |
| `height=control` | as tall as a one-line control (a button, a text field), whatever it would be on its own |
| `center` | in a `row`, centred top to bottom; in a `column`, left to right |
| `hidden` | not shown (the program can show it later) |

In a row, `fill-w` means "grow sideways into the spare room", and
`fill-h` means "be as tall as the row". In a column it is the other way
round. With nothing set to fill, a control is just as big as it needs.

## Controls

| Type | Text in quotes | Its own settings | Holds |
|---|---|---|---|
| `label` | what it says | `dim` (greyed), `bold`, `wrap`, `rows=N` | -- |
| `button` | its label | `command=word`, `default`, `outlined`, `disabled` | -- |
| `checkbox` | its label | `checked`, `accent` (a rounded box in the accent colour) | -- |
| `textbox` | what is typed in it at the start | -- | -- |
| `spinbox` | -- | `range=LO..HI` (needed), `step=N`, `unit=word`, `value=N` | -- |
| `tree` | -- | -- | -- |
| `app` | -- | -- | -- |
| `dropdown` | -- | `selected=N` | `item "..."` lines |
| `listbox` | -- | -- | `item "..."` lines |
| `progress` | -- | `value=0..1000` (thousandths) | -- |
| `toolbar` | -- | -- | `item` and `separator` lines |
| `table` | -- | -- | `column "title"` lines |
| `markdown` | -- | -- | -- |
| `stretch` | -- | -- | -- |
| `custom` | -- | -- | -- |
| `colourpick` | -- | `swatch` | -- |
| `themepreview` | -- | -- | -- |
| `segmented` | -- | `selected=N` | `item "..."` lines, or none |
| `meter` | -- | -- | -- |
| `chart` | its heading | -- | -- |
| `image` | -- | -- | -- |
| `thumbstrip` | -- | -- | -- |
| `transport` | -- | -- | -- |
| `menubar` | -- | `menu=name`, a `menu #name` in this file | -- |
| `statusbar` | -- | -- | -- |
| `loglist` | -- | -- | -- |
| `space` | -- | `size=medium` (any spacing) | -- |

Some notes:

- **A button's `command=`** is the word the program knows it by. If you
  leave it out, the button's `#name` is used. The button marked
  `default` is drawn in the accent colour: it is what Enter does.
- **A dropdown's or listbox's items** go inside it, one `item "..."`
  per line, usually with an `id=`: `item "Rounded" id=round`. The
  program reads the id, not the place in the list, so you can reorder
  and relabel the items freely -- but keep each id. A program may fill
  the items in itself instead (the Theme Editor's list of themes), and
  then the file leaves them out.
- **A toolbar item** looks like
  `item "Save" icon=tb-save tip="Save it" command=save role=edit`.
  `icon=` is needed; `role=` colours the icon by what the command does:
  `nav`, `view`, `create`, `edit`, `danger`, `arrange` or `media`.
  `menu` adds a small arrow, and `end` moves it and everything after it
  to the right end. A `separator` line draws a divider. **Any other
  line in a toolbar is a control the toolbar holds** -- Disk Mark's
  `dropdown #size` between Stop and Copy. The toolbar places it, a
  little apart from the buttons and centred on them, and narrows it when
  the window is narrow; it takes no `width=` of its own.
- **A table's columns** look like `column "Size" width=10 align=right`.
  `width=` is in characters; a column with no width shares out what is
  left.
- **A spinbox** is a number typed or stepped with its arrows, kept within
  `range=` -- `spinbox #size range=8..40 unit=px` -- and moved by
  `step=` (1 unless it says). It starts at `value=`, or the range's low
  end; a program usually puts its own number in when the window opens.
- **A `tree`**'s rows -- Help's contents -- are the program's.
- **`app #name`** is a place for one of the program's own controls, which
  the file has no type for (Help's page, a web view or a Markdown view as
  the machine allows). The file decides where it sits and how it is
  sized; the program gives it the control.
- **`custom`** is an area the program draws itself; the program also
  decides how big it is.
- **`segmented`** is a row of buttons of which one is chosen (Log
  Viewer's All / Errors / Warnings); its `item` lines work as a
  dropdown's. A program may fill it itself, as Log Viewer does to show
  counts. **`menubar menu=bar`** is a menu bar holding the `menu #bar`
  declared in the same file (see Menus). A **`statusbar`**'s panes and a
  **`loglist`**'s lines are the program's.
- **`space size=medium`** is a fixed gap of that size in a row or a
  column, where `stretch` takes whatever is spare. **`colourpick`** and **`themepreview`** are
  the Theme Editor's colour picker and theme preview.

## Menus

A `menu #name` line beside the windows holds a menu: the titles of a
menu bar, each with its rows, or the rows of a menu that opens on a
right-click. A program may keep only its menus in its file, with its
window still in its code -- Log Viewer does.

```
menu #bar {
    menu "File" {
        item "Save shown lines as..." command=save accel="Ctrl+S"
        separator
        item "Exit" command=exit accel="Alt+F4"
    }
    menu "Go" {
        item "Next error" command=next_err accel="F8"
        item "Previous error" command=prev_err accel="F7"
    }
}
```

A row's `menu "Title" { ... }` instead of an `item` opens a submenu,
with its own rows inside the braces. Every `command=` must be one the
program knows.

| Line | Its text in quotes | Its settings |
|---|---|---|
| `item` | the row's label (needed) | `command=word` (needed), `accel="Ctrl+S"`, `icon=`, `role=`, `strip` |
| `separator` | -- | -- |
| `menu` | the submenu's title (needed) | `command=word`, `icon=`, `role=` |

- **`accel=`** is the shortcut shown at the row's right. It is only a
  label: the program already handles the key, so changing the text here
  does not change the key.
- **Ticks and greying are the program's.** Whether "Follow new lines" is
  ticked or "Copy line" greyed changes as you work, so the file has no
  setting for it; the program answers each time the menu opens, by the
  row's `command=`. A submenu's `command=` lets it grey the whole branch.
- **`icon=`** draws an icon in the row's left margin, and `role=` colours
  it, with the toolbar's words. **`strip`** makes an item an icon-only
  button in a row across the menu's top; strip items come first and
  need an `icon=`.
- A menu opens at most five levels deep, and the menu at the top of the
  file counts as the first -- so a menu bar has room for three submenus
  inside each other below a title's own menu.

## Options windows

An `options #name` line beside the windows holds a program's **Options
window**: the pages in its sidebar, and each page's rows. Most rows edit
one **setting** of the program's own settings file, named by its `key=`
-- the word the setting has in that file -- so the window fills the row
when it opens, puts the program's default in on **Defaults** and writes
it back on **OK**. The program's settings file says what each setting
holds and what it may be; the window file only says how it is shown.

```
options #options title="Notepad Options" {
    page "Editor" {
        fontsize #font caption="Font size:" key=font_size range=8..40 unit=px "Use the desktop's size"
        choice #tabwidth caption="Tab width:" key=tab_width {
            item "2" value=2
            item "4" value=4
            item "8" value=8
        }
        check #tabspaces "Insert spaces when Tab is pressed" key=tab_spaces
        check #indent caption="Typing:" "Keep the indent of the line above" key=auto_indent
    }
    page "Files and startup" {
        row "Recent files:" {
            number #recent key=recent unit=files
            button #clear "Clear list"
        }
    }
}
```

Each line in a page is a row: its `caption=` on the left, the control
beside it. A row with no caption sits under the one above, as the second
checkbox does. A `row "Caption:" { ... }` puts two or three controls
side by side; its quoted text is the caption.

| Line | Edits | Its text in quotes | Its settings |
|---|---|---|---|
| `page` | -- | the sidebar's title (needed) | `#name`, `note="..."` (a line under the page's heading) |
| `check` | an on/off setting | the box's label (needed) | `key=` (needed), `caption=` |
| `choice` | an on/off, number or word setting | -- | `key=` (needed), `caption=`, and an `item "Label" value=word` line per button |
| `number` | a number setting | -- | `key=` (needed), `caption=`, `unit=`, `step=`, `range=LO..HI` |
| `fontsize` | a size where 0 means the desktop's | the box's label (needed) | `key=` (needed), `range=LO..HI` (needed), `caption=`, `unit=` |
| `rows` | rows the program adds itself | -- | `#name` (needed) |
| `app` | a control of the program's own | -- | `#name` (needed), `caption=`, `fill`, `fill-w`, `fill-h`, `width=` |

- **A choice's `value=` is spelt as the settings file spells it**:
  `on` or `off`, a number, or the setting's word (`value=markdown`). The
  buttons may be relabelled and reordered; a value the setting cannot
  take is refused when the program starts.
- **`range=` may only narrow** a number's own range, never widen it.
- **`rows`** marks where a part the program builds row by row goes --
  the Terminal's and Retro Launcher's Screen effect page.
- **`app`** is a place for one of the program's own controls, such as the
  Terminal's colour-scheme cards. The program fills it; the file decides
  where it sits.
- The ordinary controls -- `label`, `button`, `checkbox`, `textbox`,
  `dropdown`, `segmented`, `spinbox` and `table` -- may be rows too. They are the
  program's, not a setting's: the File Manager's Scripts and Programs
  rows, for instance, are stored in another file, so it reads them itself.
- `options` takes `title="..."` and `size=WxH` (characters by lines, the
  smallest it opens at; it grows to fit its largest page).

## When something is wrong

The program checks the whole file when it starts. If anything is wrong
it does not guess: it opens a small window saying **which file, which
line and what is wrong**, for example

```
This program's window could not be built:
crashreports.ui:18: table takes no 'fil'
```

Fix that line, save, and open the program again. The common ones:

| The message says | It means |
|---|---|
| `there is no widget type 'lable'` | a type is misspelt |
| `label takes no 'fil-w'` | a setting is misspelt, or that control does not have it |
| `the app has no command 'print'` | a button, toolbar item or menu row names a command the program does not have |
| `resizable is a flag and takes no value` | write `resizable`, or leave it out |
| `width= sizes a row's child` | `width=` only works inside a `row` (`height=` inside a `column`) |
| `window #main has two #ok` | two controls in one window share a name |
| `#ok and #cancel both answer to the app as 3` | two controls would be the same thing to the program |
| `the app needs #details in window #list` | the program looks for a name the file no longer has -- a rename went too far |
| `the app needs an item with id=round` | a dropdown item's `id=` was changed or removed |
| `#frame_page ... is not a page of #pages` | a page was moved out of its `stack` |
| `gap=1.5 is not a spacing` | pixels are whole numbers; lines and characters take up to two decimals (`0.25lh`); a step is `small`, `medium` or `large` |
| `a step holds one page` | put a step's controls inside one `column` |
| `#wizard has 3 steps; the program has 4` | a step was added or removed; the program needs all of its own |
| `the app asked for menu #bar, which the file lacks` | a `menu #name` was renamed or removed |
| `strip buttons come first in a menu` | move the `strip` items above every other row |
| `menubar names menu #bar, which the file lacks` | the `menu #bar` it shows was renamed or removed |
| `selected= picks one of the item lines, and there are none` | give the control its `item` lines, or leave `selected=` out |
| `space needs size=` | say how big: `space size=medium` |
| `spinbox needs range=LO..HI` | say which numbers it offers: `range=1..100` |
| `the program gave app #doc no widget` | the program has no control for that `app` place -- its name was changed |
| `the program has no setting key=tab_widht` | a `key=` is misspelt; the setting's word is in the program's settings file |
| `key=wrap cannot store value=yes: it takes on or off` | a choice's `value=` is not one the setting can hold -- the message lists what it takes |
| `check edits an on/off setting, and key=tab_width is a number setting` | use `choice` or `number` for that setting |
| `range=0..2000 reaches past key=thumb_max_mb's own 1..1024` | `range=` can narrow a setting's range, not widen it |
| `a row holds two or three controls` | a single control goes on its own line, with `caption=` |
| `options #options has two #wrap` | two rows share a name -- a row with no `#name` is called by its `key=` |

**Keep the names.** The program finds its controls by their `#name`
and the commands by their word. You can move, resize, reorder and
relabel freely; renaming a `#name` or a `command=` breaks the link, and
the program says so when it starts.

**After editing, close every window of the program, then open it
again.** It reads the file once, when it starts; while an old window is
still open, opening the program may just bring that window forward.

## Your changes and updates

System Update keeps your changes to files in `/etc` and `/home`, but
not in `/usr`: the next update puts every window file back the way it
shipped. Keep a copy of anything you want to keep.

## See also

Crash Reports, the Theme Editor, Calculator, the Windows installer, Log
Viewer and the programs above, the ones with window files. In the source tree, `docs/ui-files-design.md` says why the
language is shaped like this, and each program's `.ui` file sits beside
its code.
