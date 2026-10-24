// file16.c -- a Windows program's files: the drives it sees, KERNEL's
// file calls (OpenFile, _lopen, _lread...) and the DOS ones (INT 21h)
// behind them, on toy-os files.
//
// TWO DRIVES, Wine's shape: C: is the Win16 folder (its WINDOWS holds the
// INI files), Z: is the whole toy-os tree -- so /tests/sample.txt is
// Z:\tests\sample.txt. A program starts in the folder it was run from.
// DOS names are matched WITHOUT CASE, one component at a time, so a
// program's "Z:\TESTS\SAMPLE.TXT" finds toy-os's lower-case file; a new
// file keeps the case it was given.
//
// A DOS FILE HANDLE is 5 or more (0-4 were DOS's standard ones), an index
// into a table that grows as files are opened. A TOY-OS DESCRIPTOR EITHER
// READS OR WRITES (fcntl.h's O_RDWR), so a handle opened for both is two
// of them, and the handle keeps the file position they share.
#include "w16_internal.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "w16/w16prefs.h"
#include "rt/sys.h"

#define OF_READ      0x0000
#define OF_WRITE     0x0001
#define OF_READWRITE 0x0002
#define OF_PARSE     0x0100
#define OF_DELETE    0x0200
#define OF_VERIFY    0x0400
#define OF_CREATE    0x1000
#define OF_EXIST     0x4000
#define OF_REOPEN    0x8000
#define HFILE_ERROR  0xFFFF
#define DOS_PATH     128              // OFSTRUCT's szPathName: the ABI's bound on a path
#define FIRST_HANDLE 5

// DOS's error codes, what nErrCode and INT 21h's AX say.
#define E_FILE   2
#define E_PATH   3
#define E_ACCESS 5
#define E_HANDLE 6
#define E_DRIVE  15

static char g_cwd[DOS_PATH] = "C:\\";
static struct dosfile { int rfd, wfd; off_t pos; char *host; } *g_files;
static int g_nfiles;

// --- paths --------------------------------------------------------------------------

static char upper(char c) { return c >= 'a' && c <= 'z' ? (char)(c - 32) : c; }

static int same_ci(const char *a, const char *b) {
    while (*a && *b && upper(*a) == upper(*b)) { a++; b++; }
    return !*a && !*b;
}

// The drive's root on the host: "" for Z: (so that "/" + rest is right),
// 0 for a letter that is no drive.
const char *w16_drive_root(char d) {
    d = upper(d);
    if (d == 'C') return g_w16_drive_c;
    if (d == 'Z') return "";
    if (d >= 'D' && d < 'D' + W16_DRIVES && g_w16_prefs.drive[d - 'D'][0]) return g_w16_prefs.drive[d - 'D'];
    return 0;
}
#define drive_root w16_drive_root

// A host path on the drive whose folder holds it most closely, else Z:.
int w16_host_to_dos(const char *host, char *out, size_t cap) {
    const char *rest = host;
    char drive = 'Z';
    size_t best = 0;
    for (char d = 'C'; d < 'Z'; d++) {
        const char *root = drive_root(d);
        size_t n = root ? strlen(root) : 0;
        if (n > best && !strncmp(host, root, n) && (host[n] == '/' || !host[n])) {
            best = n;
            drive = d;
            rest = host + n;
        }
    }
    if ((size_t)snprintf(out, cap, "%c:%s", drive, rest[0] ? rest : "\\") >= cap) return -1;
    for (char *p = out; *p; p++) if (*p == '/') *p = '\\';
    return 0;
}

// The full DOS path of `dos` (relative to the current folder, or rooted),
// its "." and ".." taken out; 0, or a DOS error.
static int full_dos(const char *dos, char *out, size_t cap) {
    char buf[2 * DOS_PATH + 4];
    if (dos[0] && dos[1] == ':') {
        if (!drive_root(dos[0])) return E_DRIVE;
        if (dos[2] == '\\' || dos[2] == '/') snprintf(buf, sizeof buf, "%s", dos);
        else if (upper(dos[0]) == upper(g_cwd[0])) snprintf(buf, sizeof buf, "%s\\%s", g_cwd, dos + 2);
        else snprintf(buf, sizeof buf, "%c:\\%s", upper(dos[0]), dos + 2);
    } else if (dos[0] == '\\' || dos[0] == '/') {
        snprintf(buf, sizeof buf, "%c:%s", g_cwd[0], dos);
    } else {
        snprintf(buf, sizeof buf, "%s\\%s", g_cwd, dos);
    }
    // Components, one by one, "." and ".." resolved; never above the root.
    char *seg[64];
    int n = 0;
    for (char *p = buf + 2; *p; p++) if (*p == '/') *p = '\\';
    for (char *tok = strtok(buf + 3, "\\"); tok; tok = strtok(0, "\\")) {
        if (!strcmp(tok, ".")) continue;
        if (!strcmp(tok, "..")) { if (n) n--; continue; }
        if (n == 64) return E_PATH;
        seg[n++] = tok;
    }
    size_t len = (size_t)snprintf(out, cap, "%c:", upper(buf[0]));
    for (int i = 0; i < n && len < cap; i++) len += (size_t)snprintf(out + len, cap - len, "\\%s", seg[i]);
    if (!n && len < cap) len += (size_t)snprintf(out + len, cap - len, "\\");
    return len < cap ? 0 : E_PATH;
}

