// File operations, once -- see base/ufileop.h for what is shared and why.
#include "base/ufileop.h"
#include "kpath.h"   // k_path_join/_basename -- the kernel's, KTESTed
#include <string.h>
#include <stdio.h>
#include <stdlib.h>   // malloc -- the copy buffer, sized to the file
#include "syscall_abi.h" // SYS_WRITE_MAX
#include "base/ugrow.h"
#include "base/upath.h"

// --- reporting --------------------------------------------------------
//
// Every failure goes through here so the walk keeps going: one
// unreadable file in a tree of a thousand must not abandon the other
// 999, which is coreutils' behaviour and the only useful one.
static void report(struct ufileop *s, const struct ufileop_policy *p,
                    const char *path, int err) {
    s->failed = 1;
    if (p && p->on_error) p->on_error(p->ctx, path, err);
}

static int progress(struct ufileop *s, const struct ufileop_policy *p,
                     const char *path, uint64_t done, uint64_t total) {
    if (!p || !p->on_progress) return 1;
    if (p->on_progress(p->ctx, path, done, total)) return 1;
    s->cancelled = 1;
    return 0;
}

// --- helpers ----------------------------------------------------------

char *ufileop_resolve_dest(const char *src, const char *dst) {
    struct sys_stat st;
    if (sys_stat(dst, &st) == 0 && st.is_dir) return upath_join(dst, k_path_basename(src));
    return strdup(dst);
}

int ufileop_inside(const char *src, const char *dst) {
    size_t n = strlen(src);
    if (strncmp(dst, src, n) != 0) return 0;
    return dst[n] == '/' || (dst[n] == '\0' && n > 0);
}

int ufileop_name_ok(const char *name) {
    return name && name[0] && !strchr(name, '/') && strcmp(name, ".") != 0 && strcmp(name, "..") != 0;
}

int ufileop_unique_name(const char *dir, const char *name, char *out, int cap) {
    // The LAST dot, and not a leading one: ".config" is a name, not an
    // extension, so its suffix goes at the end like a directory's.
    const char *dot = 0;
    for (const char *q = name; *q; q++)
        if (*q == '.' && q != name) dot = q;

    int stem = dot ? (int)(dot - name) : (int)strlen(name);
    const char *ext = dot ? dot : "";

    for (int n = 1; n < 1000; n++) {
        char *cand = 0;
        if (asprintf(&cand, "%.*s (%d)%s", stem, name, n, ext) < 0) return 0;
        char *full = upath_join(dir, cand);
        struct sys_stat st;
        // REFUSED, not truncated, when `out` is short: a shortened name
        // is a different file, and silently writing one is how a paste
        // overwrites something else.
        int unused = full && sys_stat(full, &st) != 0;
        int ok = unused && strlcpy(out, cand, (size_t)cap) < (size_t)cap;
        free(full);
        free(cand);
        if (!full || unused) return ok;
    }
    return 0;
}

// The decision for a destination that exists, resolved into the path to
// actually write. Returns the enum; on RENAME `*dst` (heap) is replaced.
static int resolve_conflict(struct ufileop *s, const struct ufileop_policy *p,
                             const char *src, char **dst) {
    struct sys_stat st;
    if (sys_stat(*dst, &st) != 0) return UFILEOP_OVERWRITE;  // no conflict
    if (!p || !p->on_conflict) return UFILEOP_OVERWRITE;     // cp's answer

    char newname[ULIST_NAME_MAX];
    newname[0] = '\0';
    int d = p->on_conflict(p->ctx, src, *dst, newname, sizeof newname);
    if (d == UFILEOP_CANCEL) { s->cancelled = 1; return d; }
    if (d != UFILEOP_RENAME) return d;

    if (!ufileop_name_ok(newname)) {
        report(s, p, *dst, EINVAL);
        return UFILEOP_SKIP;
    }
    char *dir = upath_dirname(*dst), *to = dir ? upath_join(dir, newname) : 0;
    free(dir);
    if (!to) { report(s, p, *dst, ENOMEM); return UFILEOP_SKIP; }
    free(*dst);
    *dst = to;
    return UFILEOP_RENAME;
}

// --- one file ---------------------------------------------------------

// Renames `dst` to a hidden name beside it and answers that path (heap).
// NULL when there is no such name to be had or the rename fails -- the
// caller deletes whatever a non-NULL answer names.
static char *set_aside(const char *dst) {
    char *dir = upath_dirname(dst), *name = 0, *out = 0;
    char uniq[ULIST_NAME_MAX];
    struct sys_stat st;
    if (dir && asprintf(&name, ".%s.replaced", k_path_basename(dst)) >= 0)
        out = upath_join(dir, name);
    if (out && sys_stat(out, &st) == 0) {
        free(out);
        out = ufileop_unique_name(dir, name, uniq, sizeof uniq) ? upath_join(dir, uniq) : 0;
    }
    free(dir);
    free(name);
    if (out && sys_rename(dst, out) != 0) { free(out); out = 0; }
    return out;
}


