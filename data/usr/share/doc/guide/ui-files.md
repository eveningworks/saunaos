# Window files

**Category:** Getting started

## Description

Some programs keep their windows in a **window file** instead of in
their code: a short text file that says which controls a window has,
how they are arranged and what they are called. The program reads it
every time it starts. So you can change a window -- move a panel, rename
a button, make a column wider -- by editing the file and opening the
program again. Nothing needs rebuilding.

The files live in `/usr/share/ui`, one per program, named after it:
`/usr/share/ui/crashreports.ui` is Crash Reports'. Today Crash Reports
and the Theme Editor have one.

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
- **Either**: `margin=` and `gap=` are pixels as a plain number, or
  lines and characters with `lh` and `ch` (see "Arranging things").

## Windows

Every file is a list of windows. A window holds exactly ONE `row`,
`column` or `grid`, and everything else goes inside that -- except a
`dialog #name` line beside it: the place the program's questions
appear ("These disks can't be used"), drawn over the window when one is
asked. It has no settings; keep its name.

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
size. A plain number is pixels (`gap=8`); `lh` counts lines and `ch`
characters, so `gap=0.5lh` is half a line of text and `gap=1ch` one
character, and both follow the text size too. `0` is not allowed: it
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
  to the right end. A `separator` line draws a divider.
- **A table's columns** look like `column "Size" width=10 align=right`.
  `width=` is in characters; a column with no width shares out what is
  left.
- **`custom`** is an area the program draws itself; the program also
  decides how big it is. **`colourpick`** and **`themepreview`** are
  the Theme Editor's colour picker and theme preview.

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
| `the app has no command 'print'` | a button or toolbar item names a command the program does not have |
| `resizable is a flag and takes no value` | write `resizable`, or leave it out |
| `width= sizes a row's child` | `width=` only works inside a `row` (`height=` inside a `column`) |
| `window #main has two #ok` | two controls in one window share a name |
| `#ok and #cancel both answer to the app as 3` | two controls would be the same thing to the program |
| `the app needs #details in window #list` | the program looks for a name the file no longer has -- a rename went too far |
| `the app needs an item with id=round` | a dropdown item's `id=` was changed or removed |
| `#frame_page ... is not a page of #pages` | a page was moved out of its `stack` |
| `gap=1.5 is not a spacing` | pixels are whole numbers; lines and characters take up to two decimals (`0.25lh`) |
| `a step holds one page` | put a step's controls inside one `column` |
| `#wizard has 3 steps; the program has 4` | a step was added or removed; the program needs all of its own |

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

The Theme Editor and Crash Reports, the two programs with window
files. In the source tree, `docs/ui-files-design.md` says why the
language is shaped like this, and each program's `.ui` file sits beside
its code.
