// dpmi16.c -- INT 31h, the DOS Protected Mode Interface a Windows 3.1
// program in enhanced mode could call: descriptors and linear memory, the
// part a Watcom Win386 program's supervisor needs to build the 32-bit flat
// segments its code runs in (docs/w16-design.md, "32-bit programs").
//
// What real systems do: Windows 3.1 itself was the DPMI host (0.9, 32-bit
// clients); Wine's 16-bit KERNEL answers INT 31h in krnl386's int31.c, on
// real LDT descriptors. w16 answers it on its own descriptor table, whose
// linear bases are the arena's (lin16.c): a descriptor DPMI sets reaches the
// arena modulo 2^32 and nothing else.
//
// Every call is AX's. Success clears CF; failure sets it with DPMI 1.0's
// error code in AX, as Windows' host did for the codes 0.9 left open. A
// function w16 lacks fails the same way (8001h, unsupported) rather than
// ending the program: a DPMI client is written to check CF.
#include "w16_internal.h"

#include <stdio.h>
#include <string.h>

#define DPMI_UNSUPPORTED 0x8001
#define DPMI_DESC_UNAVAIL 0x8011
#define DPMI_LINEAR_UNAVAIL 0x8012
#define DPMI_INVALID_VALUE 0x8021
#define DPMI_INVALID_SEL 0x8022
#define DPMI_INVALID_HANDLE 0x8023

static uint32_t pair(uint16_t hi, uint16_t lo) { return (uint32_t)hi << 16 | lo; }

static int ok(struct cpu16 *c) {
    c->flags &= (uint16_t)~F_CF;
    return 0;
}

static int fail(struct cpu16 *c, uint16_t code) {
    c->r[R_AX].w = code;
    c->flags |= F_CF;
    return 0;
}

// A selector the program may change: one it made itself (DPMI 0000h, AllocSelector) -- never w16's
// own (a segment, a global block), whose memory w16 manages.
static struct cpu16_desc *changeable(struct cpu16 *c, uint16_t sel) {
    unsigned i = cpu16_index(sel);
    if (i == 0 || i >= CPU16_MAX_DESC || (sel & 7) != 7) return 0;
    struct cpu16_desc *d = &c->desc[i];
    return (d->kind == D_DATA || d->kind == D_CODE) && d->owner == CPU16_OWNER_ALLOC ? d : 0;
}

// Any data or code selector, to read: DPMI's 0006h and 000Bh answer for w16's own as Windows' did.
static struct cpu16_desc *readable(struct cpu16 *c, uint16_t sel) {
    unsigned i = cpu16_index(sel);
    if (i == 0 || i >= CPU16_MAX_DESC || (sel & 7) != 7) return 0;
    struct cpu16_desc *d = &c->desc[i];
    return d->kind == D_DATA || d->kind == D_CODE ? d : 0;
}

// ES:DI, or ES:EDI from 32-bit code, as DPMI's buffer arguments are.
static uint8_t *es_di(struct cpu16 *c, uint32_t len, int write) {
    uint32_t off = c->cs32 ? c->r[R_DI].e : c->r[R_DI].w;
    uint8_t *p = cpu16_ptr32(c, c->sreg[S_ES], off, len, write);
    if (!p) c->fault = 0;   // a bad buffer is the program's error to see, not a fault of w16's
    return p;
}

// The 8 bytes of a descriptor as the 386 lays one out.
static void desc_bytes(struct cpu16 *c, uint16_t sel, const struct cpu16_desc *d, uint8_t out[8]) {
    uint32_t base = 0;
    cpu16_linear_base(c, sel, &base);
    uint32_t lim = d->limit;
    int g = lim > 0xFFFFF;
    if (g) lim >>= 12;
    uint8_t access = d->kind == D_CODE ? 0xFA : 0xF2;   // present, DPL 3, code readable / data writable
    out[0] = (uint8_t)lim;
    out[1] = (uint8_t)(lim >> 8);
    out[2] = (uint8_t)base;
    out[3] = (uint8_t)(base >> 8);
    out[4] = (uint8_t)(base >> 16);
    out[5] = access;
    out[6] = (uint8_t)((lim >> 16 & 0x0F) | (g ? 0x80 : 0) | (d->flags & CPU16_DF_BIG ? 0x40 : 0));
    out[7] = (uint8_t)(base >> 24);
}