// `*copied` is 1 only when every byte arrived -- what a move needs to
// know before it deletes the source.
static int copy_file(struct ufileop *s, const struct ufileop_policy *p,
                     const char *src, const char *dst, int *copied);

static int copy_one(struct ufileop *s, const struct ufileop_policy *p,
                     const char *src, const char *dst_in, int *copied) {
    if (copied) *copied = 0;
    char *dst = strdup(dst_in);
    if (!dst) { report(s, p, dst_in, ENOMEM); return 1; }
    int d = resolve_conflict(s, p, src, &dst);
    int rc = d == UFILEOP_CANCEL ? 0
           : d == UFILEOP_SKIP   ? 1   // not an error; the caller chose
           : copy_file(s, p, src, dst, copied);
    free(dst);
    return rc;
}

// copy_one()'s body, once the destination is settled.
static int copy_file(struct ufileop *s, const struct ufileop_policy *p,
                     const char *src, const char *dst, int *copied) {
    int in = sys_open(src, 0);
    if (in < 0) { report(s, p, src, sys_errno()); return 1; }

    // A REPLACE SETS THE OLD FILE ASIDE rather than truncating it: a copy
    // that is cancelled or fails halfway puts it back, so the person who
    // said Replace never ends up with neither file.
    char *aside = 0;
    struct sys_stat dst_st;
    if (sys_stat(dst, &dst_st) == 0 && !dst_st.is_dir) aside = set_aside(dst);

    int out = sys_open(dst, SYS_O_WRITE | SYS_O_CREAT | SYS_O_TRUNC);
    if (out < 0) {
        report(s, p, dst, sys_errno());
        sys_close(in);
        if (aside) sys_rename(aside, dst);
        free(aside);
        return 1;
    }

    struct sys_stat st;
    uint64_t total = (sys_stat(src, &st) == 0) ? st.size : 0;
    uint64_t done = 0;
    int keep_going = 1, whole = 1;

    // A MiB a call where the file has one (SYS_WRITE_MAX): on a stick
    // every write is a commit and a flush. Sized to the file, and the
    // state's own 64 KiB when the heap says no.
    size_t cap = total < SYS_WRITE_MAX ? (size_t)total : SYS_WRITE_MAX;
    char *big = cap > sizeof s->buf ? malloc(cap) : NULL;
    char *buf = big ? big : s->buf;
    if (!big) cap = sizeof s->buf;

    for (;;) {
        int64_t n = sys_read(in, buf, cap);
        if (n < 0) { report(s, p, src, sys_errno()); whole = 0; break; }
        if (n == 0) break; // EOF
        // A SHORT WRITE IS A FAILURE, not something to retry blindly:
        // this filesystem writes a whole request or reports why, and a
        // loop that kept going would turn "the disk is full" into a
        // silently truncated file.
        int64_t w = sys_write(out, buf, (size_t)n);
        if (w != n) { report(s, p, dst, sys_errno()); whole = 0; break; }
        done += (uint64_t)n;
        if (!progress(s, p, src, done, total)) { keep_going = 0; break; }
    }

    free(big);
    sys_close(in);
    sys_close(out);
    // A CANCELLED OR FAILED COPY LEAVES A PARTIAL FILE, and it is removed
    // rather than left: half a file with the right name is worse than no
    // file, because nothing downstream can tell the difference.
    if (!keep_going || !whole) {
        sys_unlink(dst);
        if (aside) sys_rename(aside, dst);
        free(aside);
        return keep_going;
    }
    if (aside) sys_unlink(aside);
    free(aside);
    if (copied) *copied = 1;
    return keep_going;
}

// --- trees ------------------------------------------------------------

// Takes `src` and `dst` (heap; `dst` may be NULL) whatever it answers.
static int queue_push(struct ufileop *s, const struct ufileop_policy *p,
                       char *src, char *dst) {
    if (!src || !UGROW(s->qsrc, s->qcap_src, s->qtail + 1) ||
        !UGROW(s->qdst, s->qcap_dst, s->qtail + 1)) {
        report(s, p, src ? src : "", ENOMEM);
        free(src);
        free(dst);
        return 0;
    }
    s->qsrc[s->qtail] = src;
    s->qdst[s->qtail] = dst;
    s->qtail++;
    return 1;
}

static void queue_free(struct ufileop *s) {
    for (int i = 0; i < s->qtail; i++) { free(s->qsrc[i]); free(s->qdst[i]); }
    free(s->qsrc);
    free(s->qdst);
    s->qsrc = 0;
    s->qdst = 0;
    s->qhead = s->qtail = s->qcap_src = s->qcap_dst = 0;
}

