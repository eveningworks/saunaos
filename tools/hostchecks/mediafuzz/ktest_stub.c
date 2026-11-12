// The KTESTs some kernel/lib sources carry inline (scsi.c) are linked, never run.
#include <stdint.h>
#include "ktest.h"
void ktest_fail(struct ktest_ctx *ctx, const char *expr, const char *file, int line) { (void)ctx; (void)expr; (void)file; (void)line; }
void ktest_fail_eq(struct ktest_ctx *ctx, const char *expr, int64_t got, int64_t expected, const char *file, int line) { (void)ctx; (void)expr; (void)got; (void)expected; (void)file; (void)line; }
