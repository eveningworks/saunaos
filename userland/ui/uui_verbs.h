#ifndef UUI_VERBS_H
#define UUI_VERBS_H

// THE ICON AND COLOUR EACH VERB IS DRAWN WITH, in every menu that offers
// it -- the File Manager's, the desktop's -- so Copy is one picture in
// one colour wherever it appears (freedesktop's icon naming spec, where
// `edit-copy` names the act and the theme draws it). The colour is the
// theme's ACTION ROLE for what the verb does (ui/utheme.h).
//
// COMPILE-TIME NAMES, NOT A TABLE: a menu is a static const array, and
// a lookup is not a constant expression. UUI_VI_<VERB> is the icon,
// UUI_VT_<VERB> its tint; UUI_MENU_VERB() and UUI_STRIP_VERB() write a
// uui_menu_item with both.
//
//     UUI_MENU_VERB("Open", CMD_OPEN, "Enter", OPEN),
//     UUI_STRIP_VERB("Cut", CMD_CUT, "Ctrl+X", CUT),   // Windows 11's strip
//     (struct context_menu_item){ .icon = UUI_VI_COPY, .tint = UUI_VT_COPY, ... }

#include "ui/utheme.h"
#include "ui/uui_menubar.h"

// Opening and going places.
#define UUI_VI_OPEN        "tb-open"
#define UUI_VT_OPEN        UTHEME_ACT_NAV
#define UUI_VI_OPEN_TAB    "tb-tab"
#define UUI_VT_OPEN_TAB    UTHEME_ACT_NAV
#define UUI_VI_OTHER_PANE  "tb-panes"
#define UUI_VT_OTHER_PANE  UTHEME_ACT_VIEW
#define UUI_VI_RUN         "tb-play"
#define UUI_VT_RUN         UTHEME_ACT_CREATE
#define UUI_VI_TERMINAL    "tb-terminal"
#define UUI_VT_TERMINAL    UTHEME_ACT_NONE
#define UUI_VI_NOTEPAD     "tb-notepad"
#define UUI_VT_NOTEPAD     UTHEME_ACT_EDIT
#define UUI_VI_DISKS       "tb-drive"
#define UUI_VT_DISKS       UTHEME_ACT_NONE

// The clipboard and the file itself.
#define UUI_VI_CUT         "tb-cut"
#define UUI_VT_CUT         UTHEME_ACT_EDIT
#define UUI_VI_COPY        "tb-copy"
#define UUI_VT_COPY        UTHEME_ACT_EDIT
#define UUI_VI_PASTE       "tb-paste"
#define UUI_VT_PASTE       UTHEME_ACT_EDIT
#define UUI_VI_COPY_PATH   "tb-copy"
#define UUI_VT_COPY_PATH   UTHEME_ACT_EDIT
#define UUI_VI_LINK        "tb-link"
#define UUI_VT_LINK        UTHEME_ACT_EDIT
#define UUI_VI_RENAME      "tb-rename"
#define UUI_VT_RENAME      UTHEME_ACT_EDIT
#define UUI_VI_DELETE      "tb-delete"
#define UUI_VT_DELETE      UTHEME_ACT_DANGER
#define UUI_VI_MOVE        "tb-move"
#define UUI_VT_MOVE        UTHEME_ACT_EDIT
#define UUI_VI_NEW_FOLDER  "tb-mkdir"
#define UUI_VT_NEW_FOLDER  UTHEME_ACT_CREATE
#define UUI_VI_NEW_FILE    "tb-new"
#define UUI_VT_NEW_FILE    UTHEME_ACT_CREATE
#define UUI_VI_PROPERTIES  "tb-info"
#define UUI_VT_PROPERTIES  UTHEME_ACT_NONE
#define UUI_VI_SETTINGS    "tb-gear"
#define UUI_VT_SETTINGS    UTHEME_ACT_NONE

// Places.
#define UUI_VI_PIN         "tb-pin"
#define UUI_VT_PIN         UTHEME_ACT_ARRANGE
#define UUI_VI_MOVE_UP     "tb-up"
#define UUI_VT_MOVE_UP     UTHEME_ACT_ARRANGE
#define UUI_VI_MOVE_DOWN   "tb-down"
#define UUI_VT_MOVE_DOWN   UTHEME_ACT_ARRANGE
#define UUI_VI_HIDE        "tb-hide"
#define UUI_VT_HIDE        UTHEME_ACT_ARRANGE
#define UUI_VI_SHOW        "tb-show"
#define UUI_VT_SHOW        UTHEME_ACT_VIEW
#define UUI_VI_TREE        "tb-tree"
#define UUI_VT_TREE        UTHEME_ACT_VIEW

// Volumes and shares.
#define UUI_VI_CHECK       "tb-check"
#define UUI_VT_CHECK       UTHEME_ACT_VIEW
#define UUI_VI_UNMOUNT     "tb-eject"
#define UUI_VT_UNMOUNT     UTHEME_ACT_ARRANGE
#define UUI_VI_SIGN_IN     "tb-key"
#define UUI_VT_SIGN_IN     UTHEME_ACT_CREATE
#define UUI_VI_DISCONNECT  "tb-plug"
#define UUI_VT_DISCONNECT  UTHEME_ACT_ARRANGE
#define UUI_VI_FORGET      "tb-close"
#define UUI_VT_FORGET      UTHEME_ACT_DANGER

// The Recycle Bin and Recent.
#define UUI_VI_RESTORE     "tb-bin-restore"
#define UUI_VT_RESTORE     UTHEME_ACT_CREATE
#define UUI_VI_EMPTY_BIN   "tb-bin-empty"
#define UUI_VT_EMPTY_BIN   UTHEME_ACT_DANGER
#define UUI_VI_CLEAR       "tb-close"
#define UUI_VT_CLEAR       UTHEME_ACT_DANGER

// Tabs, history, selection.
#define UUI_VI_NEW_TAB     "tb-tab"
#define UUI_VT_NEW_TAB     UTHEME_ACT_CREATE
#define UUI_VI_CLOSE       "tb-close"
#define UUI_VT_CLOSE       UTHEME_ACT_NONE
#define UUI_VI_UNDO        "tb-undo"
#define UUI_VT_UNDO        UTHEME_ACT_EDIT
#define UUI_VI_REDO        "tb-redo"
#define UUI_VT_REDO        UTHEME_ACT_EDIT
#define UUI_VI_SELECT_ALL  "tb-check"
#define UUI_VT_SELECT_ALL  UTHEME_ACT_EDIT
#define UUI_VI_REFRESH     "tb-refresh"
#define UUI_VT_REFRESH     UTHEME_ACT_VIEW
#define UUI_VI_SWAP        "tb-panes"
#define UUI_VT_SWAP        UTHEME_ACT_NAV

#define UUI_MENU_VERB(label_, code_, accel_, verb_) \
    UUI_MENU_ICON(label_, code_, accel_, UUI_VI_##verb_, UUI_VT_##verb_)
#define UUI_STRIP_VERB(label_, code_, accel_, verb_) \
    { (label_), (accel_), (code_), 0, 0, UUI_VI_##verb_, UUI_VT_##verb_, UUI_MIS_STRIP }

#endif