// `move`: each file's source is deleted as soon as its copy has landed,
// so a file the person SKIPPED -- or one that failed -- stays where it
// was, and the folders left empty are pruned afterwards (remove_tree's
// `files` 0). Deleting the whole source after the walk would delete the
// skipped files with it.
static int copy_tree(struct ufileop *s, const struct ufileop_policy *p,
                      const char *src, const char *dst, int move) {
    s->qhead = s->qtail = 0;
    if (sys_mkdir(dst) < 0 && sys_errno() != EEXIST) {
        report(s, p, dst, sys_errno());
        return 1;
    }
    char *dst_copy = strdup(dst);
    if (!dst_copy || !queue_push(s, p, strdup(src), dst_copy)) {
        if (!dst_copy) report(s, p, dst, ENOMEM);
        queue_free(s);
        return 1;
    }

    while (s->qhead < s->qtail && !s->cancelled) {
        // The strings stay put while the queue grows; only the arrays of
        // pointers move.
        const char *sdir = s->qsrc[s->qhead], *ddir = s->qdst[s->qhead];
        s->qhead++;

        // ONE SNAPSHOT of the directory, then the work: a move deletes
        // sources as it goes, and a listing read after a delete would
        // be shifted under any page offset into it.
        struct ulist_entry *ents;
        int n = ulistdir(sdir, &ents);
        if (n < 0) { report(s, p, sdir, sys_errno()); continue; }
        for (int i = 0; i < n && !s->cancelled; i++) {
            char *sp = upath_join(sdir, ents[i].name), *dp = upath_join(ddir, ents[i].name);
            if (!sp || !dp) {
                report(s, p, ents[i].name, ENOMEM);
            } else if (p && p->leave_out && p->leave_out(p->ctx, sp, ents[i].is_dir)) {
                // left out
            } else if (ents[i].is_dir) {
                if (sys_mkdir(dp) < 0 && sys_errno() != EEXIST) {
                    report(s, p, dp, sys_errno());
                } else if (!p || !p->enter_dir || p->enter_dir(p->ctx, sp)) {
                    queue_push(s, p, sp, dp);   // the queue owns both now
                    sp = dp = 0;
                }
            } else {
                int copied;
                copy_one(s, p, sp, dp, &copied);
                if (move && copied && sys_unlink(sp) != 0) report(s, p, sp, sys_errno());
            }
            free(sp);
            free(dp);
        }
        free(ents);
    }
    queue_free(s);
    return 1;
}

// `files` 0 PRUNES: only the folders go, and only those left empty --
// one still holding something fails quietly and stays (a move's skips).
static int remove_tree(struct ufileop *s, const struct ufileop_policy *p,
                        const char *root, int files) {
    // The queue's SOURCE half doubles as the directory list here; the
    // destination half is unused, which is cheaper than a second array
    // for a walk that is otherwise identical.
    s->qhead = s->qtail = 0;
    if (!queue_push(s, p, strdup(root), 0)) { queue_free(s); return 1; }

    for (int read = 0; read < s->qtail && !s->cancelled; read++) {
        const char *dir = s->qsrc[read];   // stays put while the queue grows

        // ONE SNAPSHOT, then the deletes: every file a pass deletes
        // shifts the rest of a live listing down, so no offset into one
        // survives a delete (copy_tree() says the same).
        struct ulist_entry *ents;
        int n = ulistdir(dir, &ents);
        if (n < 0) { report(s, p, dir, sys_errno()); continue; }
        for (int i = 0; i < n && !s->cancelled; i++) {
            char *path = upath_join(dir, ents[i].name);
            if (!path) {
                report(s, p, ents[i].name, ENOMEM);
                continue;
            }
            if (ents[i].is_dir) {
                queue_push(s, p, path, 0);   // the queue owns it now
                continue;
            }
            int go_on = 1;
            if (files) {
                if (sys_unlink(path) != 0) report(s, p, path, sys_errno());
                go_on = progress(s, p, path, 1, 1);
            }
            free(path);
            if (!go_on) { free(ents); queue_free(s); return 1; }
        }
        free(ents);
    }

    // DEEPEST FIRST: a directory with anything still in it cannot be
    // unlinked, and the queue is in discovery order, so walking it
    // backwards is walking the tree from the leaves.
    for (int i = s->qtail - 1; i >= 0 && !s->cancelled; i--)
        if (sys_unlink(s->qsrc[i]) != 0 && files) report(s, p, s->qsrc[i], sys_errno());
    queue_free(s);
    return 1;
}

// --- the public operations --------------------------------------------

static int finish(const struct ufileop *s) {
    if (s->cancelled) return UFILEOP_CANCELLED;
    return s->failed ? UFILEOP_FAILED : UFILEOP_OK;
}