// A full DOS path's host path, each existing component in the case the
// disk has it; the last may be missing when `creating`. 0 or a DOS error.
static int host_of(const char *full, char *out, size_t cap, int creating) {
    const char *root = drive_root(full[0]);
    if (!root) return E_DRIVE;
    size_t len = (size_t)snprintf(out, cap, "%s", root);
    const char *p = full + 2;
    while (*p == '\\') p++;
    while (*p) {
        const char *end = strchr(p, '\\');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        char name[256];
        if (n >= sizeof name) return E_PATH;
        memcpy(name, p, n);
        name[n] = 0;
        int last = !end || !end[1];
        // The entry that matches without case, if any; else the name as given.
        const char *found = 0;
        DIR *d = opendir(len ? out : "/");
        struct dirent *de;
        while (d && (de = readdir(d))) if (same_ci(de->d_name, name)) { found = de->d_name; break; }
        if (!found && !(last && creating)) { if (d) closedir(d); return last ? E_FILE : E_PATH; }
        if ((size_t)snprintf(out + len, cap - len, "/%s", found ? found : name) >= cap - len) {
            if (d) closedir(d);
            return E_PATH;
        }
        len += strlen(out + len);
        if (d) closedir(d);
        p += n;
        while (*p == '\\') p++;
    }
    if (!len) snprintf(out, cap, "/");
    return 0;
}

int w16_dos_path(const char *dos, char *host, size_t cap, int creating, char *full, size_t full_cap) {
    char f[DOS_PATH * 2];
    int e = full_dos(dos, f, sizeof f);
    if (e) return e;
    if (full) snprintf(full, full_cap, "%s", f);
    return host_of(f, host, cap, creating);
}

const char *w16_dos_cwd(void) { return g_cwd; }

int w16_dos_chdir(const char *dos) {
    char host[1024], full[DOS_PATH * 2];
    struct stat st;
    int e = w16_dos_path(dos, host, sizeof host, 0, full, sizeof full);
    if (e) return e;
    if (stat(host, &st) != 0 || !S_ISDIR(st.st_mode)) return E_PATH;
    snprintf(g_cwd, sizeof g_cwd, "%s", full);
    return 0;
}

void w16_dos_start(const char *exe) {
    char dir[512];
    snprintf(dir, sizeof dir, "%s", exe);
    char *slash = strrchr(dir, '/');
    if (slash && slash != dir) *slash = 0;
    else snprintf(dir, sizeof dir, "/");
    if (w16_host_to_dos(dir, g_cwd, sizeof g_cwd) < 0) snprintf(g_cwd, sizeof g_cwd, "C:\\");
    size_t n = strlen(g_cwd);
    if (n > 3 && g_cwd[n - 1] == '\\') g_cwd[n - 1] = 0;
}

// --- handles --------------------------------------------------------------------

static int add_handle(int rfd, int wfd, const char *host) {
    int i = 0;
    while (i < g_nfiles && (g_files[i].rfd >= 0 || g_files[i].wfd >= 0)) i++;
    if (i == g_nfiles) {
        struct dosfile *g = realloc(g_files, sizeof *g * (size_t)(g_nfiles + 1));
        if (!g) return -1;
        g_files = g;
        g_nfiles++;
    }
    g_files[i] = (struct dosfile){ rfd, wfd, 0, strdup(host) };
    return i + FIRST_HANDLE;
}

static struct dosfile *file_of(unsigned h) {
    if (h < FIRST_HANDLE || h - FIRST_HANDLE >= (unsigned)g_nfiles) return 0;
    struct dosfile *f = &g_files[h - FIRST_HANDLE];
    return f->rfd >= 0 || f->wfd >= 0 ? f : 0;
}

static int close_handle(unsigned h) {
    struct dosfile *f = file_of(h);
    if (!f) return -1;
    if (f->rfd >= 0) close(f->rfd);
    if (f->wfd >= 0) close(f->wfd);
    free(f->host);
    *f = (struct dosfile){ -1, -1, 0, 0 };
    return 0;
}

