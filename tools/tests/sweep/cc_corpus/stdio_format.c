/* printf's conversions, flags and widths, and the scanf family reading them back. */
#include <inttypes.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    printf("[%d] [%5d] [%-5d] [%05d] [%+d] [% d]\n", 42, 42, 42, 42, 42, 42);
    printf("[%i] [%u] [%o] [%x] [%X] [%#o] [%#x]\n", -7, 4000000000u, 8, 255, 255, 8, 255);
    printf("[%ld] [%lld] [%hd] [%hhd] [%zu] [%td]\n", LONG_MIN, LLONG_MAX, (short)-3, (char)200,
           sizeof(long double), (ptrdiff_t)-9);
    printf("[%" PRId64 "] [%" PRIx32 "] [%" PRIuMAX "]\n", INT64_MIN, UINT32_MAX, UINTMAX_MAX);
    printf("[%c] [%5c] [%s] [%.3s] [%8s] [%-8s|]\n", 'q', 'r', "string", "truncate", "right", "left");
    printf("[%f] [%.0f] [%.10f] [%10.3f] [%-10.2f|]\n", 3.14159, 2.5, 1.0 / 7, -0.0005, 99.999);
    printf("[%e] [%.2E] [%g] [%G] [%g] [%g]\n", 12345.678, 0.000123, 100000.0, 1e-5, 1e15, 0.1);
    printf("[%a] [%.3a] [%A]\n", 1.0, 3.14159, -0.5);
    printf("[%f] [%f] [%e] [%g]\n", 1e300 * 1e10, -(1e300 * 1e10), 0.0, -0.0);
    printf("[%*d] [%-*d] [%.*f]\n", 6, 7, 6, 7, 2, 1.23456);
    printf("[%Lf] [%Le]\n", 1.5L, 123.25L);
    printf("[%%]\n");
    int n = 0;
    printf("abc%n", &n);
    printf(" n=%d\n", n);

    char buf[16];
    int r = snprintf(buf, sizeof buf, "%s-%d-%s", "abcdefgh", 12345, "tail");
    printf("snprintf %d [%s]\n", r, buf);
    r = snprintf(NULL, 0, "%08.3f", 3.0);
    printf("snprintf-null %d\n", r);

    int a, b;
    char word[16];
    double d;
    unsigned x;
    r = sscanf("  17 -4 hello 2.5e3 ff", "%d %d %15s %lf %x", &a, &b, word, &d, &x);
    printf("sscanf %d: %d %d %s %.1f %u\n", r, a, b, word, d, x);
    r = sscanf("12abc", "%d%3c", &a, word);
    word[3] = 0;
    printf("sscanf %d: %d %s\n", r, a, word);
    r = sscanf("key=value;rest", "%[^=]=%[^;]", word, buf);
    printf("sscanf %d: %s %s\n", r, word, buf);
    r = sscanf("", "%d", &a);
    printf("sscanf-empty %d\n", r);
    return 0;
}
