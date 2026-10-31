// ELF32 and ELF64, either byte order: the header, the program headers
// (with the interpreter and the notes they point at) and the section
// headers named from .shstrtab. The System V gABI's layouts.
#include "lib/ubinmap_internal.h"

#include <stdio.h>
#include <string.h>

// How many table entries are LISTED; the table's own node still covers
// all of them. A count in the header is a field like any other.
#define ELF_LIST_MAX 4096
#define NOTES_MAX    256
#define SEGS_MAX     16     // interpreter and note segments shown

static const char *type_name(unsigned t) {
    switch (t) {
    case 0: return "none";
    case 1: return "relocatable";
    case 2: return "executable";
    case 3: return "shared object";
    case 4: return "core dump";
    default: return "type";
    }
}

static const char *machine_name(unsigned m) {
    switch (m) {
    case 0: return "no machine";
    case 2: return "SPARC";
    case 3: return "x86";
    case 8: return "MIPS";
    case 20: return "PowerPC";
    case 21: return "PowerPC64";
    case 40: return "ARM";
    case 62: return "x86-64";
    case 183: return "AArch64";
    case 243: return "RISC-V";
    default: return 0;
    }
}

static const char *ptype_name(uint32_t t) {
    switch (t) {
    case 0: return "NULL";
    case 1: return "LOAD";
    case 2: return "DYNAMIC";
    case 3: return "INTERP";
    case 4: return "NOTE";
    case 5: return "SHLIB";
    case 6: return "PHDR";
    case 7: return "TLS";
    case 0x6474e550: return "GNU_EH_FRAME";
    case 0x6474e551: return "GNU_STACK";
    case 0x6474e552: return "GNU_RELRO";
    case 0x6474e553: return "GNU_PROPERTY";
    default: return 0;
    }
}

static const char *stype_name(uint32_t t) {
    static const char *const n[] = { "NULL", "PROGBITS", "SYMTAB", "STRTAB", "RELA", "HASH", "DYNAMIC",
                                     "NOTE", "NOBITS", "REL", "SHLIB", "DYNSYM", "type 12", "type 13",
                                     "INIT_ARRAY", "FINI_ARRAY", "PREINIT_ARRAY", "GROUP", "SYMTAB_SHNDX" };
    if (t < sizeof n / sizeof n[0]) return n[t];
    if (t == 0x6ffffff6) return "GNU_HASH";
    if (t == 0x6ffffffe) return "VERNEED";
    if (t == 0x6fffffff) return "VERSYM";
    return "OS/processor";
}

struct elf {
    int is64;
    uint64_t phoff, shoff;
    unsigned phentsize, phnum, shentsize, shnum, shstrndx;
};

static void notes(struct ubinmap_ctx *c, int depth, uint64_t off, uint64_t len) {
    uint64_t end = off + len, at = off;
    if (end < off || end > c->src->size) end = c->src->size;
    for (int k = 0; k < NOTES_MAX && at + 12 <= end; k++) {
        uint32_t namesz, descsz, type;
        if (bm_u32(c, at, &namesz) || bm_u32(c, at + 4, &descsz) || bm_u32(c, at + 8, &type)) return;
        uint64_t size = 12 + (((uint64_t)namesz + 3) & ~3ull) + (((uint64_t)descsz + 3) & ~3ull);
        char owner[64];
        bm_str(c, at + 12, namesz < 63 ? namesz : 63, owner, sizeof owner);
        if (bm_add(c, depth, at, size, 0, owner[0] ? owner : "(no owner)", "type %u, %u bytes", type, descsz) < 0)
            return;
        if (size > end - at) {
            bm_note(c, "a note at 0x%llX runs past its segment", (unsigned long long)at);
            return;
        }
        at += size;
    }
}