static off_t file_size(struct dosfile *f) {
    struct stat st;
    return fstat(f->rfd >= 0 ? f->rfd : f->wfd, &st) == 0 ? st.st_size : -1;
}

static off_t seek(struct dosfile *f, off_t off, int origin) {
    off_t base = origin == 0 ? 0 : origin == 1 ? f->pos : file_size(f);
    if (base < 0 || base + off < 0) return -1;
    return f->pos = base + off;
}

// Open a DOS path the DOS way: mode 0 read, 1 write, 2 both; create
// truncates. A handle, or -(DOS error).
static int open_dos(const char *dos, int mode, int create, char *full, size_t full_cap) {
    char host[1024];
    int e = w16_dos_path(dos, host, sizeof host, create, full, full_cap);
    if (e) return -e;
    struct stat st;
    if (!create && stat(host, &st) == 0 && S_ISDIR(st.st_mode)) return -E_ACCESS;
    // The writer first: creating makes the file the reader then opens.
    int writes = create || (mode & 3) != 0, reads = create || (mode & 3) != 1;
    int wfd = writes ? open(host, O_WRONLY | (create ? O_CREAT | O_TRUNC : 0), 0644) : -1;
    int rfd = reads && (!writes || wfd >= 0) ? open(host, O_RDONLY) : -1;
    if (g_w16_trace) {
        fprintf(g_w16_trace, "  open \"%s\" -> %s read %d write %d%s%s\n", dos, host, rfd, wfd,
                (writes && wfd < 0) || (reads && rfd < 0) ? ", " : "",
                (writes && wfd < 0) || (reads && rfd < 0) ? strerror(errno) : "");
        fflush(g_w16_trace);
    }
    if ((writes && wfd < 0) || (reads && rfd < 0)) {
        int missing = errno == ENOENT;
        if (wfd >= 0) close(wfd);
        if (rfd >= 0) close(rfd);
        return missing ? -E_FILE : -E_ACCESS;
    }
    int h = add_handle(rfd, wfd, host);
    if (h < 0) { if (wfd >= 0) close(wfd); if (rfd >= 0) close(rfd); return -E_ACCESS; }
    return h;
}

static long rw(struct w16 *w, unsigned h, uint32_t far, unsigned n, int writing) {
    struct dosfile *f = file_of(h);
    int fd = f ? (writing ? f->wfd : f->rfd) : -1;
    if (fd < 0) return -1;
    if (writing && n == 0)                       // DOS: a write of nothing ends the file here
        return sys_truncate(f->host, (unsigned long long)f->pos) != 0 ? -1 : 0;
    uint8_t *p = n ? cpu16_ptr(&w->cpu, (uint16_t)(far >> 16), (uint16_t)far, n, !writing) : 0;
    if (n && !p) return -2;                      // the fault is set
    if (lseek(fd, f->pos, SEEK_SET) < 0) return -1;
    long r = writing ? (long)write(fd, p, n) : (long)read(fd, p, n);
    if (r > 0) f->pos += r;
    return r < 0 ? -1 : r;
}

// --- KERNEL's calls ---------------------------------------------------------------

static void set32(struct cpu16 *c, uint32_t v) {
    c->r[R_AX] = (uint16_t)v;
    c->r[R_DX] = (uint16_t)(v >> 16);
}

static char *far_path(struct w16 *w, uint32_t far) {
    char *s = malloc(DOS_PATH * 2);
    if (!s) return 0;
    if (!(far >> 16) || cpu16_str(&w->cpu, (uint16_t)(far >> 16), (uint16_t)far, s, DOS_PATH * 2) < 0) {
        free(s);
        return 0;
    }
    return s;
}

// OpenFile(lpFileName, lpReOpenBuff, wStyle) -> HFILE
// Windows: Opens, creates, deletes or tests a file by its DOS path, and fills an OFSTRUCT with the
// full path and, on failure, DOS's error code; OF_REOPEN opens the path the OFSTRUCT already holds,
// OF_PARSE only works out the full path.
// w16: The same, over the drives the program sees: a DOS path becomes a toy-os one (C: is the Win16
// folder, Z: all of toy-os), names match without case as DOS's did, and the OFSTRUCT gets the full
// path in capitals, as DOS gave it. A drive outside /home is read-only. A bare name not in the
// current folder is looked for in C:\WINDOWS, C:\WINDOWS\SYSTEM and the program's own folder,
// Windows' order (PATH and network drives aside).
static int api_OpenFile(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint32_t name = w16_arg32(a), ofs = w16_arg32(a);
    uint16_t style = w16_arg16(a);
    int h = w16_open_file(w, name, ofs, style);
    if (h < 0) return -1;
    w->cpu.r[R_AX] = (uint16_t)h;
    return 10;
}

