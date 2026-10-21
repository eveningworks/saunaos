// uhlpmac.h: the WinHelp macro language, parsed whole and then run.
#include "lib/uhlpmac.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define PARSE_DEPTH 64          // nested calls in an argument: a hostile `A(A(A(...` must not use the stack up

enum { N_NUM, N_STR, N_CALL };

struct node {
    int kind;
    long num;                    // N_NUM
    char *str;                   // N_STR: the text; N_CALL: the macro's name
    struct node **arg;
    int narg, cap;
};

static void node_free(struct node *n) {
    if (!n) return;
    for (int i = 0; i < n->narg; i++) node_free(n->arg[i]);
    free(n->arg);
    free(n->str);
    free(n);
}

static struct node *node_new(void) { return calloc(1, sizeof(struct node)); }

static int node_add(struct node *n, struct node *a) {
    if (n->narg == n->cap) {
        int nc = n->cap ? n->cap * 2 : 4;
        struct node **g = realloc(n->arg, sizeof *g * (size_t)nc);
        if (!g) return 0;
        n->arg = g;
        n->cap = nc;
    }
    n->arg[n->narg++] = a;
    return 1;
}

// --- the parser -----------------------------------------------------------------------------------------

struct parser { const char *p; const char *err; };

static void skip_ws(struct parser *ps) { while (*ps->p && isspace((unsigned char)*ps->p)) ps->p++; }

static struct node *parse_call(struct parser *ps, int depth);

// A string: "..." with \" and \\, or `...' (nesting) or '...'. The text between, unescaped, allocated.
static struct node *parse_string(struct parser *ps) {
    const char *p = ps->p;
    char open = *p++;
    size_t cap = strlen(p) + 1, n = 0;
    char *out = malloc(cap);
    if (!out) { ps->err = "out of memory"; return 0; }
    int nest = 1;
    for (;;) {
        if (!*p) { free(out); ps->err = "a string is not closed"; return 0; }
        char c = *p++;
        if (open == '"') {
            if (c == '\\' && (*p == '"' || *p == '\\')) { out[n++] = *p++; continue; }
            if (c == '"') break;
        } else if (open == '`') {
            if (c == '`') nest++;
            else if (c == '\'' && --nest == 0) break;
        } else if (c == '\'') {
            break;
        }
        out[n++] = c;
    }
    out[n] = 0;
    ps->p = p;
    struct node *s = node_new();
    if (!s) { free(out); ps->err = "out of memory"; return 0; }
    s->kind = N_STR;
    s->str = out;
    return s;
}

static struct node *parse_number(struct parser *ps) {
    char *end;
    long v = strtol(ps->p, &end, 0);       // decimal, 0x hex, a leading minus
    if (end == ps->p) { ps->err = "a number was expected"; return 0; }
    ps->p = end;
    struct node *n = node_new();
    if (!n) { ps->err = "out of memory"; return 0; }
    n->kind = N_NUM;
    n->num = v;
    return n;
}

static struct node *parse_arg(struct parser *ps, int depth) {
    skip_ws(ps);
    char c = *ps->p;
    if (c == '"' || c == '`' || c == '\'') return parse_string(ps);
    if (isdigit((unsigned char)c) || ((c == '-' || c == '+') && isdigit((unsigned char)ps->p[1]))) return parse_number(ps);
    if (isalpha((unsigned char)c) || c == '_') return parse_call(ps, depth);
    ps->err = "an argument was expected";
    return 0;
}

static struct node *parse_call(struct parser *ps, int depth) {
    if (depth > PARSE_DEPTH) { ps->err = "macros nested too deeply"; return 0; }
    const char *s = ps->p;
    while (isalnum((unsigned char)*ps->p) || *ps->p == '_') ps->p++;
    struct node *call = node_new();
    if (!call) { ps->err = "out of memory"; return 0; }
    call->kind = N_CALL;
    call->str = malloc((size_t)(ps->p - s) + 1);
    if (!call->str) { node_free(call); ps->err = "out of memory"; return 0; }
    memcpy(call->str, s, (size_t)(ps->p - s));
    call->str[ps->p - s] = 0;
    skip_ws(ps);
    if (*ps->p != '(') return call;               // `Contents` alone: a call with no arguments
    ps->p++;
    skip_ws(ps);
    if (*ps->p == ')') { ps->p++; return call; }
    for (;;) {
        struct node *a = parse_arg(ps, depth + 1);
        if (!a) { node_free(call); return 0; }
        if (!node_add(call, a)) { node_free(a); node_free(call); ps->err = "out of memory"; return 0; }
        skip_ws(ps);
        if (*ps->p == ',') { ps->p++; continue; }
        if (*ps->p == ')') { ps->p++; return call; }
        node_free(call);
        ps->err = "a , or ) was expected";
        return 0;
    }
}

