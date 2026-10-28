// RAM-sized ceilings for kernel tables -- kernel/include/kernel/klimit.h.
#include "klimit.h"
#include "pmm.h"
#include "klog.h"
#include "kfmt.h"
#include "string.h"
#include "heap.h"

uint32_t klimit_from_ram(uint64_t ram, uint32_t div, uint32_t bytes, uint32_t lo, uint32_t hi) {
    uint64_t n = (div && bytes) ? ram / div / bytes : lo;
    if (n < lo) n = lo;
    if (n > hi) n = hi;
    return (uint32_t)n;
}

static uint64_t ram_bytes(void) { return pmm_total_frames() * pmm_frame_size(); }

uint32_t klimit_get(struct klimit *l) {
    uint32_t v = __atomic_load_n(&l->value, __ATOMIC_RELAXED);
    if (v) return v;
    if (!l->ram_div) v = l->floor;
    else if (!pmm_total_frames()) return l->floor;   // too early to know
    else v = klimit_from_ram(ram_bytes(), l->ram_div, l->entry_bytes, l->floor, l->ceiling);
    __atomic_store_n(&l->value, v, __ATOMIC_RELAXED);
    return v;
}

void klimit_hit(struct klimit *l) {
    if (__atomic_exchange_n(&l->logged, 1, __ATOMIC_RELAXED)) return;
    if (l->ram_div)
        klog_printf(KLOG_WARN "limits: %s: ceiling of %u reached (sized from %lu MiB of RAM) "
                    "-- refused, and not logged again\n",
                    l->what, klimit_get(l), (unsigned long)(ram_bytes() >> 20));
    else
        klog_printf(KLOG_WARN "limits: %s: ceiling of %u reached -- refused, and not logged again\n",
                    l->what, klimit_get(l));
}

uint32_t klimit_test_set(struct klimit *l, uint32_t value) {
    uint32_t was = klimit_get(l);
    __atomic_store_n(&l->value, value, __ATOMIC_RELAXED);
    __atomic_store_n(&l->logged, 0, __ATOMIC_RELAXED);
    return was;
}

int klimit_test_logged(const struct klimit *l, uint64_t since) {
    char want[96];
    k_snprintf(want, sizeof want, "limits: %s: ceiling of", l->what);
    char *buf = kmalloc(4096);
    if (!buf) return 0;
    uint64_t first = 0;
    uint32_t n = 0;
    while (n + 1 < 4096) {
        uint32_t got = klog_read(since + n, buf + n, 4096 - 1 - n, &first);
        if (!got) break;
        n += got;
    }
    buf[n] = 0;
    int found = k_strstr(buf, want) != 0;
    kfree(buf);
    return found;
}
