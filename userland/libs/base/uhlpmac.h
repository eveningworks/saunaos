#ifndef ULIB_UHLPMAC_H
#define ULIB_UHLPMAC_H

// uhlpmac -- THE WINHELP MACRO LANGUAGE: the text a help file runs when it opens (|SYSTEM's macros and a
// topic's own), when a hotspot is clicked (`JumpId("file.hlp", "ctx")`) or a button is pressed,
// parsed and run against a table of what the viewer can do. The viewer is `/bin/wm/apps/hlpview`;
// this knows nothing of windows or files.
//
// THE LANGUAGE: `Name(arg, arg ...)`, macros separated by `:` or `;`, an argument a "string", a
// `string', a number (decimal or 0x hex) or another macro call whose answer is the value
// (`IfThen(Not(IsMark("seen")), "SaveMark('seen')")`). A string given to IfThen, CreateButton and the like
// is itself a macro text, with its own strings in single quotes. Names are case-insensitive; the
// short forms JI JC PI PC IF IE CB are known.
//
// **IT PARSES UNTRUSTED INPUT** (a help file is). The whole text is parsed BEFORE anything runs, and
// a syntax error runs NONE of it (a parser rejects rather than guesses); nothing is capped by a
// buffer -- strings and the tree are allocated to fit -- except the depth of macros run from
// strings, which is UHLPMAC_DEPTH and is reported when it is hit.
//
// **WHAT IS RUN** is `uhlpmac_ops`: jumps and pop-ups, the viewer's commands, marks, buttons. A macro
// the language has and the viewer does not (printing, annotations, running a program) is reported
// through `unsupported` with the reason, and the rest carries on. ExecProgram is refused ON PURPOSE.

#define UHLPMAC_DEPTH 8     // macros run from a string, from a string, ...: nesting that ends the run

enum uhlpmac_target { UHLPMAC_ID, UHLPMAC_CONTEXT, UHLPMAC_HASH, UHLPMAC_KEYWORD, UHLPMAC_TOC };
enum uhlpmac_cmd { UHLPMAC_CONTENTS = 1, UHLPMAC_SEARCH, UHLPMAC_BACK, UHLPMAC_NEXT, UHLPMAC_PREV,
                   UHLPMAC_HISTORY, UHLPMAC_EXIT, UHLPMAC_COPY, UHLPMAC_BROWSE };
enum uhlpmac_button { UHLPMAC_BTN_CREATE, UHLPMAC_BTN_DESTROY, UHLPMAC_BTN_ENABLE, UHLPMAC_BTN_DISABLE,
                      UHLPMAC_BTN_BIND };
enum uhlpmac_mark { UHLPMAC_MARK_SAVE, UHLPMAC_MARK_DELETE, UHLPMAC_MARK_IS, UHLPMAC_MARK_GOTO };

struct uhlpmac_ops {
    void *ctx;
    // JumpId / JumpContext / JumpHash / JumpKeyword / JumpContents and the Popup forms. `file` is "" for
    // this help file; `id` the context string or keyword; `num` the context number or hash.
    void (*jump)(void *ctx, int popup, int target, const char *file, const char *id, unsigned long num);
    void (*command)(void *ctx, int cmd);                       // UHLPMAC_*
    void (*button)(void *ctx, int op, const char *id, const char *caption, const char *macro);
    // SaveMark / DeleteMark / GotoMark act; IsMark answers 1 or 0.
    int  (*mark)(void *ctx, int op, const char *name);
    // A macro that is known and not done, or text that did not parse: `why` is a short phrase.
    void (*unsupported)(void *ctx, const char *name, const char *why);
};

// Runs `text`. The number of macros run (those nested in an argument or run from a string count too), or -1
// with a static reason in `*err` when it did not parse -- and then none of it ran.
int uhlpmac_run(const char *text, const struct uhlpmac_ops *ops, const char **err);

#endif
