// lib/ugrow.h: UGROW grows a typed array and, when it cannot, leaves
// the array and its capacity exactly as they were.
//
// WHAT A BROKEN VERSION WOULD STILL PASS: one that never grew passes
// under its first allocation, so the array is filled well past it and
// every element read back; one that clobbered the pointer on failure
// passes every success, so a request no int-sized capacity can meet is
// made and the old contents checked afterwards. Exits with the number
// of failures.
#include "lib/ugrow.h"
#include "lib/utest.h"

struct pair { int a, b; };

int main(void) {
    utest_begin("ugrow_test", "UGROW grows, and fails without losing anything", UTEST_VERDICT_FILE);

    struct pair *v = NULL;
    int n = 0, cap = 0, ok = 1;
    for (int i = 0; i < 1000; i++) {
        if (!UGROW(v, cap, n + 1)) { ok = 0; break; }
        v[n++] = (struct pair){ i, -i };
    }
    utest_checkf(ok && n == 1000, "a thousand appended (%d)", n);
    int intact = 1;
    for (int i = 0; i < n; i++) if (v[i].a != i || v[i].b != -i) intact = 0;
    utest_check(intact, "every element survived each move");
    utest_checkf(cap >= n && cap < 2 * n + UGROW_FIRST, "the capacity doubles, no more (%d)", cap);

    int cap_was = cap;
    struct pair *v_was = v;
    utest_check(UGROW(v, cap, n) == 1 && v == v_was && cap == cap_was,
                "asking for what it already holds moves nothing");

    utest_check(UGROW(v, cap, 0x7fffffff) == 0, "an impossible size is refused");
    utest_check(v == v_was && cap == cap_was, "...and the array and capacity are unchanged");
    utest_check(v[999].a == 999, "...and still hold their contents");

    char (*names)[16] = NULL;   // a pointer to arrays: the shape Help uses
    int ncap = 0;
    utest_check(UGROW(names, ncap, 3) && ncap >= 3, "a pointer-to-array grows too");

    free(v);
    free(names);
    return utest_end();
}