static void begin(struct ufileop *s) { s->cancelled = s->failed = 0; }

int ufileop_copy(const char *src, const char *dst_arg, struct ufileop *s,
                  const struct ufileop_policy *p) {
    begin(s);

    struct sys_stat st;
    if (sys_stat(src, &st) != 0) { report(s, p, src, sys_errno()); return finish(s); }

    char *dst = ufileop_resolve_dest(src, dst_arg);
    if (!dst) {
        report(s, p, dst_arg, ENOMEM);
    } else if (strcmp(src, dst) == 0) {
        report(s, p, src, EINVAL);
    } else if (st.is_dir && ufileop_inside(src, dst)) {
        report(s, p, dst, EINVAL);
    } else if (st.is_dir) {
        // A DIRECTORY'S CONFLICT IS ITS OWN, resolved before the walk:
        // asking per file inside it would ask once per entry for what is
        // one decision about one folder.
        int d = resolve_conflict(s, p, src, &dst);
        if (d != UFILEOP_CANCEL && d != UFILEOP_SKIP) copy_tree(s, p, src, dst, 0);
    } else {
        copy_one(s, p, src, dst, 0);
    }
    free(dst);
    return finish(s);
}

static void move_to(const char *src, char **dstp, int is_dir,
                    struct ufileop *s, const struct ufileop_policy *p);

int ufileop_move(const char *src, const char *dst_arg, struct ufileop *s,
                  const struct ufileop_policy *p) {
    begin(s);

    struct sys_stat st;
    if (sys_stat(src, &st) != 0) { report(s, p, src, sys_errno()); return finish(s); }

    char *dst = ufileop_resolve_dest(src, dst_arg);
    if (!dst) report(s, p, dst_arg, ENOMEM);
    else if (strcmp(src, dst) == 0) {}   // already there
    else if (st.is_dir && ufileop_inside(src, dst)) report(s, p, dst, EINVAL);
    else move_to(src, &dst, st.is_dir, s, p);
    free(dst);
    return finish(s);
}

// ufileop_move()'s body, once `*dstp` (heap; a rename may replace it)
// is known to be somewhere else.
static void move_to(const char *src, char **dstp, int is_dir,
                    struct ufileop *s, const struct ufileop_policy *p) {
    int d = resolve_conflict(s, p, src, dstp);
    if (d == UFILEOP_CANCEL || d == UFILEOP_SKIP) return;
    const char *dst = *dstp;
    // AN OVERWRITE SETS THE OLD FILE ASIDE, never deletes it first:
    // SYS_RENAME will not replace a name, and a move whose copy fallback
    // then fails (disk full, a read error) must not have lost the file it
    // was told to replace. Dropped once the move has landed, put back if not.
    char *aside = 0;
    if (d == UFILEOP_OVERWRITE) {
        struct sys_stat dstat;
        if (sys_stat(dst, &dstat) == 0 && !dstat.is_dir && !(aside = set_aside(dst)))
            sys_unlink(dst);   // nowhere to set it aside: clear the way as before
    }

    if (sys_rename(src, dst) == 0) {
        if (aside) sys_unlink(aside);
        free(aside);
        return;
    }

    // THE FALLBACK IS THE POINT. A rename across parents needs five
    // journal credits and a v1 TFS3 volume has four slots (fs.h), so
    // that one case fails with EIO -- and a person dragging a folder
    // should not be told about journal credits. Copy, then delete.
    int rc = sys_errno();
    if (is_dir) {
        // File by file: what landed leaves the source, what was skipped
        // or failed stays there, and the emptied folders go.
        copy_tree(s, p, src, dst, 1);
        if (!s->cancelled) remove_tree(s, p, src, 0);
        free(aside);
        return;
    }
    int copied;
    copy_one(s, p, src, dst, &copied);
    if (!copied && !s->failed && !s->cancelled) {
        // skipped: nothing moved
    } else if (s->failed || s->cancelled) {
        // The source stays: a move whose copy half failed must not
        // delete the only remaining copy.
        if (aside) {
            sys_unlink(dst);
            sys_rename(aside, dst);
        }
        if (!s->failed) report(s, p, src, rc);
    } else {
        if (aside) sys_unlink(aside);
        if (sys_unlink(src) != 0) report(s, p, src, sys_errno());
    }
    free(aside);
}

int ufileop_remove(const char *path, int recursive, struct ufileop *s,
                    const struct ufileop_policy *p) {
    begin(s);

    struct sys_stat st;
    // A plain file passed with `recursive` is still just a file: a
    // mixed list must not refuse the files in it.
    if (recursive && sys_stat(path, &st) == 0 && st.is_dir) {
        remove_tree(s, p, path, 1);
        return finish(s);
    }
    if (sys_unlink(path) != 0) report(s, p, path, sys_errno());
    return finish(s);
}
