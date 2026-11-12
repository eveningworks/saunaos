// WHAT THE KERNEL COPIES OUT, IT WROTE -- NO BYTE OF ITS STACK RIDES ALONG.
//
// Four calls filled a struct on the kernel stack field by field and then
// copied the whole struct to ring 3, so every byte they skipped -- a
// name's tail past its NUL, a padding hole, the unused end of a record --
// was whatever the kernel stack held: fragments of directory blocks,
// other processes' data, kernel pointers. Linux's rule is a memset before
// copy_to_user; each is zeroed first now.
//
//   proc_info  SYS_PROC_INFO: a name's tail, the hole before delay_disk_ns
//   listdir    SYS_LISTDIR: each entry's name tail, the rtc_time pad byte
//   gettime    SYS_GETTIME: the rtc_time pad byte
//   query      SYS_QUERY's kernel log: data[] past each record's len
//
// The buffers are filled with 0xA5 first, so a byte the kernel did not
// define reads as either 0 (zeroed, right) or something else (leaked).
// Each call runs many times between other calls, so the kernel stack
// under it is not freshly zero by luck.
//
// It may be `run` or spawned.
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "rt/sys.h"
#include "base/utest.h"
#include "query_abi.h"

#define ROUNDS 50
#define MAX_ENTS 64

// A stat and a directory listing between rounds: they leave the kernel
// stack full of filesystem scratch, which is what a skipped byte copied.
static void churn(void) {
    struct sys_stat st;
    sys_stat("/bin", &st);
    sys_stat("/etc/settings.d", &st);
}

static size_t used_len(const char *s, size_t cap) {
    size_t n = 0;
    while (n < cap && s[n]) n++;
    return n;
}

static int zero(const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) if (b[i]) return 0;
    return 1;
}

int main(void) {
    utest_begin("leak_test", "no kernel stack byte reaches ring 3", 0);

    unsigned bad_proc = 0;
    for (int r = 0; r < ROUNDS; r++) {
        churn();
        struct proc_info pi;
        memset(&pi, 0xA5, sizeof pi);
        if (sys_proc_info(PROC_INFO_SELF, &pi) != 0) { bad_proc++; continue; }
        size_t used = used_len(pi.name, sizeof pi.name);
        size_t hole = offsetof(struct proc_info, reserved_sandbox) + sizeof pi.reserved_sandbox;
        if (!zero(pi.name + used, sizeof pi.name - used) ||
            !zero((uint8_t *)&pi + hole, offsetof(struct proc_info, delay_disk_ns) - hole))
            bad_proc++;
    }
    utest_checkf(bad_proc == 0, "proc_info: the name's tail and the padding are zero (%u of %d dirty)",
                 bad_proc, ROUNDS);

    static struct sys_dirent ents[MAX_ENTS];
    unsigned bad_dir = 0, seen = 0;
    for (int r = 0; r < ROUNDS; r++) {
        churn();
        memset(ents, 0xA5, sizeof ents);
        int n = sys_listdir("/bin", ents, MAX_ENTS);
        for (int i = 0; i < n; i++) {
            size_t used = used_len(ents[i].name, sizeof ents[i].name);
            const uint8_t *pad = (const uint8_t *)&ents[i].modified + 5;   // rtc_time's hole
            if (!zero(ents[i].name + used, sizeof ents[i].name - used) || *pad) bad_dir++;
            seen++;
        }
    }
    utest_checkf(seen > 0 && bad_dir == 0, "listdir: each name's tail and the rtc pad are zero "
                 "(%u of %u entries dirty)", bad_dir, seen);

    unsigned bad_time = 0;
    for (int r = 0; r < ROUNDS; r++) {
        churn();
        struct rtc_time t;
        memset(&t, 0xA5, sizeof t);
        if (sys_gettime(&t) != 0 || ((const uint8_t *)&t)[5]) bad_time++;
    }
    utest_checkf(bad_time == 0, "gettime: the rtc pad byte is zero (%u of %d dirty)", bad_time, ROUNDS);

    unsigned bad_q = 0, records = 0;
    for (int r = 0; r < 8; r++) {
        churn();
        for (unsigned i = 0; i < 64; i++) {
            struct query_klog k;
            memset(&k, 0xA5, sizeof k);
            if (sys_query_record(QUERY_KLOG, i, &k, sizeof k) <= 0) break;
            if (k.len < sizeof k.data && !zero(k.data + k.len, sizeof k.data - k.len)) bad_q++;
            records++;
        }
    }
    utest_checkf(records > 0 && bad_q == 0, "query: a kernel-log record past its len is zero "
                 "(%u of %u records dirty)", bad_q, records);
    return utest_end();
}
