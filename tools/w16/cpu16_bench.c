// cpu16_bench.c -- the driver of tools/bench/cpu16_bench.py: how fast w16's
// CPU (userland/utils/w16/cpu16.c) runs 32-bit code, on the host.
//
// The blob (tools/w16/bench32.c, compiled -m32 and stripped to its bytes) is
// put at offset 0 of a 4 MiB FLAT segment -- code, data and stack one base,
// as a Win386 program's are -- and bench(0x100000) is called cdecl with its
// return address on a HLT. The run ends at that HLT; its instruction count
// and wall time give MIPS. The same C compiled for the host (bench_native)
// gives the checksum the interpreter's must equal, so a speed-up that broke
// an instruction shows as a FAIL, not as a faster number.
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cpu16.h"
#include "jit16.h"

uint32_t bench_native(uint8_t *buf);

#define FLAT (4u << 20)
#define ARG 0x100000u
#define HALT_AT 0x0FFF0u

static struct cpu16_desc g_desc[CPU16_MAX_DESC];

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: cpu16_bench BLOB ENTRY [RUNS [JIT-THRESHOLD]]\n");
        return 2;
    }
    unsigned entry = (unsigned)strtoul(argv[2], 0, 0), runs = argc > 3 ? (unsigned)atoi(argv[3]) : 3;
    int jit = argc > 4;                                      // the JIT, with this threshold (0: its default)
    unsigned threshold = jit ? (unsigned)atoi(argv[4]) : 0;
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    uint8_t *flat = calloc(1, FLAT);
    size_t n = fread(flat, 1, HALT_AT, f);
    fclose(f);
    if (!n || n >= HALT_AT) { fprintf(stderr, "blob empty or too big\n"); return 2; }

    uint8_t *native = calloc(1, FLAT);
    uint32_t want = bench_native(native);

    double best = 0;
    uint64_t steps = 0;
    int bad = 0;
    for (unsigned r = 0; r < runs; r++) {
        uint8_t *blob = malloc(n);
        memcpy(blob, flat, n);
        memset(flat, 0, FLAT);
        memcpy(flat, blob, n);
        free(blob);
        flat[HALT_AT] = 0xF4;                                // hlt: where bench() returns to
        struct cpu16 c;
        memset(&c, 0, sizeof c);
        memset(g_desc, 0, sizeof g_desc);
        c.desc = g_desc;
        g_desc[1] = (struct cpu16_desc){ .base = flat, .limit = FLAT - 1, .kind = D_CODE, .flags = CPU16_DF_BIG };
        g_desc[2] = (struct cpu16_desc){ .base = flat, .limit = FLAT - 1, .kind = D_DATA, .flags = CPU16_DF_BIG };
        c.sreg[S_CS] = cpu16_sel(1);
        c.sreg[S_DS] = c.sreg[S_ES] = c.sreg[S_SS] = c.sreg[S_FS] = c.sreg[S_GS] = cpu16_sel(2);
        uint32_t esp = FLAT - 16;
        memcpy(flat + esp - 4, &(uint32_t){ ARG }, 4);       // the argument
        memcpy(flat + esp - 8, &(uint32_t){ HALT_AT }, 4);   // the return address
        esp -= 8;
        c.r[R_SP].e = esp;
        c.ip = entry;
        c.flags = 0x0202;
        if (jit && jit16_init(&c, threshold) < 0) { fprintf(stderr, "no JIT arena\n"); return 2; }
        double t0 = now();
        cpu16_run(&c, 0);
        double t = now() - t0;
        if (jit) {
            const struct jit16_stats *s = jit16_stats(&c);
            printf("  jit: %llu blocks of %llu instructions (%llu KiB), %.1f%% of instructions run translated, "
                   "%llu runs, %llu left early, %llu refused\n",
                   (unsigned long long)s->blocks, (unsigned long long)s->insns,
                   (unsigned long long)(s->code_bytes >> 10), c.steps ? 100.0 * (double)s->run_insns / (double)c.steps : 0.0,
                   (unsigned long long)s->runs, (unsigned long long)s->exit_slow, (unsigned long long)s->refused);
            for (int k = 0; k < 256; k++)
                if (s->stop_op[k] || s->stop_0f[k])
                    printf("    stopped at %s%02x: %u\n", s->stop_0f[k] ? "0f " : "", k,
                           s->stop_0f[k] ? s->stop_0f[k] : s->stop_op[k]);
            jit16_free(&c);
        }
        uint32_t got = c.r[R_AX].e;
        if (c.fault != FAULT_HLT || got != want) {
            printf("FAIL run %u: %s at %04x:%x (%s), checksum %08x, the host's %08x\n", r,
                   cpu16_fault_name(c.fault), c.fault_cs, (unsigned)c.fault_ip, c.fault_why, got, want);
            bad = 1;
            break;
        }
        steps = c.steps;
        double mips = (double)c.steps / t / 1e6;
        if (mips > best) best = mips;
        printf("run %u: %llu instructions in %.3f s = %.1f MIPS, checksum %08x ok\n", r,
               (unsigned long long)c.steps, t, mips, got);
    }
    if (!bad) printf("best %.1f MIPS over %llu instructions\n", best, (unsigned long long)steps);
    free(flat);
    free(native);
    return bad;
}
