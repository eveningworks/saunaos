/* The C language itself: structs and unions by value, bit-fields, function pointers,
 * varargs, setjmp, VLAs, compound literals, designated initialisers, _Generic,
 * computed goto, __int128, atomics and thread-local storage. */
#include <setjmp.h>
#include <stdalign.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct big { char c; double d; int a[5]; short s; };
struct bits { unsigned a : 3; signed b : 5; unsigned c : 12; unsigned : 0; unsigned d : 1; };
union pun { float f; uint32_t u; unsigned char b[4]; };

static struct big make_big(int k) {
    struct big b = { .c = (char)('a' + k), .d = k * 0.5, .a = { k, k + 1, [4] = k * 10 }, .s = (short)-k };
    return b;
}
static struct big twiddle(struct big b) { b.a[2] = b.a[0] + b.a[4]; b.d *= 3; return b; }

static int add(int a, int b) { return a + b; }
static int sub(int a, int b) { return a - b; }
static int mul(int a, int b) { return a * b; }
typedef int (*binop)(int, int);
static binop pick(char op) { return op == '+' ? add : op == '-' ? sub : mul; }

static double vavg(int n, ...) {
    va_list ap, ap2;
    va_start(ap, n);
    va_copy(ap2, ap);
    double s = 0;
    for (int i = 0; i < n; i++) s += va_arg(ap, double);
    double first = va_arg(ap2, double);
    va_end(ap2);
    va_end(ap);
    return s / n + first * 0;
}
static void vlog(char *out, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out, cap, fmt, ap);
    va_end(ap);
}
static long mixed(int n, ...) {
    va_list ap;
    va_start(ap, n);
    long r = 0;
    for (int i = 0; i < n; i++) {
        int kind = va_arg(ap, int);
        if (kind == 0) r += va_arg(ap, int);
        else if (kind == 1) r += (long)va_arg(ap, double);
        else { struct big b = va_arg(ap, struct big); r += b.a[4]; }
    }
    va_end(ap);
    return r;
}

static jmp_buf env;
static int depth_reached;
static void dive(int n) {
    volatile char pad[64];
    pad[0] = (char)n;
    depth_reached = n + (pad[0] != (char)n);
    if (n == 200) longjmp(env, n);
    dive(n + 1);
}

#define TYPE_NAME(x) _Generic((x), int: "int", double: "double", char *: "char*", default: "other")

static _Thread_local int tls_counter = 7;
static _Atomic long shared;

static int dispatch(int op) {
    static void *labels[] = { &&l0, &&l1, &&l2 };
    goto *labels[op];
l0: return 100;
l1: return 200;
l2: return 300;
}

static int vla_sum(int n) {
    int grid[n][n];
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++) grid[i][j] = i * n + j;
    int s = 0;
    for (int i = 0; i < n; i++) s += grid[i][n - 1 - i];
    return s + (int)(sizeof grid / sizeof grid[0][0]);
}

int main(void) {
    struct big b = twiddle(make_big(3));
    printf("struct %c %.2f %d %d %d %d %d %d\n", b.c, b.d, b.a[0], b.a[1], b.a[2], b.a[3], b.a[4], b.s);
    printf("layout %zu %zu %zu %zu\n", sizeof(struct big), offsetof(struct big, d), offsetof(struct big, s),
           alignof(struct big));
    struct bits bf = { 5, -9, 4095, 1 };
    bf.a += 3;
    bf.b -= 7;
    printf("bits %u %d %u %u %zu\n", bf.a, bf.b, bf.c, bf.d, sizeof bf);
    union pun p = { .f = -1.5f };
    printf("union %08x %u %u\n", p.u, p.b[3], p.b[0]);

    const char *ops = "+-*";
    for (int i = 0; i < 3; i++) printf("op %c %d\n", ops[i], pick(ops[i])(12, 5));
    binop table[] = { add, sub, mul };
    int acc = 1;
    for (int i = 0; i < 9; i++) acc = table[i % 3](acc, i + 1);
    printf("table %d\n", acc);

    printf("vavg %.3f\n", vavg(4, 1.0, 2.5, 3.5, 9.0));
    char line[64];
    vlog(line, sizeof line, "%s=%d,%c,%.1e", "v", -12, 'z', 31415.9);
    printf("vlog %s\n", line);
    printf("mixed %ld\n", mixed(3, 0, 40, 1, 2.9, 2, make_big(2)));

    int r = setjmp(env);
    if (r == 0) dive(0);
    else printf("longjmp %d depth %d\n", r, depth_reached);

    printf("generic %s %s %s %s\n", TYPE_NAME(1), TYPE_NAME(1.0), TYPE_NAME((char *)"s"), TYPE_NAME(1L));
    int *cl = (int[]){ 9, 8, 7 };
    printf("compound %d %d\n", cl[1], ((struct big){ .a[3] = 33 }).a[3]);
    printf("goto %d %d %d\n", dispatch(0), dispatch(1), dispatch(2));
    printf("vla %d\n", vla_sum(7));

    __int128 w = (__int128)UINT64_MAX * 3 + 5;
    unsigned __int128 q = (unsigned __int128)w / 7;
    printf("int128 %llu %llu %d\n", (unsigned long long)(w >> 64), (unsigned long long)q, (int)(w % 1000));

    for (int i = 0; i < 1000; i++) atomic_fetch_add(&shared, i);
    long expect = 499500;
    long loaded = atomic_load(&shared);
    int swapped = atomic_compare_exchange_strong(&shared, &expect, 1);
    printf("atomic %ld %d %ld\n", loaded, swapped, atomic_load(&shared));
    tls_counter += 5;
    printf("tls %d\n", tls_counter);

    unsigned x = 0xF0F00F0Fu;
    printf("builtins %d %d %d %u\n", __builtin_popcount(x), __builtin_clz(x >> 4), __builtin_ctz(x << 3),
           __builtin_bswap32(x));
    int ov;
    printf("overflow %d", __builtin_add_overflow(INT32_MAX, 1, &ov));
    printf(" %d\n", __builtin_mul_overflow(46341, 46341, &ov));
    return 0;
}