// Macros separated by `:` or `;`. The calls, allocated; NULL with `ps->err` set on any error.
static struct node **parse_list(struct parser *ps, int *count) {
    struct node **v = 0;
    int n = 0, cap = 0;
    *count = 0;
    for (;;) {
        skip_ws(ps);
        while (*ps->p == ':' || *ps->p == ';') { ps->p++; skip_ws(ps); }
        if (!*ps->p) break;
        if (!isalpha((unsigned char)*ps->p) && *ps->p != '_') { ps->err = "a macro name was expected"; goto fail; }
        struct node *c = parse_call(ps, 0);
        if (!c) goto fail;
        if (n == cap) {
            int nc = cap ? cap * 2 : 4;
            struct node **g = realloc(v, sizeof *g * (size_t)nc);
            if (!g) { node_free(c); ps->err = "out of memory"; goto fail; }
            v = g;
            cap = nc;
        }
        v[n++] = c;
        skip_ws(ps);
        if (*ps->p && *ps->p != ':' && *ps->p != ';') { ps->err = "a : or ; was expected between macros"; goto fail; }
    }
    *count = n;
    return v ? v : calloc(1, sizeof *v);
fail:
    for (int i = 0; i < n; i++) node_free(v[i]);
    free(v);
    return 0;
}

// --- running it -----------------------------------------------------------------------------------------

struct run {
    const struct uhlpmac_ops *ops;
    int count;
};

enum {
    M_JUMPID, M_JUMPCTX, M_JUMPHASH, M_JUMPKW, M_JUMPTOC,
    M_POPID, M_POPCTX, M_POPHASH,
    M_CONTENTS, M_SEARCH, M_BACK, M_NEXT, M_PREV, M_HISTORY, M_EXIT, M_COPY, M_BROWSE,
    M_CREATEBTN, M_DESTROYBTN, M_ENABLEBTN, M_DISABLEBTN, M_BINDBTN,
    M_SAVEMARK, M_DELMARK, M_ISMARK, M_GOTOMARK, M_NOT, M_IFTHEN, M_IFTHENELSE,
    M_UNSUPPORTED
};

struct macro { const char *name, *abbr; int id; const char *why; };

static const struct macro MACROS[] = {
    { "JumpId", "JI", M_JUMPID, 0 }, { "JumpContext", "JC", M_JUMPCTX, 0 }, { "JumpHash", 0, M_JUMPHASH, 0 },
    { "JumpKeyword", 0, M_JUMPKW, 0 }, { "JumpContents", 0, M_JUMPTOC, 0 },
    { "PopupId", "PI", M_POPID, 0 }, { "PopupContext", "PC", M_POPCTX, 0 }, { "PopupHash", 0, M_POPHASH, 0 },
    { "Contents", 0, M_CONTENTS, 0 }, { "Search", 0, M_SEARCH, 0 }, { "Back", 0, M_BACK, 0 },
    { "Next", 0, M_NEXT, 0 }, { "Prev", 0, M_PREV, 0 }, { "History", 0, M_HISTORY, 0 },
    { "Exit", 0, M_EXIT, 0 }, { "CopyTopic", 0, M_COPY, 0 }, { "BrowseButtons", 0, M_BROWSE, 0 },
    { "CreateButton", "CB", M_CREATEBTN, 0 }, { "DestroyButton", 0, M_DESTROYBTN, 0 },
    { "EnableButton", 0, M_ENABLEBTN, 0 }, { "DisableButton", 0, M_DISABLEBTN, 0 },
    { "ChangeButtonBinding", 0, M_BINDBTN, 0 },
    { "SaveMark", 0, M_SAVEMARK, 0 }, { "DeleteMark", 0, M_DELMARK, 0 }, { "IsMark", 0, M_ISMARK, 0 },
    { "GotoMark", 0, M_GOTOMARK, 0 }, { "Not", 0, M_NOT, 0 },
    { "IfThen", "IF", M_IFTHEN, 0 }, { "IfThenElse", "IE", M_IFTHENELSE, 0 },
    // Known to WinHelp, not done here. ExecProgram is refused on purpose.
    { "ExecProgram", 0, M_UNSUPPORTED, "a help file does not start programs here" },
    { "ExecFile", 0, M_UNSUPPORTED, "a help file does not start programs here" },
    { "RegisterRoutine", 0, M_UNSUPPORTED, "a help file does not load code here" },
    { "Annotate", 0, M_UNSUPPORTED, "annotations are not built yet" },
    { "Print", 0, M_UNSUPPORTED, "printing is not built" }, { "PrinterSetup", 0, M_UNSUPPORTED, "printing is not built" },
    { "SetContents", 0, M_UNSUPPORTED, "a changed contents topic is not kept" },
    { "SetPopupColor", 0, M_UNSUPPORTED, "pop-up colours follow the theme" },
    { "FileOpen", 0, M_UNSUPPORTED, "opening another file by dialog is not built" },
    { "HelpOn", 0, M_UNSUPPORTED, "Help on Help is not here" },
    { "CloseWindow", 0, M_UNSUPPORTED, "secondary windows are not built" },
    { "PositionWindow", 0, M_UNSUPPORTED, "the viewer places its own window" },
    { "FocusWindow", 0, M_UNSUPPORTED, "secondary windows are not built" },
    { "ALink", 0, M_UNSUPPORTED, "associative links are not built" }, { "KLink", 0, M_UNSUPPORTED, "keyword links are not built" },
    { "ShortCut", 0, M_UNSUPPORTED, "no other program is started" },
    { "AppendItem", 0, M_UNSUPPORTED, "menus a file adds are not built" }, { "InsertItem", 0, M_UNSUPPORTED, "menus a file adds are not built" },
    { "InsertMenu", 0, M_UNSUPPORTED, "menus a file adds are not built" }, { "DeleteItem", 0, M_UNSUPPORTED, "menus a file adds are not built" },
    { "EnableItem", 0, M_UNSUPPORTED, "menus a file adds are not built" }, { "DisableItem", 0, M_UNSUPPORTED, "menus a file adds are not built" },
    { "ChangeItemBinding", 0, M_UNSUPPORTED, "menus a file adds are not built" }, { "ResetMenu", 0, M_UNSUPPORTED, "menus a file adds are not built" },
    { "Menu", 0, M_UNSUPPORTED, "menus a file adds are not built" }, { "Generate", 0, M_UNSUPPORTED, "messages to another program are not sent" },
    { "SetHelpOnFile", 0, M_UNSUPPORTED, "Help on Help is not here" }, { "UncheckItem", 0, M_UNSUPPORTED, "menus a file adds are not built" },
    { "CheckItem", 0, M_UNSUPPORTED, "menus a file adds are not built" }, { "Compare", 0, M_UNSUPPORTED, "comparing two files is not built" },
};

