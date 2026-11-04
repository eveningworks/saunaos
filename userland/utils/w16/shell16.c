// shell16.c -- SHELL's program-starting and icon calls, and KERNEL's WinExec: what Program
// Manager is built on. ExtractIcon and its kin read icons out of other programs' files;
// FindExecutable and ShellExecute go from a document to the program WIN.INI's [extensions] names
// for it; WinExec starts a program.
//
// A PROGRAM STARTED IS A SECOND w16, a process of its own, as NT's "separate memory space" ran a
// 16-bit program in an NTVDM of its own -- not a task in this one, as 3.1 had it. It is not started
// from here: w16 is sandboxed, and a child would inherit an unveil list it could not add its own
// paths to. The main thread asks the compositor to start it (uapp_launch(), WIN_REQ_LAUNCH), which
// starts nothing but /bin/w16 with an .EXE that exists -- the shape of a desktop portal (Flatpak's
// xdg-desktop-portal). docs/decisions/gui.md, "A Win16 program starts another through the
// compositor".
#include "w16_internal.h"
#include "common/w16prefs.h"     // W16_PATH_MAX: a DOS path, as 3.1 bounded one

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define HOST_MAX 512                     // a SaunaOS path a DOS one maps to

static int same_ci(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) return 0;
    return !*a && !*b;
}

static const char *ext_of(const char *name) {
    const char *slash = strrchr(name, '\\'), *dot = strrchr(name, '.');
    return dot && (!slash || dot > slash) ? dot + 1 : "";
}

// WHERE A NAME WITHOUT A FOLDER IS LOOKED FOR, as Windows' OpenFile searched: the current folder,
// the Windows folder and its SYSTEM, the program's own folder. `host` the file's SaunaOS path,
// `full` its DOS one; 0, or a DOS error.
static int find_file(struct w16 *w, const char *name, char *host, size_t hcap, char *full, size_t fcap) {
    if (strchr(name, '\\') || strchr(name, ':')) {
        int e = w16_dos_path(name, host, hcap, 0, full, fcap);
        struct stat st;
        return e ? e : stat(host, &st) == 0 && S_ISREG(st.st_mode) ? 0 : 2;
    }
    char own[W16_PATH_MAX];
    own[0] = 0;
    if (w16_host_to_dos(w->path, own, sizeof own) == 0) {
        char *b = strrchr(own, '\\');
        if (b) b[1] = 0;
    }
    const char *dirs[] = { "", "C:\\WINDOWS\\", "C:\\WINDOWS\\SYSTEM\\", own };
    for (unsigned i = 0; i < sizeof dirs / sizeof dirs[0]; i++) {
        char dos[W16_PATH_MAX * 2];
        snprintf(dos, sizeof dos, "%s%s", dirs[i], name);
        struct stat st;
        if (w16_dos_path(dos, host, hcap, 0, full, fcap) == 0 && stat(host, &st) == 0 && S_ISREG(st.st_mode)) return 0;
    }
    return 2;
}

static uint8_t *slurp(const char *host, uint32_t *size) {
    FILE *f = fopen(host, "rb");
    if (!f) return 0;
    uint8_t *b = 0;
    if (fseek(f, 0, SEEK_END) == 0) {
        long n = ftell(f);
        if (n > 0 && n < 16L * 1024 * 1024 && fseek(f, 0, SEEK_SET) == 0 && (b = malloc((size_t)n))) {
            if (fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = 0; }
            else *size = (uint32_t)n;
        }
    }
    fclose(f);
    return b;
}

