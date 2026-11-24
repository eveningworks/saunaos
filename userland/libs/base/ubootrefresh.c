// The boot loader rewritten in place, and the record that says whether it
// is worth doing (base/ubootrefresh.h). Moved out of /bin/install so
// System Update runs the same steps.
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "rt/sys.h"
#include "query_abi.h"
#include "base/ubootcfg.h"
#include "base/ubootlayout.h"
#include "base/ubootmenu.h"
#include "base/ubootmode.h"
#include "base/ubootrefresh.h"
#include "base/ufile.h"
#include "base/uinstall.h"
#include "update_abi.h"    // UPDATE_STAGED_SUFFIX: a payload waiting for the boot

static char *read_text(const char *path) {
    uint8_t *buf = 0;
    size_t len = 0;
    if (ufile_slurp(path, 0, &buf, &len) != UFILE_OK) return 0;
    char *s = malloc(len + 1);
    if (s) { memcpy(s, buf, len); s[len] = 0; }
    free(buf);
    return s;
}

// The flat layout first: a refresh always writes there, even when the
// legacy tree's removal after it failed and /boot/boot is still about.
char *ubootrefresh_record(void) {
    char *r = read_text(UBOOTREFRESH_RECORD);
    if (!r) r = read_text(UBOOTLAYOUT_LEGACY "/grub/core.modules");
    return r ? r : read_text(UBOOTREFRESH_RECORD_OLD);
}

// AN UPDATE THAT TOUCHED THE KERNEL STAGES EVERY FILE FOR THE NEXT BOOT,
// /install's included (docs/update-design.md), so until that restart the
// live /install is the OLD payload: the staged copy is what to compare
// against, and nothing may be written from either.
static int payload_pending(void) {
    struct sys_stat st;
    return sys_stat(UBOOTREFRESH_STAGED UPDATE_STAGED_SUFFIX, &st) == 0 ||
           sys_stat(UINSTALL_PAYLOAD "/core.img" UPDATE_STAGED_SUFFIX, &st) == 0 ||
           sys_stat(UINSTALL_PAYLOAD "/core-msdos.img" UPDATE_STAGED_SUFFIX, &st) == 0;
}

// The module line is the first; `grub-version=` lines are not modules.
static size_t module_line(const char *rec) {
    return strcspn(rec, "\n");
}

static int has_n(const char *record, const char *module, size_t n) {
    if (!record) return 0;
    size_t end = module_line(record);
    for (size_t i = 0; i < end;) {
        while (i < end && (record[i] == ' ' || record[i] == '\t')) i++;
        size_t w = i;
        while (i < end && record[i] != ' ' && record[i] != '\t' && record[i] != '\r') i++;
        if (i - w == n && !memcmp(record + w, module, n)) return 1;
        while (i < end && record[i] == '\r') i++;
    }
    return 0;
}

int ubootrefresh_has(const char *record, const char *module) {
    return has_n(record, module, strlen(module));
}

// The record's GRUB version, allocated to fit; NULL when it names none
// (or out of memory, which reads the same: no version to compare).
static char *version_of(const char *rec) {
    const char *v = rec ? strstr(rec, "grub-version=") : 0;
    if (!v) return 0;
    v += strlen("grub-version=");
    size_t n = strcspn(v, " \t\r\n");
    char *out = malloc(n + 1);
    if (out) { memcpy(out, v, n); out[n] = 0; }
    return out;
}

// strverscmp()'s idea of order: digit runs compare as numbers, the rest
// byte by byte. "2.12-1" < "2.16-1.1" < "2.16-2".
static int vercmp(const char *a, const char *b) {
    while (*a && *b) {
        if (*a >= '0' && *a <= '9' && *b >= '0' && *b <= '9') {
            while (*a == '0') a++;
            while (*b == '0') b++;
            size_t na = 0, nb = 0;
            while (a[na] >= '0' && a[na] <= '9') na++;
            while (b[nb] >= '0' && b[nb] <= '9') nb++;
            if (na != nb) return na < nb ? -1 : 1;
            int c = memcmp(a, b, na);
            if (c) return c < 0 ? -1 : 1;
            a += na;
            b += nb;
        } else {
            if (*a != *b) return (unsigned char)*a < (unsigned char)*b ? -1 : 1;
            a++;
            b++;
        }
    }
    return *a ? 1 : *b ? -1 : 0;
}

