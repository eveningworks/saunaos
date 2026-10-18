# Themes

A THEME is one file that sets every colour the desktop draws with, the
look of the window frame, and a handful of the settings that make up a
desktop's appearance. Picked in System Settings > Appearance, made or
changed in the Theme Editor. Chosen 2026-10-12 as "B, a global theme"
from three scopes (the canvas is linked from `docs/decisions/gui.md`'s
entry on it).

## What real systems do

- **KDE Plasma**: a Global Theme is a BUNDLE -- a colour scheme (an INI
  file of roles per colour set, plus `[WM]` for the title bar), a
  window decoration (Breeze in code, or Aurorae SVG artwork), a widget
  style (a compiled QStyle), icons, cursors, a splash. Each piece is
  also picked on its own.
- **Windows**: a `.theme` file is an INI bundle -- wallpaper, accent,
  light/dark, cursors, sounds -- that writes those settings when
  applied. The artwork half (`.msstyles` visual styles) loads only when
  Microsoft-signed.
- **GNOME (libadwaita), macOS**: light/dark, high contrast and an accent
  colour; no third-party themes.

toy-os copies KDE's and Windows' SHAPE: colour roles plus a few frame
styles drawn in code, plus settings applied as a bundle. It does NOT
draw from artwork (Aurorae, `.msstyles`): that is "Artwork visual
styles" on the roadmap.

## The file

`/usr/share/themes/<stem>.theme` ships (tracked at `data/usr/share/themes/`);
`/etc/themes/<stem>.theme` is made on this machine and WINS over a
shipped one of the same stem -- `/etc` over `/usr`, as everywhere. The
`desktop.theme` setting names a stem.

    # toy-os theme
    Name=Dark
    Description=Dark grey pages and a blue accent

    window_bg=2B2B30          # every struct utheme role, RRGGBB
    accent=3F73BF
    action_danger=EC7B70      # action[] and severity[] by name
    titlebar=1F3F70           # the frame's own roles

    [frame]
    caption_buttons=disc      # disc | rect
    title_align=left          # left | center
    titlebar_pad=8            # px added to the font height, 4..20

    [apply]                   # settings applied with the theme
    desktop.taskbar.theme=dark
    system.cursor.theme=graphite

- **A role the file leaves out is Light's.** A partial theme draws;
  a broken one degrades role by role, the cursor themes' rule.
- **A colour is `RRGGBB`, bare**: `#` starts a comment anywhere on an
  `/etc` line. A value that does not parse is REJECTED, never guessed
  (`12345`, `rgb(...)`): that role is Light's.
- **`[apply]`'s lines are applied only when a person
  applies the theme** (the gallery, the editor's Apply). Each goes
  through the registry, so an undeclared name or a refused value is
  skipped, not written. `config set desktop.theme dark` changes the
  palette and the frame and leaves those settings alone -- Windows'
  split between a `.theme` file and the registry values it writes.

## Live switching

Every ring-3 process already re-reads its appearance settings on
`WIN_EV_SETTING` (`utheme_settings_reload()`); the palette joins them,
so a switch needs no restart and no new event. Widgets resolve colours
at DRAW time (`UUI_COLOR_UNSET`), which is what makes that enough.

## Previews draw with a palette that is not live

`utheme_push()`/`utheme_pop()` swap the palette for the length of one
draw: `uui_themepreview` draws a mini desktop with real widgets under
the theme being previewed, for the Settings gallery and the editor.
Single-threaded drawing makes that safe; a push that is never popped
is the trap, so the preview pushes and pops in one function.

## Picking one, and making one

- **System Settings > Appearance > Theme** is a gallery: a card per
  theme, each a `uui_themepreview` drawn in that theme's colours.
  Apply goes through `utheme_apply()`, so the theme's `[apply]` settings
  come with it. Under the cards: Customize... (the Theme Editor on the
  theme picked), Save as new theme..., Import a theme file... (parsed,
  then written fresh into `/etc/themes`) and Delete (local themes only;
  the theme in use falls back to Light).
- **The Theme Editor** (`/bin/wm/apps/themeedit [stem]`): every role in
  its group with a swatch, the window frame's style and the desktop's
  settings on two more pages, a large preview of the theme being edited,
  and an inspector -- the colour picker (`uui_colourpick`) with each
  role's WCAG contrast against what it is read on. Save on a shipped
  theme is Save as; Apply saves first.