static const struct macro *lookup(const char *name) {
    for (size_t i = 0; i < sizeof MACROS / sizeof MACROS[0]; i++)
        if (!strcasecmp(MACROS[i].name, name) || (MACROS[i].abbr && !strcasecmp(MACROS[i].abbr, name))) return &MACROS[i];
    return 0;
}

static void say(struct run *r, const char *name, const char *why) {
    if (r->ops->unsupported) r->ops->unsupported(r->ops->ctx, name, why);
}

static long eval(struct run *r, const struct node *n, int depth);
static int run_text(struct run *r, const char *text, int depth);

// An argument as text: a string as written, a number in decimal, a call by its answer.
static const char *arg_text(struct run *r, const struct node *call, int i, char buf[24], int depth) {
    if (i >= call->narg) return "";
    const struct node *a = call->arg[i];
    if (a->kind == N_STR) return a->str;
    snprintf(buf, 24, "%ld", a->kind == N_NUM ? a->num : eval(r, a, depth));
    return buf;
}

static long arg_num(struct run *r, const struct node *call, int i, int depth) {
    if (i >= call->narg) return 0;
    const struct node *a = call->arg[i];
    if (a->kind == N_NUM) return a->num;
    if (a->kind == N_CALL) return eval(r, a, depth);
    return strtol(a->str, 0, 0);
}