// One try of OpenFile's at one DOS path: a handle (1 for a test that passed), or -1 with *err DOS's code.
static int open_once(const char *dos, uint16_t style, char *full, size_t cap, int *err) {
    *err = 0;
    if (style & (OF_PARSE | OF_DELETE | OF_EXIST)) {
        char host[1024];
        *err = w16_dos_path(dos, host, sizeof host, (style & OF_PARSE) != 0, full, cap);
        if (!*err && (style & OF_DELETE) && unlink(host) != 0) *err = E_ACCESS;
        if (!*err && (style & OF_EXIST) && access(host, F_OK) != 0) *err = E_FILE;
        return *err ? -1 : 1;
    }
    int h = open_dos(dos, style & 3, (style & OF_CREATE) != 0, full, cap);
    if (h < 0) { *err = -h; return -1; }
    return h;
}

int w16_open_file(struct w16 *w, uint32_t name, uint32_t ofs, uint16_t style) {
    // OF_REOPEN reads the path the OFSTRUCT holds; nothing else of it is read
    char *dos = 0;
    if (style & OF_REOPEN) {
        char prev[DOS_PATH];
        if (cpu16_str(&w->cpu, (uint16_t)(ofs >> 16), (uint16_t)(ofs + 8), prev, sizeof prev) < 0) return -1;
        dos = strdup(prev);
    } else {
        dos = far_path(w, name);
    }
    if (!dos) return -1;
    char full[DOS_PATH * 2] = "";
    int err, h = open_once(dos, style, full, sizeof full, &err);
    // A bare name not in the current folder is looked for where Windows looked: its own folder,
    // SYSTEM, then the program's -- Control Panel opens "CONTROL.INF" so.
    int bare = !strpbrk(dos, "\\/:");
    char where[DOS_PATH * 2];
    for (int i = 0; h < 0 && err == E_FILE && bare && !(style & (OF_CREATE | OF_PARSE)) && i < 3; i++) {
        if (i == 2) w16_prog_dir(w, where, sizeof where);
        else snprintf(where, sizeof where, "%s", i ? "C:\\WINDOWS\\SYSTEM" : "C:\\WINDOWS");
        char path[DOS_PATH * 3];
        if (!where[0] || snprintf(path, sizeof path, "%s\\%s", where, dos) >= (int)sizeof path) continue;
        h = open_once(path, style, full, sizeof full, &err);
    }
    if (g_w16_trace) {
        fprintf(g_w16_trace, "  OpenFile \"%s\" style %04x -> %d (%s), DOS error %d\n", dos, style, h, full, err);
        fflush(g_w16_trace);
    }
    free(dos);
    // The header and the path's characters, as KERNEL wrote them -- never the
    // whole 136 bytes: Write's buffer is 132, its saved registers right after it.
    size_t n = 0;
    while (full[n] && n < DOS_PATH - 1) n++;
    uint8_t *put = cpu16_ptr(&w->cpu, (uint16_t)(ofs >> 16), (uint16_t)ofs, (uint32_t)(8 + n + 1), 1);
    if (!put) return -1;
    memset(put, 0, 8);
    put[0] = 136;                                // cBytes
    put[1] = 1;                                  // fFixedDisk
    put[2] = (uint8_t)err;
    for (size_t i = 0; i < n; i++) put[8 + i] = (uint8_t)upper(full[i]);
    put[8 + n] = 0;
    return h < 0 ? HFILE_ERROR : h;
}

// --- for LZEXPAND (lz16.c): a handle read, written, moved and closed in host memory -------------

long w16_file_read(unsigned h, void *buf, size_t n) {
    struct dosfile *f = file_of(h);
    if (!f || f->rfd < 0 || lseek(f->rfd, f->pos, SEEK_SET) < 0) return -1;
    long r = (long)read(f->rfd, buf, n);
    if (r > 0) f->pos += r;
    return r < 0 ? -1 : r;
}

long w16_file_write(unsigned h, const void *buf, size_t n) {
    struct dosfile *f = file_of(h);
    if (!f || f->wfd < 0 || lseek(f->wfd, f->pos, SEEK_SET) < 0) return -1;
    long r = (long)write(f->wfd, buf, n);
    if (r > 0) f->pos += r;
    return r < 0 ? -1 : r;
}

long w16_file_seek(unsigned h, long off, int origin) {
    struct dosfile *f = file_of(h);
    return f && origin >= 0 && origin <= 2 ? (long)seek(f, off, origin) : -1;
}

int w16_file_close(unsigned h) { return close_handle(h); }