// Access rights as DPMI 0009h takes them: CL the access byte (bit 3 code),
// CH's bit 6 the D/B bit. G is the limit's business (0008h sets the limit in
// bytes, as w16 keeps it), so it is not kept.
static int set_rights(struct cpu16_desc *d, uint8_t access, uint8_t ext) {
    if (!(access & 0x10) || !(access & 0x80)) return -1;   // a system descriptor, or not present
    d->kind = access & 0x08 ? D_CODE : D_DATA;
    if (ext & 0x40) d->flags |= CPU16_DF_BIG;
    else d->flags &= (uint8_t)~CPU16_DF_BIG;
    return 0;
}

static int alloc_descs(struct cpu16 *c, unsigned n) {
    if (n == 0) return fail(c, DPMI_INVALID_VALUE);
    // A data descriptor with base and limit 0, as the spec has them -- a linear base, so 0007h's is kept.
    uint16_t first = cpu16_alloc_run(c, n, D_DATA, 0, n * 0x10000u, 0, 0);
    if (!first) return fail(c, DPMI_DESC_UNAVAIL);
    for (unsigned k = 0; k < n; k++) {
        struct cpu16_desc *d = &c->desc[cpu16_index(first) + k];
        *d = (struct cpu16_desc){ .kind = D_DATA, .flags = CPU16_DF_LIN, .owner = CPU16_OWNER_ALLOC };
    }
    cpu16_desc_changed(c);
    c->r[R_AX].w = first;
    return ok(c);
}