static const char *plan_disk(struct ubootrefresh_plan *p) {
    memset(p, 0, sizeof *p);
    if (!uinstall_root_disk(p->disk, sizeof p->disk))
        return "this machine has no disk-backed root -- there is no boot loader here to rewrite";
    struct query_parttable t;
    if (sys_query_record(QUERY_PARTTABLE, 0, &t, sizeof t) < (int)sizeof t ||
        t.kind == QUERY_PART_NONE)
        return "this disk has no partition table -- refusing to guess where the core image goes";
    p->mbr = t.kind == QUERY_PART_MBR;
    const char *missing = uinstall_payload_problem(p->mbr);
    if (missing) return missing;
    p->legacy = ubootlayout_legacy();
    p->core = p->mbr ? UINSTALL_PAYLOAD "/core-msdos.img" : UINSTALL_PAYLOAD "/core.img";
    struct sys_stat st;
    int staged = sys_stat(UINSTALL_PAYLOAD "/bootx64.efi", &st) == 0 && !st.is_dir;
    int on_disk = sys_stat("/boot/EFI/BOOT/BOOTX64.EFI", &st) == 0 && !st.is_dir;
    struct query_firmware fw;
    int uefi = sys_query_record(QUERY_FIRMWARE, 0, &fw, sizeof fw) == (int)sizeof fw && fw.uefi;
    p->has_efi = staged && (on_disk || uefi);
    p->efi_kept_off = staged && !p->has_efi;
    struct ubootcfg *cfg = malloc(sizeof *cfg);
    p->cfg_missing = cfg && ubootcfg_load(cfg, UBOOTMENU_CFG) == 0 ? ubootcfg_bootpart_missing(cfg) : -1;
    free(cfg);
    return 0;
}

int ubootrefresh_add_efi(struct ubootrefresh_plan *p) {
    if (!p->has_efi && !p->efi_kept_off) return -1;
    p->has_efi = 1;
    p->efi_kept_off = 0;
    return 0;
}

const char *ubootrefresh_plan(struct ubootrefresh_plan *p) {
    const char *no = plan_disk(p);
    if (no) return no;
    if (payload_pending())
        return "the boot loader in /install is replaced at the next restart, with the rest of the "
               "update -- restart first, then update the boot loader";
    return 0;
}