// _lopen(lpPathName, iReadWrite) -> HFILE
// Windows: Opens an existing file for reading, writing or both, and returns DOS's file handle;
// HFILE_ERROR if it cannot.
// w16: The same, on a toy-os file found through the DOS path.
static int api_lopen(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    char *dos = far_path(w, w16_arg32(a));
    uint16_t mode = w16_arg16(a);
    int h = dos ? open_dos(dos, mode & 3, 0, 0, 0) : -E_FILE;
    free(dos);
    w->cpu.r[R_AX] = h < 0 ? HFILE_ERROR : (uint16_t)h;
    return 6;
}

// _lcreat(lpPathName, iAttribute) -> HFILE
// Windows: Makes a file, or empties one that exists, and opens it for reading and writing.
// w16: The same. The DOS attribute (read-only, hidden, system) is not kept: toy-os files have none
// of them.
static int api_lcreat(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    char *dos = far_path(w, w16_arg32(a));
    w16_arg16(a);
    int h = dos ? open_dos(dos, 2, 1, 0, 0) : -E_PATH;
    free(dos);
    w->cpu.r[R_AX] = h < 0 ? HFILE_ERROR : (uint16_t)h;
    return 6;
}

// _lclose(hFile) -> HFILE
// Windows: Closes a file handle.
// w16: The same; 0, or HFILE_ERROR for a handle that is not open.
static int api_lclose(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w->cpu.r[R_AX] = close_handle(w16_arg16(a)) < 0 ? HFILE_ERROR : 0;
    return 2;
}

// _lread(hFile, lpBuffer, wBytes) -> UINT
// _lwrite(hFile, lpBuffer, wBytes) -> UINT
// Windows: Read or write up to 65535 bytes at the file's position, which moves on; the count done,
// or HFILE_ERROR. A write of 0 bytes ends the file at the position, as DOS's did.
// w16: The same, the 0-byte write included, straight into or out of the program's memory, checked
// to lie inside the segment it names.
static int api_lread(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    uint16_t h = w16_arg16(a);
    uint32_t far = w16_arg32(a);
    uint16_t n = w16_arg16(a);
    long r = rw(w, h, far, n, ordinal == 86);
    if (r == -2) return -1;
    w->cpu.r[R_AX] = r < 0 ? HFILE_ERROR : (uint16_t)r;
    return 8;
}

// _llseek(hFile, lOffset, iOrigin) -> LONG
// Windows: Moves a file's position -- from its start, from where it is, or from its end -- and
// returns the new position.
// w16: The same; -1 for a handle that is not open or an origin that is not one of the three.
static int api_llseek(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t h = w16_arg16(a);
    int32_t off = (int32_t)w16_arg32(a);
    uint16_t origin = w16_arg16(a);
    struct dosfile *f = file_of(h);
    off_t r = f && origin <= 2 ? seek(f, off, origin) : -1;
    set32(&w->cpu, r < 0 ? 0xFFFFFFFFu : (uint32_t)r);
    return 8;
}

// GetTempFileName(cDriveLetter, lpPrefixString, uUnique, lpTempFileName) -> UINT
// Windows: A name for a temporary file, "~PRE1A2B.TMP" in the TEMP folder (or the drive's root):
// the prefix's first three letters and a number in hex. With uUnique 0 the number is the time and
// the file is made, a taken name stepping on to the next; else the name is only given. The number.
// w16: The same, in C:\TEMP -- the TEMP the program's environment names, made when it is not
// there -- whatever drive is asked: the program's temporary files stay in the Win16 folder.
static int api_GetTempFileName(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w16_arg16(a);                                // the drive: always C:'s TEMP
    uint32_t pre = w16_arg32(a);
    uint16_t unique = w16_arg16(a);
    uint32_t out = w16_arg32(a);
    char prefix[4] = "";
    char pbuf[16];
    if ((pre >> 16) && cpu16_str(&w->cpu, (uint16_t)(pre >> 16), (uint16_t)pre, pbuf, sizeof pbuf) >= 0)
        for (int i = 0; i < 3 && pbuf[i]; i++) { prefix[i] = upper(pbuf[i]); prefix[i + 1] = 0; }
    char host[1024], name[DOS_PATH];
    if (w16_dos_path("C:\\TEMP", host, sizeof host, 1, 0, 0) == 0) mkdir(host, 0755);
    uint16_t n = unique ? unique : (uint16_t)time(0);
    for (int tries = 0; tries < 0x10000; tries++, n++) {
        if (!n) continue;
        snprintf(name, sizeof name, "C:\\TEMP\\~%s%04X.TMP", prefix, n);
        if (unique) break;
        if (w16_dos_path(name, host, sizeof host, 1, 0, 0) != 0) { n = 0; break; }
        int fd = open(host, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd >= 0) { close(fd); break; }
        if (errno != EEXIST) { n = 0; break; }
    }
    w->cpu.r[R_AX] = n;
    if (!n) return 10;
    uint8_t *p = cpu16_ptr(&w->cpu, (uint16_t)(out >> 16), (uint16_t)out, (uint32_t)strlen(name) + 1, 1);
    if (!p) return -1;
    memcpy(p, name, strlen(name) + 1);
    return 10;
}

