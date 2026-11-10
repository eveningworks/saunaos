/* <stdlib.h>, <string.h> and <ctype.h>: conversions, sorting, searching, memory. */
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int cmp_int(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}
static int cmp_str(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

int main(void) {
    char *end;
    printf("strtol %ld %ld %ld\n", strtol("  -123xyz", &end, 10), strtol("0x1F", NULL, 16),
           strtol("777", NULL, 8));
    printf("strtol-end [%s]\n", end);
    errno = 0;
    long big = strtol("99999999999999999999", NULL, 10);
    printf("strtol-range %d %d\n", big == LONG_MAX, errno == ERANGE);
    printf("strtoul %lu strtoll %lld strtoull %llu\n", strtoul("4294967295", NULL, 0),
           strtoll("-9223372036854775808", NULL, 10), strtoull("18446744073709551615", NULL, 10));
    printf("strtod %.17g %.17g %.17g %g\n", strtod("0.1", NULL), strtod("1e-310", NULL),
           strtod("123456789012345678901234567890", NULL), strtod("-inf", NULL));
    printf("strtod-hex %g strtof %.9g atof %g atoi %d\n", strtod("0x1.8p3", NULL),
           (double)strtof("3.4028235e38", NULL), atof("2.5e-1"), atoi("  42"));

    int v[20];
    for (int i = 0; i < 20; i++) v[i] = (i * 7919) % 23 - 11;
    qsort(v, 20, sizeof v[0], cmp_int);
    for (int i = 0; i < 20; i++) printf("%d%c", v[i], i == 19 ? '\n' : ' ');
    int key = 6;
    int *hit = bsearch(&key, v, 20, sizeof v[0], cmp_int);
    printf("bsearch %s\n", hit ? "found" : "missing");
    const char *words[] = { "pear", "apple", "fig", "banana", "cherry" };
    qsort(words, 5, sizeof words[0], cmp_str);
    printf("words %s %s %s %s %s\n", words[0], words[1], words[2], words[3], words[4]);

    char s[64] = "Hello, World";
    printf("strlen %zu strchr %s strrchr %s strstr %s\n", strlen(s), strchr(s, 'o'), strrchr(s, 'o'),
           strstr(s, "Wor"));
    printf("strspn %zu strcspn %zu strpbrk %s\n", strspn(s, "HeloW"), strcspn(s, ", "), strpbrk(s, "dW"));
    strncat(s, "!!!???", 3);
    printf("strncat [%s] cmp %d %d %d\n", s, strcmp("a", "b") < 0, strncmp("abcd", "abcf", 3),
           memcmp("\x01\xff", "\x01\x00", 2) > 0);
    char copy[64];
    memset(copy, 'x', sizeof copy);
    memcpy(copy, s, 5);
    copy[5] = 0;
    memmove(s + 2, s, 10);
    printf("memcpy [%s] memmove [%.14s]\n", copy, s);
    char tok[] = "a,b;;c,,d";
    for (char *t = strtok(tok, ",;"); t; t = strtok(NULL, ",;")) printf("tok %s\n", t);
    char *dup = strdup("duplicate");
    printf("strdup %s %s\n", dup, strerror(ENOENT) ? "has-strerror" : "none");
    free(dup);

    int up = 0, dig = 0, sp = 0, pun = 0;
    for (const char *p = "Mixed Case 123, with: punct!"; *p; p++) {
        up += isupper((unsigned char)*p) != 0;
        dig += isdigit((unsigned char)*p) != 0;
        sp += isspace((unsigned char)*p) != 0;
        pun += ispunct((unsigned char)*p) != 0;
    }
    printf("ctype %d %d %d %d %c %c\n", up, dig, sp, pun, toupper('q'), tolower('Q'));

    div_t dv = div(-17, 5);
    ldiv_t lv = ldiv(1000000007L, 13);
    printf("div %d %d ldiv %ld %ld abs %d labs %ld\n", dv.quot, dv.rem, lv.quot, lv.rem, abs(-9), labs(-90L));

    void *blocks[64];
    size_t total = 0;
    for (int i = 0; i < 64; i++) {
        blocks[i] = malloc((size_t)(i * 37 + 1));
        memset(blocks[i], i, (size_t)(i * 37 + 1));
        total += (size_t)(i * 37 + 1);
    }
    for (int i = 0; i < 64; i += 2) blocks[i] = realloc(blocks[i], (size_t)(i * 100 + 1));
    int ok = 1;
    for (int i = 1; i < 64; i += 2) ok &= ((unsigned char *)blocks[i])[i * 37] == (unsigned char)i;
    for (int i = 0; i < 64; i++) free(blocks[i]);
    int *z = calloc(1000, sizeof(int));
    for (int i = 0; i < 1000; i++) ok &= z[i] == 0;
    free(z);
    printf("malloc %zu %s\n", total, ok ? "intact" : "CORRUPT");
    return 0;
}
