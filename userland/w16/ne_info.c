// ne_info.c -- `w16 --info`: what an NE file holds, one fact a line. The
// format is tools/gen_ne.py --dump's, plus each import's name in
// brackets, so tools/w16_hostcheck.py can compare the two readers.
#include "w16_internal.h"

#include <stdio.h>

void ne_print_info(const struct ne_module *m) {
    printf("module %s\n", m->name);
    printf("entry %u:%04x stack %u:%04x autodata %u heap %u stacksize %u\n",
           m->entry_cs, m->entry_ip, m->ss, m->sp, m->autodata, m->heap, m->stack);
    char mods[NE_MAX_MODULES][16];
    printf("imports");
    for (unsigned i = 1; i <= m->nmod; i++) {
        if (ne_module_name(m, i, mods[i - 1], sizeof mods[0]) < 0)
            snprintf(mods[i - 1], sizeof mods[0], "?");
        printf(" %s", mods[i - 1]);
    }
    printf("\n");
    for (unsigned s = 1; s <= m->nseg; s++) {
        const struct ne_segment *seg = &m->seg[s - 1];
        printf("segment %u %s file %#x length %u minalloc %u flags %#06x\n", s,
               seg->flags & NE_SEG_DATA ? "data" : "code", (unsigned)seg->file_off,
               (unsigned)seg->length, (unsigned)seg->minalloc, seg->flags);
        for (unsigned i = 0; i < seg->nrelocs; i++) {
            struct ne_reloc r;
            if (ne_reloc_at(m, s, i, &r) < 0) break;
            unsigned kind = r.flags & 3;
            if (kind == NE_REL_IMPORT_ORD && r.a >= 1 && r.a <= m->nmod) {
                const struct w16_name *n = w16_name_lookup(mods[r.a - 1], r.b);
                printf("  reloc %04x src %u import %s.%u (%s)\n", r.site, r.src, mods[r.a - 1],
                       r.b, n ? n->name : "unknown");
            } else if (kind == NE_REL_INTERNAL) {
                printf("  reloc %04x src %u internal %u:%04x\n", r.site, r.src, r.a & 0xFF, r.b);
            } else {
                printf("  reloc %04x src %u flags %u\n", r.site, r.src, r.flags);
            }
        }
    }
    for (unsigned i = 0; i < m->nres; i++) {
        struct ne_resource r;
        if (ne_resource_at(m, i, &r) < 0) break;
        char type[40], id[40];
        if (r.type_name[0]) snprintf(type, sizeof type, "%s", r.type_name);
        else snprintf(type, sizeof type, "%u", r.type);
        if (r.name[0]) snprintf(id, sizeof id, "%s", r.name);
        else snprintf(id, sizeof id, "%u", r.id);
        printf("resource %s %s file %#x length %u\n", type, id, (unsigned)r.off, (unsigned)r.len);
    }
}