static long eval(struct run *r, const struct node *n, int depth) {
    if (n->kind == N_NUM) return n->num;
    if (n->kind == N_STR) return strtol(n->str, 0, 0);
    const struct uhlpmac_ops *o = r->ops;
    const struct macro *m = lookup(n->str);
    char b0[24], b1[24], b2[24];
    r->count++;
    if (!m) { say(r, n->str, "not a macro this viewer knows"); return 0; }
    // dispatch-ok: THE SET IS THE MACRO LANGUAGE'S OWN, fixed by WinHelp 3.x; each case reads its own arguments.
    switch (m->id) {
    case M_JUMPID: case M_POPID:
        o->jump(o->ctx, m->id == M_POPID, UHLPMAC_ID, arg_text(r, n, 0, b0, depth), arg_text(r, n, 1, b1, depth), 0);
        return 0;
    case M_JUMPCTX: case M_POPCTX:
        o->jump(o->ctx, m->id == M_POPCTX, UHLPMAC_CONTEXT, arg_text(r, n, 0, b0, depth), "", (unsigned long)arg_num(r, n, 1, depth));
        return 0;
    case M_JUMPHASH: case M_POPHASH:
        o->jump(o->ctx, m->id == M_POPHASH, UHLPMAC_HASH, arg_text(r, n, 0, b0, depth), "", (unsigned long)arg_num(r, n, 1, depth));
        return 0;
    case M_JUMPKW:
        o->jump(o->ctx, 0, UHLPMAC_KEYWORD, arg_text(r, n, 0, b0, depth), arg_text(r, n, 1, b1, depth), 0);
        return 0;
    case M_JUMPTOC:
        o->jump(o->ctx, 0, UHLPMAC_TOC, arg_text(r, n, 0, b0, depth), "", 0);
        return 0;
    case M_CONTENTS: o->command(o->ctx, UHLPMAC_CONTENTS); return 0;
    case M_SEARCH: o->command(o->ctx, UHLPMAC_SEARCH); return 0;
    case M_BACK: o->command(o->ctx, UHLPMAC_BACK); return 0;
    case M_NEXT: o->command(o->ctx, UHLPMAC_NEXT); return 0;
    case M_PREV: o->command(o->ctx, UHLPMAC_PREV); return 0;
    case M_HISTORY: o->command(o->ctx, UHLPMAC_HISTORY); return 0;
    case M_EXIT: o->command(o->ctx, UHLPMAC_EXIT); return 0;
    case M_COPY: o->command(o->ctx, UHLPMAC_COPY); return 0;
    case M_BROWSE: o->command(o->ctx, UHLPMAC_BROWSE); return 0;
    case M_CREATEBTN:
        o->button(o->ctx, UHLPMAC_BTN_CREATE, arg_text(r, n, 0, b0, depth), arg_text(r, n, 1, b1, depth), arg_text(r, n, 2, b2, depth));
        return 0;
    case M_DESTROYBTN: o->button(o->ctx, UHLPMAC_BTN_DESTROY, arg_text(r, n, 0, b0, depth), "", ""); return 0;
    case M_ENABLEBTN: o->button(o->ctx, UHLPMAC_BTN_ENABLE, arg_text(r, n, 0, b0, depth), "", ""); return 0;
    case M_DISABLEBTN: o->button(o->ctx, UHLPMAC_BTN_DISABLE, arg_text(r, n, 0, b0, depth), "", ""); return 0;
    case M_BINDBTN: o->button(o->ctx, UHLPMAC_BTN_BIND, arg_text(r, n, 0, b0, depth), "", arg_text(r, n, 1, b1, depth)); return 0;
    case M_SAVEMARK: o->mark(o->ctx, UHLPMAC_MARK_SAVE, arg_text(r, n, 0, b0, depth)); return 0;
    case M_DELMARK: o->mark(o->ctx, UHLPMAC_MARK_DELETE, arg_text(r, n, 0, b0, depth)); return 0;
    case M_GOTOMARK: o->mark(o->ctx, UHLPMAC_MARK_GOTO, arg_text(r, n, 0, b0, depth)); return 0;
    case M_ISMARK: return o->mark(o->ctx, UHLPMAC_MARK_IS, arg_text(r, n, 0, b0, depth)) ? 1 : 0;
    case M_NOT: return arg_num(r, n, 0, depth) ? 0 : 1;
    case M_IFTHEN:
        if (arg_num(r, n, 0, depth)) run_text(r, arg_text(r, n, 1, b1, depth), depth + 1);
        return 0;
    case M_IFTHENELSE:
        run_text(r, arg_text(r, n, arg_num(r, n, 0, depth) ? 1 : 2, b1, depth), depth + 1);
        return 0;
    default:
        say(r, m->name, m->why ? m->why : "not built");
        return 0;
    }
}

static int run_text(struct run *r, const char *text, int depth) {
    if (depth > UHLPMAC_DEPTH) { say(r, "(nested)", "macros nested too deeply"); return 0; }
    struct parser ps = { text, 0 };
    int n;
    struct node **v = parse_list(&ps, &n);
    if (!v) { say(r, "(text)", ps.err ? ps.err : "did not parse"); return 0; }
    for (int i = 0; i < n; i++) {
        eval(r, v[i], depth);
        node_free(v[i]);
    }
    free(v);
    return n;
}

int uhlpmac_run(const char *text, const struct uhlpmac_ops *ops, const char **err) {
    static const char *dummy;
    if (!err) err = &dummy;
    *err = "";
    if (!text || !ops || !ops->jump || !ops->command || !ops->button || !ops->mark) { *err = "no macro or no table"; return -1; }
    struct parser ps = { text, 0 };
    int n;
    struct node **v = parse_list(&ps, &n);
    if (!v) { *err = ps.err ? ps.err : "did not parse"; return -1; }
    struct run r = { ops, 0 };
    for (int i = 0; i < n; i++) {
        eval(&r, v[i], 0);
        node_free(v[i]);
    }
    free(v);
    return r.count;
}