static void program_headers(struct ubinmap_ctx *c, const struct elf *e) {
    unsigned want = e->is64 ? 56 : 32;
    if (!e->phnum) return;
    if (e->phentsize < want) {
        bm_note(c, "program header entries of %u bytes are too small for ELF%d", e->phentsize, e->is64 ? 64 : 32);
        return;
    }
    uint64_t span = (uint64_t)e->phentsize * e->phnum;
    struct seg { uint32_t type; uint64_t off, len; } seg[SEGS_MAX];
    int nseg = 0;
    if (bm_add(c, 0, e->phoff, span, 0, "Program headers", "%u", e->phnum) < 0) return;
    if (e->phoff >= c->src->size || span > c->src->size - e->phoff)
        bm_note(c, "the program header table runs past the end of the file");
    for (unsigned i = 0; i < e->phnum && i < ELF_LIST_MAX && !bm_full(c); i++) {
        uint64_t at = e->phoff + (uint64_t)i * e->phentsize;
        uint8_t p[56];
        if (at < e->phoff || bm_read(c, at, p, want)) break;
        uint32_t type = bm_get32(c, p), flags;
        uint64_t off, vaddr, filesz, memsz, align;
        if (e->is64) {
            flags = bm_get32(c, p + 4);
            off = bm_get64(c, p + 8);
            vaddr = bm_get64(c, p + 16);
            filesz = bm_get64(c, p + 32);
            memsz = bm_get64(c, p + 40);
            align = bm_get64(c, p + 48);
        } else {
            off = bm_get32(c, p + 4);
            vaddr = bm_get32(c, p + 8);
            filesz = bm_get32(c, p + 16);
            memsz = bm_get32(c, p + 20);
            flags = bm_get32(c, p + 24);
            align = bm_get32(c, p + 28);
        }
        char label[48], rwx[4] = { flags & 4 ? 'r' : '-', flags & 2 ? 'w' : '-', flags & 1 ? 'x' : '-', 0 };
        const char *tn = ptype_name(type);
        if (tn) snprintf(label, sizeof label, "%s %s", tn, rwx);
        else snprintf(label, sizeof label, "0x%X %s", (unsigned)type, rwx);
        if (bm_add(c, 1, at, e->phentsize, 2 + (int)(i & 1), label, "0x%04llX", (unsigned long long)off) < 0) break;
        unsigned o = e->is64 ? 8 : 4, w = e->is64 ? 8 : 4;
        bm_add(c, 2, at, 4, 0, "Type", "%s", tn ? tn : "OS/processor");
        bm_add(c, 2, at + (e->is64 ? 4 : 24), 4, 0, "Flags", "%s", rwx);
        bm_add(c, 2, at + o, w, 0, "Offset", "0x%llX", (unsigned long long)off);
        bm_add(c, 2, at + o + w, w, 0, "Virtual address", "0x%llX", (unsigned long long)vaddr);
        bm_add(c, 2, at + o + 3 * w, w, 0, "File size", "%llu", (unsigned long long)filesz);
        bm_add(c, 2, at + o + 4 * w, w, 0, "Memory size", "%llu", (unsigned long long)memsz);
        bm_add(c, 2, at + (e->is64 ? 48 : 28), w, 0, "Align", "0x%llX", (unsigned long long)align);
        // What a segment POINTS AT is a top-level node of its own, added
        // after the table: a depth-0 node here would end the table's run.
        if ((type == 3 || type == 4) && nseg < SEGS_MAX)
            seg[nseg++] = (struct seg){ type, off, filesz };
    }
    if (e->phnum > ELF_LIST_MAX) bm_note(c, "%u program headers; the first %d are listed", e->phnum, ELF_LIST_MAX);
    for (int i = 0; i < nseg && !bm_full(c); i++) {
        if (seg[i].type == 3) {   // PT_INTERP: the loader's path, a string in the file
            char path[128];
            bm_str(c, seg[i].off, seg[i].len < 127 ? (size_t)seg[i].len : 127, path, sizeof path);
            bm_add(c, 0, seg[i].off, seg[i].len, 4, "Interpreter", "%s", path);
        } else if (bm_add(c, 0, seg[i].off, seg[i].len, 5, "Notes", "%llu bytes",
                          (unsigned long long)seg[i].len) >= 0) {
            notes(c, 1, seg[i].off, seg[i].len);
        }
    }
}

static void section_headers(struct ubinmap_ctx *c, const struct elf *e) {
    unsigned want = e->is64 ? 64 : 40;
    if (!e->shnum) return;
    if (e->shentsize < want) {
        bm_note(c, "section header entries of %u bytes are too small for ELF%d", e->shentsize, e->is64 ? 64 : 32);
        return;
    }
    uint64_t span = (uint64_t)e->shentsize * e->shnum;
    if (bm_add(c, 0, e->shoff, span, 6, "Section headers", "%u", e->shnum) < 0) return;
    if (e->shoff >= c->src->size || span > c->src->size - e->shoff)
        bm_note(c, "the section header table runs past the end of the file");
    // .shstrtab: the names' table, where its own header says.
    uint64_t str_off = 0, str_len = 0;
    if (e->shstrndx < e->shnum) {
        uint8_t h[64];
        uint64_t at = e->shoff + (uint64_t)e->shstrndx * e->shentsize;
        if (!bm_read(c, at, h, want)) {
            str_off = e->is64 ? bm_get64(c, h + 24) : bm_get32(c, h + 16);
            str_len = e->is64 ? bm_get64(c, h + 32) : bm_get32(c, h + 20);
        }
    }
    for (unsigned i = 0; i < e->shnum && i < ELF_LIST_MAX && !bm_full(c); i++) {
        uint64_t at = e->shoff + (uint64_t)i * e->shentsize;
        uint8_t h[64];
        if (bm_read(c, at, h, want)) break;
        uint32_t name = bm_get32(c, h), type = bm_get32(c, h + 4);
        uint64_t addr = e->is64 ? bm_get64(c, h + 16) : bm_get32(c, h + 12);
        uint64_t off = e->is64 ? bm_get64(c, h + 24) : bm_get32(c, h + 16);
        uint64_t size = e->is64 ? bm_get64(c, h + 32) : bm_get32(c, h + 20);
        char label[64] = "";
        // The name is read only from INSIDE .shstrtab, and never past it.
        if (str_len && name < str_len) {
            uint64_t room = str_len - name;
            bm_str(c, str_off + name, room < 63 ? (size_t)room : 63, label, sizeof label);
        }
        if (!label[0]) snprintf(label, sizeof label, "[%u]", i);
        if (bm_add(c, 1, at, e->shentsize, 0, label, "%s", stype_name(type)) < 0) break;
        bm_add(c, 2, at, 4, 0, "Name", "%u", name);
        bm_add(c, 2, at + 4, 4, 0, "Type", "%s", stype_name(type));
        bm_add(c, 2, at + (e->is64 ? 16 : 12), e->is64 ? 8 : 4, 0, "Address", "0x%llX", (unsigned long long)addr);
        bm_add(c, 2, at + (e->is64 ? 24 : 16), e->is64 ? 8 : 4, 0, "Offset", "0x%llX", (unsigned long long)off);
        bm_add(c, 2, at + (e->is64 ? 32 : 20), e->is64 ? 8 : 4, 0, "Size", "%llu", (unsigned long long)size);
        if (type != 8 && type != 0 && size)   // NOBITS has no bytes in the file
            bm_add(c, 2, off, size, 0, "Contents", "0x%llX", (unsigned long long)off);
    }
    if (e->shnum > ELF_LIST_MAX) bm_note(c, "%u section headers; the first %d are listed", e->shnum, ELF_LIST_MAX);
}