static void sayf(void (*say)(void *, const char *), void *ctx, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
static void sayf(void (*say)(void *, const char *), void *ctx, const char *fmt, ...) {
    char line[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (say) say(ctx, line);
}

// grub.cfg's half: every SaunaOS entry gains the bootpart= lines the
// payload's grub.cfg has, so the kernel roots on the disk GRUB booted from
// (docs/boot-flags.md). EDITED, never replaced -- the file is the
// machine's own, bootcfg and the Boot Manager write it, and grubby's rule
// is that a tool adds what it owns and leaves the rest. After the core
// image, because only a core with `probe` can answer it; either half
// without the other still boots. Entries changed, or -1.
static int add_bootpart(void (*say)(void *, const char *), void *ctx) {
    struct ubootcfg *cfg = malloc(sizeof *cfg), *before = malloc(sizeof *before);
    int rc = -1;
    if (!cfg || !before) { sayf(say, ctx, "grub.cfg not changed: out of memory"); goto out; }
    if (ubootcfg_load(cfg, UBOOTMENU_CFG) < 0) {
        sayf(say, ctx, "could not read %s -- left as it is.", UBOOTMENU_CFG);
        goto out;
    }
    int n = ubootcfg_bootpart_missing(cfg);
    if (!n) { rc = 0; goto out; }
    *before = *cfg;
    if (ubootcfg_add_bootpart(cfg) < 0) { sayf(say, ctx, "grub.cfg not changed: %s", cfg->why); goto out; }
    struct ubootcfg_problem probs[8];
    int np = ubootcfg_check(cfg, before, UBOOTCFG_CHECK_FILES, probs, 8);
    if (ubootcfg_has(probs, np, UBOOTCFG_BROKEN)) {
        sayf(say, ctx, "grub.cfg not changed -- the edit would not parse: %s", probs[0].msg);
        goto out;
    }
    const char *why = 0;
    if (ubootcfg_save(cfg, UBOOTMENU_CFG, &why) < 0) {
        sayf(say, ctx, "grub.cfg not changed: %s", why ? why : "the save failed");
        goto out;
    }
    rc = n;
out:
    free(cfg);
    free(before);
    return rc;
}

int ubootrefresh_run(const struct ubootrefresh_plan *p,
                     void (*say)(void *ctx, const char *line), void *ctx) {
    // THE MOVE UP FROM /boot/boot, around the write: the copy is made
    // while the old core still boots the old tree, and the old tree goes
    // only once a core reading /boot/grub is in place (base/ubootlayout.h).
    char dev[64] = "";
    const char *why = 0;
    if (p->legacy) {
        sayf(say, ctx, "copying /boot/boot up to /boot");
        if (uboot_writable(dev, sizeof dev) < 0) {
            sayf(say, ctx, "cannot remount /boot read-write -- nothing was changed.");
            return -1;
        }
        int ok = ubootlayout_copy_up(&why) == 0;
        if (!ok) ubootlayout_drop_flat();
        uboot_restore(dev);     // read-only again, which flushes the copy
        if (!ok) { sayf(say, ctx, "%s -- nothing was changed.", why); return -1; }
    }

    sayf(say, ctx, "writing the bootloader");
    const char *failed = uinstall_write_bootloader(p->disk, p->mbr, 1);
    if (failed) {
        sayf(say, ctx, "%s", failed);
        if (p->legacy && uboot_writable(dev, sizeof dev) == 0) {
            ubootlayout_drop_flat();
            uboot_restore(dev);
        }
        sayf(say, ctx, "FAILED -- %s still has the bootloader it had.", p->disk);
        return -1;
    }
    // THE UEFI LOADER AND THE RECORD, after the BIOS core: a machine set
    // to boot UEFI reads the loader from the ESP whatever the table is
    // (an MBR's 0xEF counts), and the record describes the core now in
    // the sectors.
    if (p->has_efi) sayf(say, ctx, "writing the UEFI loader");
    why = 0;
    int efi = p->has_efi ? -1 : 2, rec = -1;
    if (uboot_writable(dev, sizeof dev) == 0) {
        if (p->has_efi) efi = uinstall_write_efi("/boot", &why);
        rec = uinstall_write_record("/boot");
        uboot_restore(dev);
    } else {
        why = "cannot remount /boot read-write";
    }
    // `why` first: a failed remount wrote neither the loader nor the
    // record, and must not read as a loader deliberately kept off.
    if (why)
        sayf(say, ctx, "%s -- %s", why, p->has_efi ? "this disk still boots BIOS/CSM, not UEFI."
                                                   : "the record was not written.");
    else if (efi == 2 && p->efi_kept_off)
        sayf(say, ctx, "no UEFI loader added -- this disk boots BIOS/CSM and keeps doing so.");
    else if (efi == 1)
        sayf(say, ctx, "no " UINSTALL_PAYLOAD "/bootx64.efi (built without GRUB's "
             "EFI target) -- this disk boots BIOS/CSM only.");
    if (rec < 0)
        sayf(say, ctx, "could not write %s -- the bootloader is in place, but nothing "
             "records what it can do.", UBOOTREFRESH_RECORD);
    else
        unlink(UBOOTREFRESH_RECORD_OLD);   // superseded; a reader would fall back to it
    // The old loader's refusals may be its own failure to set a mode: the
    // BIOS core is always rewritten, the UEFI loader only when written.
    if (ubootmode_forget(1, efi == 0) < 0)
        sayf(say, ctx, "could not forget the refused screen sizes in " UBOOTMODE_CONF ".");
    why = 0;
    if (p->legacy) {
        sayf(say, ctx, "removing /boot/boot");
        int ok = uboot_writable(dev, sizeof dev) == 0 && ubootlayout_drop_legacy(&why) == 0;
        uboot_restore(dev);
        if (!ok)
            sayf(say, ctx, "%s -- the machine boots from /boot already; run "
                 "`install --migrate confirm` again to tidy it.",
                 why ? why : "cannot remount /boot read-write");
    }

    sayf(say, ctx, "adding bootpart= to grub.cfg");
    int added = add_bootpart(say, ctx);
    if (added > 0)
        sayf(say, ctx, "%d grub.cfg entr%s now pass%s bootpart= (the old file is %s.bak)",
             added, added == 1 ? "y" : "ies", added == 1 ? "es" : "", UBOOTMENU_CFG);
    else if (added == 0)
        sayf(say, ctx, "grub.cfg already passes bootpart=");
    sayf(say, ctx, "done. %s boots the newly written GRUB from now on%s.", p->disk,
         added < 0 ? "; grub.cfg is as it was" : "");
    return 0;
}

// The staged modules `have` lacks, space-separated, allocated.
static char *missing_from(const char *staged, const char *have) {
    size_t end = module_line(staged);
    char *out = malloc(end + 1);
    if (!out) return 0;
    size_t o = 0;
    for (size_t i = 0; i < end;) {
        while (i < end && (staged[i] == ' ' || staged[i] == '\t' || staged[i] == '\r')) i++;
        size_t w = i;
        while (i < end && staged[i] != ' ' && staged[i] != '\t' && staged[i] != '\r') i++;
        if (i == w || has_n(have, staged + w, i - w)) continue;
        if (o) out[o++] = ' ';
        memcpy(out + o, staged + w, i - w);
        o += i - w;
    }
    out[o] = 0;
    return out;
}

// Every module of `a` is in `b`.
static int subset(const char *a, const char *b) {
    char *m = missing_from(a, b);
    int r = m && !m[0];
    free(m);
    return r;
}

int ubootrefresh_offer(struct ubootrefresh_offer *o) {
    memset(o, 0, sizeof *o);
    o->kind = UBOOTREFRESH_SAME;
    struct ubootrefresh_plan p;
    if (plan_disk(&p)) return 0;
    o->pending = payload_pending();
    char *staged = read_text(o->pending ? UBOOTREFRESH_STAGED UPDATE_STAGED_SUFFIX : UBOOTREFRESH_STAGED);
    if (!staged) return 0;
    char *have = ubootrefresh_record();
    o->version = version_of(staged);
    o->installed = version_of(have);
    int rc = 0;
    if (!have) {
        o->kind = UBOOTREFRESH_UNKNOWN;
    } else if (!(o->adds = missing_from(staged, have))) {
        rc = -1;
    } else {
        // A missing staged version says nothing; a disk with none is older.
        int ver = !o->version ? 0 : !o->installed ? 1 : vercmp(o->version, o->installed);
        int drops = !subset(have, staged);
        if (drops || ver < 0) o->kind = UBOOTREFRESH_DIFFERENT;
        else if (o->adds[0] || ver > 0) o->kind = UBOOTREFRESH_NEWER;
    }
    free(staged);
    free(have);
    return rc;
}

void ubootrefresh_offer_free(struct ubootrefresh_offer *o) {
    free(o->adds);
    free(o->version);
    free(o->installed);
    o->adds = o->version = o->installed = 0;
}

void ubootrefresh_offer_detail(const struct ubootrefresh_offer *o, char *out, size_t cap) {
    if (!cap) return;
    out[0] = 0;
    if (o->kind != UBOOTREFRESH_NEWER && o->kind != UBOOTREFRESH_DIFFERENT) return;
    // The modules named up to three; past that a count, which fits a bar.
    int words = 0;
    for (const char *c = o->adds; c && *c; c++) words += (c == o->adds || c[-1] == ' ') && *c != ' ';
    char count[40] = "";
    if (words > 3) snprintf(count, sizeof count, "%d GRUB modules", words);
    const char *mods = words > 3 ? count : words ? o->adds : 0;
    int ver = o->version && (!o->installed || strcmp(o->version, o->installed));
    int n;
    if (mods && ver)
        n = snprintf(out, cap, "adds %s, GRUB %s%s%s", mods, o->installed ? o->installed : "",
                     o->installed ? " -> " : "", o->version);
    else if (mods)
        n = snprintf(out, cap, "adds %s", mods);
    else if (ver)
        n = snprintf(out, cap, "GRUB %s%s%s", o->installed ? o->installed : "",
                     o->installed ? " -> " : "", o->version);
    else
        n = snprintf(out, cap, "a different set of GRUB modules");
    if (n < 0 || (size_t)n >= cap) out[0] = 0;
}