static unsigned u16le(const uint8_t *p) { return (unsigned)(p[0] | p[1] << 8); }
static uint32_t u32le(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

// THE ICONS IN A FILE, as ExtractIcon counts them: an NE program's or DLL's icon groups in table
// order, or an .ICO's one. Icon `index`'s picture into px; the count, 0 for a file with none, -1 for
// one that is no program and no icon (a document, a DOS program).
static int file_icons(const char *host, int index, uint32_t *px, int *iw, int *ih) {
    uint32_t size = 0;
    uint8_t *img = slurp(host, &size);
    if (!img) return -1;
    int n = -1;
    if (size >= 6 && u16le(img) == 0 && u16le(img + 2) == 1 && u16le(img + 4) > 0) {   // an .ICO
        unsigned count = u16le(img + 4), pick = 0;
        if (6 + 16u * count <= size) {
            for (unsigned i = 0; i < count; i++)
                if (img[6 + 16 * i] == 32 && img[6 + 16 * i + 1] == 32) { pick = i; break; }
            const uint8_t *e = img + 6 + 16 * pick;
            uint32_t len = u32le(e + 8), off = u32le(e + 12);
            n = 1;
            if (index == 0 && px && (off > size || len > size - off || w16_icon_dib(img + off, len, px, iw, ih) < 0)) n = -1;
        }
    } else {
        struct ne_module *m = calloc(1, sizeof *m);
        if (m && ne_parse(img, size, m) == 0) {
            n = 0;
            struct ne_resource r;
            for (unsigned i = 0; i < m->nres; i++)
                if (ne_resource_at(m, i, &r) == 0 && !r.type_name[0] && r.type == NE_RT_GROUP_ICON) {
                    if (n == index && px && w16_icon_group(m, &r, px, iw, ih) < 0) n = -2;
                    if (n == -2) break;
                    n++;
                }
            if (n == -2) n = -1;
        }
        free(m);
    }
    free(img);
    return n;
}

// ExtractIcon's work: icon `index` of the file a program named (-1: how many there are), as a
// block. 0 none, 1 the file is no program (Windows' own answer).
static uint16_t extract(struct w16 *w, const char *name, int index) {
    char host[HOST_MAX], full[W16_PATH_MAX];
    if (find_file(w, name, host, sizeof host, full, sizeof full)) return 0;
    static uint32_t px[64 * 64];
    int iw = 0, ih = 0, n = file_icons(host, index < 0 ? -1 : index, index < 0 ? 0 : px, &iw, &ih);
    if (n < 0) return 1;
    if (index < 0) return (uint16_t)n;
    if (index >= n || iw <= 0) return 0;
    return w16_icon_block_new(w, px, iw, ih);
}

static int far_name(struct w16 *w, uint32_t far, char *out, size_t cap) {
    out[0] = 0;
    return (far >> 16) && cpu16_str(&w->cpu, (uint16_t)(far >> 16), (uint16_t)far, out, (unsigned)cap) >= 0 ? 0 : -1;
}

// ExtractIcon(hInst, lpszExeFileName, nIconIndex) -> HICON
// Windows: Icon nIconIndex of a program, a DLL or an .ICO file -- -1 for how many it has; NULL when
// it has none there, 1 when the file is no program.
// w16: The same, as an icon block in 3.1's layout; a bare name is looked for as OpenFile did.
static int api_ExtractIcon(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w16_arg16(a);
    uint32_t far = w16_arg32(a);
    int index = (int16_t)w16_arg16(a);
    char name[W16_PATH_MAX];
    if (far_name(w, far, name, sizeof name) < 0) return -1;
    w->cpu.r[R_AX] = extract(w, name, index);
    return 8;
}

// InternalExtractIcon(hInst, lpszExeName, nIconIndex, nIcons) -> HGLOBAL
// Windows: Undocumented, Program Manager's: nIcons icons of a file from nIconIndex on, as a block of
// HICONs.
// w16: The same.
static int api_InternalExtractIcon(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w16_arg16(a);
    uint32_t far = w16_arg32(a);
    int index = (int16_t)w16_arg16(a), n = (int16_t)w16_arg16(a);
    char name[W16_PATH_MAX];
    if (far_name(w, far, name, sizeof name) < 0) return -1;
    w->cpu.r[R_AX] = 0;
    if (n <= 0 || n > 512) return 10;
    uint16_t *v = calloc((size_t)n, sizeof *v);
    if (!v) return 10;
    for (int i = 0; i < n; i++) {
        uint16_t h = extract(w, name, index + i);
        v[i] = h > 1 ? h : 0;
    }
    w->cpu.r[R_AX] = w16_global_alloc(w, (uint32_t)n * 2, v);
    free(v);
    return 10;
}

// FINDEXECUTABLE'S WORK: the program a file opens with -- itself when it is one, else the one
// WIN.INI's [extensions] names for its extension ("txt=notepad.exe ^.txt"), found as a program is.
// >32 found (its DOS path in `exe`), 2 no such file, 31 no program for it.
static int find_executable(struct w16 *w, const char *file, char *exe, size_t cap) {
    char host[HOST_MAX], full[W16_PATH_MAX];
    exe[0] = 0;
    if (find_file(w, file, host, sizeof host, full, sizeof full)) return 2;
    const char *ext = ext_of(full);
    static const char *const run[] = { "EXE", "COM", "BAT", "PIF" };
    for (unsigned i = 0; i < sizeof run / sizeof run[0]; i++)
        if (same_ci(ext, run[i])) {
            snprintf(exe, cap, "%s", full);
            return 33;
        }
    char line[W16_PATH_MAX], lower[16];
    snprintf(lower, sizeof lower, "%s", ext);
    for (char *p = lower; *p; p++) *p = (char)tolower((unsigned char)*p);
    if (!ext[0] || w16_ini_get("WIN.INI", "extensions", lower, line, sizeof line) <= 0) return 31;
    char *sp = strchr(line, ' ');
    if (sp) *sp = 0;
    char eh[HOST_MAX], ef[W16_PATH_MAX];
    if (find_file(w, line, eh, sizeof eh, ef, sizeof ef)) return 31;
    snprintf(exe, cap, "%s", ef);
    return 33;
}

// FindExecutable(lpFile, lpDirectory, lpResult) -> HINSTANCE
// Windows: The program a file opens with, by its extension in WIN.INI's [extensions] -- itself
// for a program; >32 found, 2 no such file, 31 no program for it.
// w16: The same, a relative file looked for in lpDirectory first.
static int api_FindExecutable(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint32_t ff = w16_arg32(a), fd = w16_arg32(a), fr = w16_arg32(a);
    char file[W16_PATH_MAX], dir[W16_PATH_MAX], path[W16_PATH_MAX * 2], exe[W16_PATH_MAX];
    if (far_name(w, ff, file, sizeof file) < 0) return -1;
    far_name(w, fd, dir, sizeof dir);
    char host[HOST_MAX], full[W16_PATH_MAX];
    snprintf(path, sizeof path, "%s%s%s", dir, dir[0] && dir[strlen(dir) - 1] != '\\' ? "\\" : "", file);
    int r = find_executable(w, dir[0] && !strchr(file, '\\') && !find_file(w, path, host, sizeof host, full, sizeof full)
                            ? path : file, exe, sizeof exe);
    uint8_t *p = (fr >> 16) ? cpu16_ptr(&w->cpu, (uint16_t)(fr >> 16), (uint16_t)fr, (uint32_t)strlen(exe) + 1, 1) : 0;
    if (p) memcpy(p, exe, strlen(exe) + 1);
    w->cpu.r[R_AX] = (uint16_t)r;
    return 12;
}

// --- starting a program ------------------------------------------------------------------

// The one launch in flight: the task waits while the main thread asks the compositor.
static pthread_mutex_t g_launch_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_launch_exe[HOST_MAX], g_launch_args[256];
static int g_launch_state, g_launch_result;     // 1 asked, 2 answered
static unsigned g_launch_gen;                   // which launch: an answer to one given up on is dropped

unsigned w16_launch_take(char *exe, size_t ecap, char *args, size_t acap) {
    pthread_mutex_lock(&g_launch_lock);
    unsigned gen = g_launch_state == 1 ? g_launch_gen : 0;
    if (gen) {
        snprintf(exe, ecap, "%s", g_launch_exe);
        snprintf(args, acap, "%s", g_launch_args);
    }
    pthread_mutex_unlock(&g_launch_lock);
    return gen;
}

void w16_launch_done(unsigned gen, int result) {
    pthread_mutex_lock(&g_launch_lock);
    if (gen == g_launch_gen && g_launch_state == 1) {
        g_launch_result = result;
        g_launch_state = 2;
    }
    pthread_mutex_unlock(&g_launch_lock);
}

// WINEXEC'S WORK: a command line's program, found as Windows looked for one (".EXE" added when it
// has no extension), started as a second w16 with the rest of the line as its own. 33 started, 2 no
// such file, 11 no Windows program (a DOS one: w16 runs none), 5 not started -- a program asked to
// start hidden, an agent no other program could reach across w16s (Hearts' NETDDE.EXE).
int w16_launch(struct w16 *w, const char *cmdline, int show) {
    while (*cmdline == ' ') cmdline++;
    char prog[W16_PATH_MAX];
    size_t n = strcspn(cmdline, " ");
    if (!n || n >= sizeof prog - 4) return 2;
    memcpy(prog, cmdline, n);
    prog[n] = 0;
    const char *args = cmdline + n;
    while (*args == ' ') args++;
    if (!ext_of(prog)[0]) strcat(prog, ".EXE");
    char host[HOST_MAX], full[W16_PATH_MAX];
    if (find_file(w, prog, host, sizeof host, full, sizeof full)) return 2;
    uint32_t size = 0;
    uint8_t *img = slurp(host, &size);
    struct ne_module *m = img ? calloc(1, sizeof *m) : 0;
    int ne = m && ne_parse(img, size, m) == 0;
    free(m);
    free(img);
    if (!ne) return 11;
    if (show == 0) return 5;                     // SW_HIDE
    pthread_mutex_lock(&g_launch_lock);
    while (g_launch_state == 1) {                // one at a time
        pthread_mutex_unlock(&g_launch_lock);
        struct timespec ts = { 0, 5000000 };
        nanosleep(&ts, 0);
        pthread_mutex_lock(&g_launch_lock);
    }
    snprintf(g_launch_exe, sizeof g_launch_exe, "%s", host);
    snprintf(g_launch_args, sizeof g_launch_args, "%s", args);
    if (!++g_launch_gen) g_launch_gen = 1;       // 0 is take()'s "none"
    g_launch_state = 1;
    pthread_mutex_unlock(&g_launch_lock);
    if (!w16_task_post_arg(w, W16_POST_LAUNCH, 0)) {
        pthread_mutex_lock(&g_launch_lock);
        g_launch_state = 0;
        pthread_mutex_unlock(&g_launch_lock);
        return 8;                                // out of memory, as 3.1 said when it could not
    }
    // Ten seconds at most, in short sleeps: SaunaOS's condition variable has no timed wait.
    pthread_mutex_lock(&g_launch_lock);
    for (int waited = 0; g_launch_state == 1 && waited < 10000; waited += 5) {
        pthread_mutex_unlock(&g_launch_lock);
        struct timespec ts = { 0, 5000000 };
        nanosleep(&ts, 0);
        pthread_mutex_lock(&g_launch_lock);
    }
    int r = g_launch_state == 2 ? g_launch_result : -1;
    g_launch_state = 0;
    pthread_mutex_unlock(&g_launch_lock);
    if (g_w16_trace) {
        fprintf(g_w16_trace, "  WinExec %s %s -> %d\n", host, args, r);
        fflush(g_w16_trace);
    }
    return r > 0 ? 33 : 2;
}

// ShellExecute(hwnd, lpOperation, lpFile, lpParameters, lpDirectory, iShowCmd) -> HINSTANCE
// Windows: Opens a file -- a program started with lpParameters, a document with the program
// FindExecutable finds for it -- in lpDirectory; >32 done, else an error as WinExec's.
// w16: The same for "open" (or none) through WinExec's second w16; "print" is refused (31), there
// being no printer.
static int api_ShellExecute(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w16_arg16(a);
    uint32_t fo = w16_arg32(a), ff = w16_arg32(a), fp = w16_arg32(a), fd = w16_arg32(a);
    int show = (int16_t)w16_arg16(a);
    char op[16], file[W16_PATH_MAX], params[256], dir[W16_PATH_MAX], exe[W16_PATH_MAX], line[W16_PATH_MAX * 3];
    if (far_name(w, ff, file, sizeof file) < 0) return -1;
    far_name(w, fo, op, sizeof op);
    far_name(w, fp, params, sizeof params);
    far_name(w, fd, dir, sizeof dir);
    w->cpu.r[R_AX] = 31;
    if (op[0] && !same_ci(op, "open")) return 20;
    char was[W16_PATH_MAX];
    snprintf(was, sizeof was, "%s", w16_dos_cwd());
    if (dir[0]) w16_dos_chdir(dir);
    int r = find_executable(w, file, exe, sizeof exe);
    if (r > 32) {
        char host[HOST_MAX], full[W16_PATH_MAX];
        find_file(w, file, host, sizeof host, full, sizeof full);
        if (same_ci(exe, full)) snprintf(line, sizeof line, "%s %s", full, params);
        else snprintf(line, sizeof line, "%s %s", exe, full);
        r = w16_launch(w, line, show);
    }
    if (dir[0]) w16_dos_chdir(was);
    w->cpu.r[R_AX] = (uint16_t)r;
    return 20;
}

// The icon WIN_EXTRACTASSOC falls back to: icon `id` of the asking program (2 a DOS program's, 6 a
// document's in Program Manager), as a block, its path into `path`.
static uint16_t own_icon(struct w16 *w, uint16_t id, char *path, size_t cap) {
    uint16_t h = w16_icon_handle(w, &w->ne, id, 0);
    w16_host_to_dos(w->path, path, cap);
    return h ? w16_icon_copy(w, h) : 0;
}

// ExtractAssociatedIcon(hInst, lpIconPath, lpiIcon) -> HICON
// Windows: The icon for a file: its own icon *lpiIcon when it is a program with icons, else the
// first icon of the program it opens with (lpIconPath and *lpiIcon changed to say so), else one of
// the asking program's own -- 2 for a DOS program, 6 for anything else.
// w16: The same; the icon is a block in 3.1's layout.
static int api_ExtractAssociatedIcon(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w16_arg16(a);
    uint32_t fp = w16_arg32(a), fi = w16_arg32(a);
    char path[W16_PATH_MAX], exe[W16_PATH_MAX];
    if (far_name(w, fp, path, sizeof path) < 0) return -1;
    uint8_t *pi = (fi >> 16) ? cpu16_ptr(&w->cpu, (uint16_t)(fi >> 16), (uint16_t)fi, 2, 1) : 0;
    int index = pi ? (int16_t)u16le(pi) : 0;
    uint16_t h = extract(w, path, index), out = 0, newi = (uint16_t)index;
    char newpath[W16_PATH_MAX];
    snprintf(newpath, sizeof newpath, "%s", path);
    if (h > 1) {
        out = h;
    } else {
        if (h == 1 && find_executable(w, path, exe, sizeof exe) > 32 && !same_ci(exe, path)) {
            uint16_t e = extract(w, exe, 0);
            if (e > 1) { out = e; newi = 0; snprintf(newpath, sizeof newpath, "%s", exe); }
        }
        if (!out) {
            const char *x = ext_of(path);
            newi = (uint16_t)(same_ci(x, "EXE") || same_ci(x, "COM") || same_ci(x, "BAT") || same_ci(x, "PIF") ? 2 : 6);
            out = own_icon(w, newi, newpath, sizeof newpath);
        }
    }
    if (out && strcmp(newpath, path)) {
        uint8_t *p = cpu16_ptr(&w->cpu, (uint16_t)(fp >> 16), (uint16_t)fp, 128, 1);
        if (p) snprintf((char *)p, 128, "%s", newpath);
    }
    if (out && (pi = (fi >> 16) ? cpu16_ptr(&w->cpu, (uint16_t)(fi >> 16), (uint16_t)fi, 2, 1) : 0)) {
        pi[0] = (uint8_t)newi;
        pi[1] = (uint8_t)(newi >> 8);
    }
    w->cpu.r[R_AX] = out;
    return 10;
}

// DoEnvironmentSubst(szString, cbString) -> DWORD
// Windows: Each %NAME% in the string replaced by the environment's value, in place; the new length
// with TRUE, or cbString with FALSE when it would not fit.
// w16: The same, from the program's DOS environment (PATH, TEMP); a name it lacks is left as written.
static int api_DoEnvironmentSubst(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint32_t far = w16_arg32(a);
    unsigned cap = w16_arg16(a);
    struct cpu16 *c = &w->cpu;
    char *p = cap ? (char *)cpu16_ptr(c, (uint16_t)(far >> 16), (uint16_t)far, cap, 1) : 0;
    c->r[R_AX] = (uint16_t)cap;
    c->r[R_DX] = 0;
    if (!p) return 6;
    const uint8_t *psp = cpu16_ptr(c, w->psp, 0x2C, 2, 0);
    uint16_t envsel = psp ? (uint16_t)(psp[0] | psp[1] << 8) : 0;
    char *out = malloc(cap + 1);
    if (!out) return 6;
    unsigned o = 0, ok = 1;
    for (unsigned i = 0; i < cap && p[i] && ok; ) {
        char *end = p[i] == '%' ? memchr(p + i + 1, '%', cap - i - 1) : 0;
        const char *val = 0;
        if (end && envsel) {
            size_t nl = (size_t)(end - (p + i + 1));
            for (uint32_t at = 0; at < 0x8000 && !val; ) {
                const char *e = (const char *)cpu16_ptr(c, envsel, (uint16_t)at, 1, 0);
                if (!e || !*e) break;
                size_t el = 0;
                while (at + el < 0x8000 && cpu16_ptr(c, envsel, (uint16_t)(at + el), 1, 0) && e[el]) el++;
                if (el > nl && e[nl] == '=' && !strncasecmp(e, p + i + 1, nl)) val = e + nl + 1;
                at += (uint32_t)el + 1;
            }
        }
        if (val) {
            size_t vl = strlen(val);
            if (o + vl >= cap) ok = 0;
            else { memcpy(out + o, val, vl); o += (unsigned)vl; }
            i = (unsigned)(end - p) + 1;
        } else {
            if (o + 1 >= cap) ok = 0;
            else out[o++] = p[i];
            i++;
        }
    }
    if (ok) {
        out[o] = 0;
        memcpy(p, out, o + 1);
        c->r[R_AX] = (uint16_t)o;
        c->r[R_DX] = 1;
    }
    free(out);
    return 6;
}

// RegisterShellHook(hWnd, fInstall) -> BOOL
// Windows: Undocumented, the shell's: hWnd is told as top-level windows are made and destroyed --
// Program Manager registers itself, as Windows' shell.
// w16: Answers TRUE and tells it nothing.
// Why: The windows it would hear of are other programs', each in a w16 of its own.
// Notice: Nothing: Program Manager only takes note of them.
// Whole: Other programs' windows reported across w16s.
static int api_RegisterShellHook(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w16_arg16(a);
    w16_arg16(a);
    w->cpu.r[R_AX] = 1;
    return 4;
}

// DragQueryPoint(hDrop, lppt) -> BOOL
// Windows: Where in the window's client area files were dropped; TRUE inside it.
// w16: Answers FALSE: nothing is ever dropped (DragAcceptFiles).
// Why: A file dropped on a program is the roadmap's "w16: a file dropped on a program
// (WM_DROPFILES)".
// Notice: Nothing: no program is sent WM_DROPFILES to ask it.
// Whole: Drops from SaunaOS's File Manager made WM_DROPFILES.
static int api_DragQueryPoint(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w16_arg16(a);
    w16_arg32(a);
    w->cpu.r[R_AX] = 0;
    return 6;
}

const struct w16_api g_shell_files_api[] = {
    { 13, api_DragQueryPoint },
    { 20, api_ShellExecute },
    { 21, api_FindExecutable },
    { 34, api_ExtractIcon },
    { 36, api_ExtractAssociatedIcon },
    { 37, api_DoEnvironmentSubst },
    { 39, api_InternalExtractIcon },
    { 102, api_RegisterShellHook },
};
const unsigned g_shell_files_api_n = sizeof g_shell_files_api / sizeof g_shell_files_api[0];