// GetTempDrive(cDriveLetter) -> UINT
// Windows: The drive temporary files go on: its letter in the low byte, a colon in the high one.
// w16: C:, where GetTempFileName puts them.
static int api_GetTempDrive(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w16_arg16(a);
    w->cpu.r[R_AX] = 'C' | ':' << 8;
    return 2;
}

// GetDriveType(nDrive) -> UINT
// Windows: What kind of drive a letter (0 is A:) is: 0 none, 2 removable, 3 fixed, 4 remote, 5 CD-ROM.
// w16: 3, a fixed disk, for each drive the program sees (C:, Z: and the folders mapped to D:-Y:); 0 for
// a letter that is no drive -- there are no floppies, CD-ROMs or network drives to tell apart.
static int api_GetDriveType(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t n = w16_arg16(a);
    w->cpu.r[R_AX] = n < 26 && drive_root((char)('A' + n)) ? 3 : 0;
    return 2;
}

const struct w16_api g_file_api[] = {
    { 74, api_OpenFile },
    { 81, api_lclose },
    { 82, api_lread },
    { 83, api_lcreat },
    { 84, api_llseek },
    { 85, api_lopen },
    { 86, api_lread },
    { 92, api_GetTempDrive },
    { 97, api_GetTempFileName },
    { 136, api_GetDriveType },
};
const unsigned g_file_api_n = sizeof g_file_api / sizeof g_file_api[0];

// --- INT 21h: DOS's file calls ---------------------------------------------------------

static void dos_ok(struct cpu16 *c) { c->flags &= (uint16_t)~F_CF; }
static int dos_err(struct cpu16 *c, int e) { c->flags |= F_CF; c->r[R_AX] = (uint16_t)e; return 0; }

static char *ds_dx_path(struct cpu16 *c) {
    char *s = malloc(DOS_PATH * 2);
    if (s && cpu16_str(c, c->sreg[S_DS], c->r[R_DX], s, DOS_PATH * 2) < 0) { free(s); return 0; }
    return s;
}

// The DTA a find writes into, and the folder and pattern it walks.
static uint32_t g_dta;
static struct { char host[1024], pattern[16]; int next; uint16_t attr; } g_find;

static int match83(const char *pat, const char *name) {
    for (; *pat; pat++, name++) {
        if (*pat == '*') {
            for (const char *n = name; ; n++) {
                if (match83(pat + 1, n)) return 1;
                if (!*n) return 0;
            }
        }
        if (!*name || (*pat != '?' && upper(*pat) != upper(*name))) return 0;
    }
    return !*name;
}

static int find_next(struct cpu16 *c) {
    DIR *d = opendir(g_find.host);
    if (!d) return dos_err(c, 18);
    struct dirent *de;
    int i = 0;
    while ((de = readdir(d))) {
        if (i++ < g_find.next) continue;
        g_find.next = i;
        // "*.*" matches every name, with a dot or without, as DOS's did.
        int all = !strcmp(g_find.pattern, "*.*") || !strcmp(g_find.pattern, "*");
        if (!all && !match83(g_find.pattern, de->d_name)) continue;
        char path[1100];
        struct stat st;
        snprintf(path, sizeof path, "%s/%s", strcmp(g_find.host, "/") ? g_find.host : "", de->d_name);
        if (stat(path, &st) != 0) continue;
        int dir = S_ISDIR(st.st_mode);
        if (dir && !(g_find.attr & 0x10)) continue;
        uint8_t *p = cpu16_ptr(c, (uint16_t)(g_dta >> 16), (uint16_t)g_dta, 43, 1);
        if (!p) { closedir(d); return -1; }
        memset(p, 0, 43);
        p[21] = dir ? 0x10 : 0x20;
        struct tm *tm = localtime(&st.st_mtime);
        uint16_t t = tm ? (uint16_t)(tm->tm_hour << 11 | tm->tm_min << 5 | tm->tm_sec / 2) : 0;
        uint16_t dt = tm ? (uint16_t)((tm->tm_year - 80) << 9 | (tm->tm_mon + 1) << 5 | tm->tm_mday) : 0;
        memcpy(p + 22, &t, 2);
        memcpy(p + 24, &dt, 2);
        uint32_t size = st.st_size > 0xFFFFFFFF ? 0xFFFFFFFFu : (uint32_t)st.st_size;
        memcpy(p + 26, &size, 4);
        snprintf((char *)p + 30, 13, "%s", de->d_name);
        closedir(d);
        dos_ok(c);
        return 0;
    }
    closedir(d);
    return dos_err(c, 18);                       // no more files
}