int ubinmap_elf(struct ubinmap_ctx *c) {
    uint8_t h[64];
    if (bm_read(c, 0, h, 16) || memcmp(h, "\x7f" "ELF", 4)) return 0;
    int cls = h[4], data = h[5];
    if ((cls != 1 && cls != 2) || (data != 1 && data != 2)) return 0;
    struct elf e = { .is64 = cls == 2 };
    c->be = data == 2;
    unsigned hsize = e.is64 ? 64 : 52;
    if (bm_read(c, 0, h, hsize)) {
        // A header cut short is still an ELF file: what is there is shown.
        bm_format(c, "ELF%d file, cut short", e.is64 ? 64 : 32);
        bm_add(c, 0, 0, c->src->size, 1, "ELF header", "cut short");
        bm_note(c, "the file ends inside the ELF header");
        return 1;
    }
    unsigned type = bm_get16(c, h + 16), mach = bm_get16(c, h + 18);
    uint64_t entry;
    unsigned ehsize;
    if (e.is64) {
        entry = bm_get64(c, h + 24);
        e.phoff = bm_get64(c, h + 32);
        e.shoff = bm_get64(c, h + 40);
        ehsize = bm_get16(c, h + 52);
        e.phentsize = bm_get16(c, h + 54);
        e.phnum = bm_get16(c, h + 56);
        e.shentsize = bm_get16(c, h + 58);
        e.shnum = bm_get16(c, h + 60);
        e.shstrndx = bm_get16(c, h + 62);
    } else {
        entry = bm_get32(c, h + 24);
        e.phoff = bm_get32(c, h + 28);
        e.shoff = bm_get32(c, h + 32);
        ehsize = bm_get16(c, h + 40);
        e.phentsize = bm_get16(c, h + 42);
        e.phnum = bm_get16(c, h + 44);
        e.shentsize = bm_get16(c, h + 46);
        e.shnum = bm_get16(c, h + 48);
        e.shstrndx = bm_get16(c, h + 50);
    }
    const char *mn = machine_name(mach);
    char mbuf[24];
    if (!mn) { snprintf(mbuf, sizeof mbuf, "machine %u", mach); mn = mbuf; }
    bm_format(c, "ELF%d %s, %s", e.is64 ? 64 : 32, type_name(type), mn);

    int w = e.is64 ? 8 : 4, at = 24;
    bm_add(c, 0, 0, hsize, 1, "ELF header", "%u bytes", hsize);
    bm_add(c, 1, 0, 4, 0, "Magic", "7F 45 4C 46");
    bm_add(c, 1, 4, 1, 0, "Class", "ELF%d", e.is64 ? 64 : 32);
    bm_add(c, 1, 5, 1, 0, "Data", "%s", c->be ? "big-endian" : "little-endian");
    bm_add(c, 1, 6, 1, 0, "Version", "%u", h[6]);
    bm_add(c, 1, 7, 1, 0, "OS ABI", "%u", h[7]);
    bm_add(c, 1, 16, 2, 0, "Type", "%s (%u)", type_name(type), type);
    bm_add(c, 1, 18, 2, 0, "Machine", "%s (%u)", mn, mach);
    bm_add(c, 1, at, w, 0, "Entry", "0x%llX", (unsigned long long)entry);
    bm_add(c, 1, at + w, w, 0, "Program headers", "0x%llX, %u x %u", (unsigned long long)e.phoff, e.phnum, e.phentsize);
    bm_add(c, 1, at + 2 * w, w, 0, "Section headers", "0x%llX, %u x %u", (unsigned long long)e.shoff, e.shnum, e.shentsize);
    bm_add(c, 1, at + 3 * w + 4, 2, 0, "Header size", "%u", ehsize);
    bm_add(c, 1, hsize - 2, 2, 0, "Names section", "%u", e.shstrndx);
    program_headers(c, &e);
    section_headers(c, &e);
    return 1;
}