int w16_dpmi(struct cpu16 *c) {
    uint16_t ax = c->r[R_AX].w, bx = c->r[R_BX].w, cx = c->r[R_CX].w, dx = c->r[R_DX].w;
    if (g_w16_trace) {
        fprintf(g_w16_trace, "INT 31h AX=%04x BX=%04x CX=%04x DX=%04x SI=%04x DI=%04x\n", ax, bx, cx, dx,
                c->r[R_SI].w, c->r[R_DI].w);
        fflush(g_w16_trace);
    }
    struct cpu16_desc *d;
    // dispatch-ok: one case per DPMI 0.9 function w16 answers, a set the spec fixed in 1990.
    switch (ax) {
    case 0x0000: return alloc_descs(c, cx);                           // allocate LDT descriptors
    case 0x0001:                                                       // free one
        if (!(d = changeable(c, bx))) return fail(c, DPMI_INVALID_SEL);
        memset(d, 0, sizeof *d);
        cpu16_desc_changed(c);
        return ok(c);
    case 0x0003: c->r[R_AX].w = 8; return ok(c);                        // the selector increment
    case 0x0006: {                                                     // get the base
        uint32_t base;
        if (!readable(c, bx) || cpu16_linear_base(c, bx, &base) < 0) return fail(c, DPMI_INVALID_SEL);
        c->r[R_CX].w = (uint16_t)(base >> 16);
        c->r[R_DX].w = (uint16_t)base;
        return ok(c);
    }
    case 0x0007:                                                       // set the base
        if (!changeable(c, bx)) return fail(c, DPMI_INVALID_SEL);
        cpu16_set_linear_base(c, bx, pair(cx, dx));
        return ok(c);
    case 0x0008: {                                                     // set the limit, in bytes
        if (!(d = changeable(c, bx))) return fail(c, DPMI_INVALID_SEL);
        uint32_t lim = pair(cx, dx);
        if (lim > 0xFFFFF && (lim & 0xFFF) != 0xFFF) return fail(c, DPMI_INVALID_VALUE);   // page granular past 1 MB
        d->limit = lim;
        cpu16_desc_changed(c);
        return ok(c);
    }
    case 0x0009:                                                       // set access rights
        if (!(d = changeable(c, bx)) || set_rights(d, (uint8_t)cx, (uint8_t)(cx >> 8)) < 0)
            return fail(c, d ? DPMI_INVALID_VALUE : DPMI_INVALID_SEL);
        cpu16_desc_changed(c);
        return ok(c);
    case 0x000A: {                                                     // a data alias of a code descriptor
        if (!(d = readable(c, bx))) return fail(c, DPMI_INVALID_SEL);
        uint32_t base;
        if (cpu16_linear_base(c, bx, &base) < 0) return fail(c, DPMI_INVALID_SEL);
        struct cpu16_desc src = *d;
        if (alloc_descs(c, 1) || (c->flags & F_CF)) return 0;
        struct cpu16_desc *a = &c->desc[cpu16_index(c->r[R_AX].w)];
        *a = (struct cpu16_desc){ .kind = D_DATA, .flags = (uint8_t)(CPU16_DF_LIN | (src.flags & CPU16_DF_BIG)),
                                  .lin = base, .limit = src.limit, .owner = CPU16_OWNER_ALLOC };
        cpu16_desc_changed(c);
        return ok(c);
    }
    case 0x000B: {                                                     // get the descriptor
        if (!(d = readable(c, bx))) return fail(c, DPMI_INVALID_SEL);
        uint8_t *p = es_di(c, 8, 1);
        if (!p) return fail(c, DPMI_INVALID_VALUE);
        desc_bytes(c, bx, d, p);
        return ok(c);
    }
    case 0x000C: {                                                     // set it
        if (!(d = changeable(c, bx))) return fail(c, DPMI_INVALID_SEL);
        const uint8_t *p = es_di(c, 8, 0);
        if (!p) return fail(c, DPMI_INVALID_VALUE);
        struct cpu16_desc n = *d;
        if (set_rights(&n, p[5], p[6]) < 0) return fail(c, DPMI_INVALID_VALUE);
        uint32_t lim = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)(p[6] & 0x0F) << 16;
        n.limit = p[6] & 0x80 ? lim << 12 | 0xFFF : lim;
        *d = n;
        cpu16_set_linear_base(c, bx, (uint32_t)p[2] | (uint32_t)p[3] << 8 | (uint32_t)p[4] << 16 | (uint32_t)p[7] << 24);
        return ok(c);
    }
    case 0x0400:                                                       // the version: 0.9, 32-bit clients
        c->r[R_AX].w = 0x005A;
        c->r[R_BX].w = 0x0001;                                           // 32-bit programs; no real-mode reflection
        c->r[R_CX].w = 3;                                                // a 386
        c->r[R_DX].w = 0x0870;                                           // the PICs' bases, as a PC had them
        return ok(c);
    case 0x0500: {                                                     // free memory information
        uint8_t *p = es_di(c, 0x30, 1);
        if (!p) return fail(c, DPMI_INVALID_VALUE);
        memset(p, 0xFF, 0x30);                                         // unknown, as the spec allows
        uint32_t big = lin16_largest_free(), pages = lin16_free_bytes() >> 12;
        memcpy(p + 0x00, &big, 4);                                     // the largest free block
        uint32_t bigp = big >> 12;
        memcpy(p + 0x04, &bigp, 4);                                    // ...in pages, unlocked
        memcpy(p + 0x08, &bigp, 4);                                    // ...locked
        memcpy(p + 0x14, &pages, 4);                                   // free pages
        return ok(c);
    }
    case 0x0501: {                                                     // allocate a memory block
        uint32_t size = pair(bx, cx);
        uint8_t *p = size ? lin16_alloc(size, 4096) : 0;
        uint32_t lin;
        if (!p) return fail(c, DPMI_LINEAR_UNAVAIL);
        lin = c->lin_origin + (uint32_t)(p - c->lin);
        c->r[R_BX].w = c->r[R_SI].w = (uint16_t)(lin >> 16);              // the handle is its linear address
        c->r[R_CX].w = c->r[R_DI].w = (uint16_t)lin;
        return ok(c);
    }
    case 0x0502: {                                                     // free one
        uint8_t *p = cpu16_linear_ptr(c, pair(c->r[R_SI].w, c->r[R_DI].w), 1);
        if (!p || !lin16_size_of(p)) return fail(c, DPMI_INVALID_HANDLE);
        lin16_free(p);
        return ok(c);
    }
    case 0x0503: {                                                     // resize one: it may move
        uint8_t *p = cpu16_linear_ptr(c, pair(c->r[R_SI].w, c->r[R_DI].w), 1);
        uint32_t size = pair(bx, cx);
        if (!p || !lin16_size_of(p)) return fail(c, DPMI_INVALID_HANDLE);
        if (!size) return fail(c, DPMI_INVALID_VALUE);
        uint8_t *n = lin16_realloc(p, size);
        if (!n) return fail(c, DPMI_LINEAR_UNAVAIL);
        uint32_t lin = c->lin_origin + (uint32_t)(n - c->lin);
        c->r[R_BX].w = c->r[R_SI].w = (uint16_t)(lin >> 16);
        c->r[R_CX].w = c->r[R_DI].w = (uint16_t)lin;
        return ok(c);
    }
    case 0x0600: case 0x0601: case 0x0602: case 0x0603:               // lock and unlock: nothing pages out
    case 0x0702: case 0x0703:                                          // page hints
        return ok(c);
    case 0x0604: c->r[R_BX].w = 0; c->r[R_CX].w = 4096; return ok(c);     // the page size
    default:
        return fail(c, DPMI_UNSUPPORTED);
    }
}