int w16_dos_file(struct w16 *w, struct cpu16 *c) {
    uint8_t ah = (uint8_t)(c->r[R_AX] >> 8);
    switch (ah) { // dispatch-ok: bounded by DOS's INT 21h functions, the ABI
    case 0x0E:                                   // select drive: C: or Z:
        if (drive_root((char)('A' + (c->r[R_DX] & 0xFF))) && (c->r[R_DX] & 0xFF) < 26) {
            char d = (char)('A' + (c->r[R_DX] & 0xFF));
            if (upper(g_cwd[0]) != d) snprintf(g_cwd, sizeof g_cwd, "%c:\\", d);
        }
        c->r[R_AX] = (uint16_t)((c->r[R_AX] & 0xFF00) | 26);
        return 0;
    case 0x19:                                   // the current drive
        c->r[R_AX] = (uint16_t)((c->r[R_AX] & 0xFF00) | (upper(g_cwd[0]) - 'A'));
        return 0;
    case 0x1A:                                   // set the DTA
        g_dta = (uint32_t)c->sreg[S_DS] << 16 | c->r[R_DX];
        return 0;
    case 0x2F:                                   // get the DTA
        c->sreg[S_ES] = (uint16_t)(g_dta >> 16);
        c->r[R_BX] = (uint16_t)g_dta;
        return 0;
    case 0x2A: case 0x2C: {                      // date, time
        time_t now = time(0);
        struct tm *tm = localtime(&now);
        if (!tm) return -1;
        if (ah == 0x2A) {
            c->r[R_CX] = (uint16_t)(tm->tm_year + 1900);
            c->r[R_DX] = (uint16_t)((tm->tm_mon + 1) << 8 | tm->tm_mday);
            c->r[R_AX] = (uint16_t)((c->r[R_AX] & 0xFF00) | tm->tm_wday);
        } else {
            c->r[R_CX] = (uint16_t)(tm->tm_hour << 8 | tm->tm_min);
            c->r[R_DX] = (uint16_t)(tm->tm_sec << 8);
        }
        return 0;
    }
    case 0x36:                                   // free space: plenty (32 KiB clusters)
        c->r[R_AX] = 64;
        c->r[R_BX] = 0x7FFF;
        c->r[R_CX] = 512;
        c->r[R_DX] = 0xFFFF;
        return 0;
    case 0x3B: case 0x39: case 0x3A: case 0x41: case 0x43: case 0x4E: {
        static char host[1024], full[DOS_PATH * 2];   // the task thread's alone
        char *dos = ds_dx_path(c);
        if (!dos) return dos_err(c, E_PATH);
        int e;
        if (ah == 0x4E) {                        // find first: the folder, then the pattern
            static char folder[DOS_PATH * 2];
            char *sl = strrchr(dos, '\\');
            int drive = dos[0] && dos[1] == ':';
            snprintf(g_find.pattern, sizeof g_find.pattern, "%s", sl ? sl + 1 : drive ? dos + 2 : dos);
            if (sl) {
                *sl = 0;
                if (!dos[0] || (drive && !dos[2])) snprintf(folder, sizeof folder, "%s\\", dos);
                else snprintf(folder, sizeof folder, "%s", dos);
            } else {
                snprintf(folder, sizeof folder, drive ? "%c:" : ".", dos[0]);
            }
            e = w16_dos_path(folder, host, sizeof host, 0, 0, 0);
            free(dos);
            if (e) return dos_err(c, e);
            snprintf(g_find.host, sizeof g_find.host, "%s", host);
            g_find.next = 0;
            g_find.attr = c->r[R_CX];
            return find_next(c);
        }
        e = w16_dos_path(dos, host, sizeof host, ah == 0x39, full, sizeof full);
        free(dos);
        if (e) return dos_err(c, ah == 0x3B ? E_PATH : e);
        struct stat st;
        switch (ah) {
        case 0x3B:                               // chdir
            if (stat(host, &st) != 0 || !S_ISDIR(st.st_mode)) return dos_err(c, E_PATH);
            snprintf(g_cwd, sizeof g_cwd, "%s", full);
            break;
        case 0x39: if (mkdir(host, 0755) != 0) return dos_err(c, E_ACCESS); break;
        case 0x3A: if (unlink(host) != 0) return dos_err(c, E_ACCESS); break;
        case 0x41: if (unlink(host) != 0) return dos_err(c, E_ACCESS); break;
        case 0x43:                               // attributes: a folder, or an archive
            if (stat(host, &st) != 0) return dos_err(c, E_FILE);
            if ((c->r[R_AX] & 0xFF) == 0) c->r[R_CX] = S_ISDIR(st.st_mode) ? 0x10 : 0x20;
            break;
        }
        dos_ok(c);
        return 0;
    }
    case 0x3C: case 0x3D: {                      // create, open
        char *dos = ds_dx_path(c);
        if (!dos) return dos_err(c, E_PATH);
        int h = open_dos(dos, c->r[R_AX] & 3, ah == 0x3C, 0, 0);
        free(dos);
        if (h < 0) return dos_err(c, -h);
        c->r[R_AX] = (uint16_t)h;
        dos_ok(c);
        return 0;
    }
    case 0x3E:                                   // close
        if (close_handle(c->r[R_BX]) < 0) return dos_err(c, E_HANDLE);
        dos_ok(c);
        return 0;
    case 0x3F: case 0x40: {                      // read, write
        long r = rw(w, c->r[R_BX], (uint32_t)c->sreg[S_DS] << 16 | c->r[R_DX], c->r[R_CX], ah == 0x40);
        if (r == -2) return -1;
        if (r < 0) return dos_err(c, E_HANDLE);
        c->r[R_AX] = (uint16_t)r;
        dos_ok(c);
        return 0;
    }
    case 0x42: {                                 // seek
        struct dosfile *f = file_of(c->r[R_BX]);
        int whence = c->r[R_AX] & 0xFF;
        if (!f || whence > 2) return dos_err(c, E_HANDLE);
        off_t off = (int32_t)((uint32_t)c->r[R_CX] << 16 | c->r[R_DX]);
        off_t r = seek(f, off, whence);
        if (r < 0) return dos_err(c, 25);
        c->r[R_AX] = (uint16_t)r;
        c->r[R_DX] = (uint16_t)(r >> 16);
        dos_ok(c);
        return 0;
    }
    case 0x44:                                   // IOCTL: every handle is a file on a fixed disk
        if ((c->r[R_AX] & 0xFF) == 0) { c->r[R_DX] = 0; dos_ok(c); return 0; }
        if ((c->r[R_AX] & 0xFF) == 8) { c->r[R_AX] = 1; dos_ok(c); return 0; }
        return dos_err(c, 1);
    case 0x47: {                                 // the current folder, no drive or first '\'
        uint8_t *p = cpu16_ptr(c, c->sreg[S_DS], c->r[R_SI], 64, 1);
        if (!p) return -1;
        snprintf((char *)p, 64, "%s", g_cwd + 3);
        c->r[R_AX] = 0x0100;                     // DOS's own (undocumented) answer: Write scans for AL's 0
        dos_ok(c);
        return 0;
    }
    case 0x4F:
        return find_next(c);
    case 0x56: {                                 // rename DS:DX to ES:DI
        static char to[DOS_PATH * 2], hf[1024], ht[1024];
        char *from = ds_dx_path(c);
        if (!from) return dos_err(c, E_PATH);
        int e = cpu16_str(c, c->sreg[S_ES], c->r[R_DI], to, sizeof to) < 0 ? E_PATH : 0;
        if (!e) e = w16_dos_path(from, hf, sizeof hf, 0, 0, 0);
        if (!e) e = w16_dos_path(to, ht, sizeof ht, 1, 0, 0);
        free(from);
        if (e) return dos_err(c, e);
        if (rename(hf, ht) != 0) return dos_err(c, E_ACCESS);
        dos_ok(c);
        return 0;
    }
    case 0x57: {                                 // a file's date and time
        struct dosfile *f = file_of(c->r[R_BX]);
        struct stat st;
        if (!f || fstat(f->rfd >= 0 ? f->rfd : f->wfd, &st) != 0) return dos_err(c, E_HANDLE);
        if ((c->r[R_AX] & 0xFF) == 0) {
            struct tm *tm = localtime(&st.st_mtime);
            c->r[R_CX] = tm ? (uint16_t)(tm->tm_hour << 11 | tm->tm_min << 5 | tm->tm_sec / 2) : 0;
            c->r[R_DX] = tm ? (uint16_t)((tm->tm_year - 80) << 9 | (tm->tm_mon + 1) << 5 | tm->tm_mday) : 0;
        }
        dos_ok(c);
        return 0;
    }
    case 0x59:                                   // extended error: none to tell
        c->r[R_AX] = 0;
        return 0;
    default:
        return -1;
    }
}
