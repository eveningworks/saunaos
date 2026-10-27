// The boot-menu reader and the environment-block writer behind `reboot
// --entry` and the Leave page's "Restart into" (lib/ubootmenu.h).
//
// WHAT A BROKEN VERSION WOULD STILL PASS, which is what shaped this:
//
//   - A reader that counted every `menuentry` line would pass a flat
//     grub.cfg; the fixture nests a `submenu`, whose entries GRUB does
//     not number at the top level.
//   - A writer checked only by reading back through the same library
//     proves nothing; the block's bytes are checked here directly,
//     including its exact size -- GRUB refuses any other.
//   - A writer that rebuilt the block from scratch would pass every
//     next_entry check and drop GRUB's own variables; one is planted
//     and must survive both a set and a clear.
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "lib/ubootmenu.h"
#include "lib/utest.h"

#define CFG  "/tmp/bootmenu_test.cfg"
#define CFG2 "/tmp/bootmenu_test2.cfg"
#define ENV  "/tmp/bootmenu_test.env"

static int put(const char *path, const char *text, int len) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return -1;
    long w = write(fd, text, (size_t)len);
    close(fd);
    return w == len ? 0 : -1;
}

static int get(const char *path, char *buf, int cap) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    int n = 0;
    long r;
    while (n < cap && (r = read(fd, buf + n, (size_t)(cap - n))) > 0) n += (int)r;
    close(fd);
    return n;
}

int main(void) {
    utest_begin("bootmenu_test", "the GRUB menu reader and the one-shot writer", 0);

    static const char cfg[] =
        "# a comment naming menuentry \"not one\"\n"
        "set timeout=5\n"
        "set default=\"SaunaOS (b)\"\n"
        "if [ \"${next_entry}\" ]; then\n"
        "    set default=\"${next_entry}\"\n"
        "fi\n"
        "menuentry 'SaunaOS (a)' {\n"
        "    multiboot2 /kernel.bin\n"
        "}\n"
        "submenu \"More\" {\n"
        "    menuentry \"hidden\" { boot }\n"
        "}\n"
        "menuentry \"SaunaOS (b)\" --class os {\n"
        "    multiboot2 /kernel.bin debugcon\n"
        "}\n"
        "menuentry bare {\n"
        "    boot\n"
        "}\n";
    utest_check(put(CFG, cfg, (int)sizeof cfg - 1) == 0, "the fixture is written");

    static struct ubootmenu m;
    int n = ubootmenu_read(&m, CFG);
    utest_checkf(n == 3, "three top-level entries (got %d) -- the submenu's is not one", n);
    utest_check(n == 3 && !strcmp(m.title[0], "SaunaOS (a)") && !strcmp(m.title[1], "SaunaOS (b)") &&
                !strcmp(m.title[2], "bare"), "single-quoted, double-quoted and bare titles");
    utest_checkf(m.def == 1, "`set default=` by title picks entry 1 (got %d)", m.def);
    utest_check(ubootmenu_find(&m, "2") == 2 && ubootmenu_find(&m, "3") == -1,
                "a number names an entry in range and nothing past it");
    utest_check(ubootmenu_find(&m, "SaunaOS (b)") == 1 && ubootmenu_find(&m, "SaunaOS") == -1,
                "a title matches exactly, never as a prefix");

    static const char plain[] = "set default=0\nmenuentry \"only\" { boot }\n";
    put(CFG2, plain, (int)sizeof plain - 1);
    static struct ubootmenu m2;
    ubootmenu_read(&m2, CFG2);
    utest_check(!m2.oneshot && m2.why_not, "a grub.cfg without the stanza cannot take a choice");

    // GRUB's block, with one of GRUB's own variables in it.
    static char blk[1024];
    static const char head[] = "# GRUB Environment Block\nsaved_entry=x\n";
    memset(blk, '#', sizeof blk);
    memcpy(blk, head, sizeof head - 1);
    put(ENV, blk, (int)sizeof blk);

    static char got[2048];
    utest_check(ubootmenu_set_next_at(ENV, "SaunaOS (b)") == 0, "a choice is written");
    int len = get(ENV, got, sizeof got);
    got[len > 0 ? len : 0] = 0;
    utest_checkf(len == 1024, "the block is exactly 1024 bytes (got %d)", len);
    utest_check(!strncmp(got, "# GRUB Environment Block\n", 25), "with GRUB's header first");
    utest_check(strstr(got, "\nnext_entry=SaunaOS (b)\n") != 0, "and next_entry naming the title");
    utest_check(strstr(got, "\nsaved_entry=x\n") != 0, "GRUB's own variable survives the set");

    utest_check(ubootmenu_set_next_at(ENV, 0) == 0, "a choice is cleared");
    len = get(ENV, got, sizeof got);
    got[len > 0 ? len : 0] = 0;
    utest_check(len == 1024 && !strstr(got, "next_entry="), "leaving no next_entry");
    utest_check(strstr(got, "\nsaved_entry=x\n") != 0, "and GRUB's variable survives the clear");

    utest_check(ubootmenu_set_next_at(ENV, "a\\b") < 0 && ubootmenu_set_next_at(ENV, "a\nb") < 0,
                "a title GRUB would read back differently is refused");

    // Any variable: two set, one read back, one removed, the others kept.
    // The LONGER name first, so a prefix match would find it.
    utest_check(ubootmenu_env_set_at(ENV, "toy_mode_try", "1024x768") == 0 &&
                ubootmenu_env_set_at(ENV, "toy_mode", "1920x1080") == 0, "two variables are written");
    static char v[64];
    utest_check(ubootmenu_env_get_at(ENV, "toy_mode", v, sizeof v) && !strcmp(v, "1920x1080"),
                "a variable is read by its whole name, not as the prefix of a longer one");
    utest_check(ubootmenu_env_get_at(ENV, "toy_mode_try", v, sizeof v) && !strcmp(v, "1024x768"),
                "and the longer one by its own");
    utest_check(ubootmenu_env_set_at(ENV, "toy_mode_try", 0) == 0 &&
                !ubootmenu_env_get_at(ENV, "toy_mode_try", v, sizeof v) &&
                ubootmenu_env_get_at(ENV, "toy_mode", v, sizeof v) && !strcmp(v, "1920x1080") &&
                ubootmenu_env_get_at(ENV, "saved_entry", v, sizeof v) && !strcmp(v, "x"),
                "removing one keeps the rest");
    len = get(ENV, got, sizeof got);
    utest_checkf(len == 1024, "the block is still exactly 1024 bytes (got %d)", len);

    unlink(CFG);
    unlink(CFG2);
    unlink(ENV);
    return utest_end();
}
