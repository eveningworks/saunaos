// uACPI against this machine's own firmware, checked by what kernel/acpi/
// already reads WITHOUT it: the `_S5_` byte scan and aml.c's walk of
// declarations. Each runs on the instance kacpid built (`acpi=uacpi`) or,
// on a default boot UNDER A HYPERVISOR, on one brought up here and taken
// down after -- so every `make test` runs the interpreter over QEMU's
// DSDT, while a real machine's _INI runs only when its boot asked.
//
// QEMU's _S5 is four zeros, so the first test alone would pass a uACPI
// that answered zeros for everything; the device and _STA tests are the
// ones that can tell.
#include <uacpi/uacpi.h>
#include <uacpi/namespace.h>
#include <uacpi/status.h>
#include <uacpi/types.h>

#include "acpi.h"
#include "aml.h"
#include "cpuinfo.h"   // cpu_info_get(), the hypervisor bit
#include "kfmt.h"   // klog_printf
#include "ktest.h"
#include "string.h"   // k_memcpy

struct uacpi_use {
    int mine;
};

static int under_hypervisor(void) {
    static struct cpu_info ci;   // large for a KTEST frame
    cpu_info_get(&ci);
    return cpu_has_feature(&ci, CPU_WORD_1_ECX, 31);
}

// 1 with an instance to test (`u->mine` when this test made it), 0 for a
// skip (`*why`), -1 when uACPI would not come up.
static int use_begin(struct uacpi_use *u, const char **why) {
    struct acpi_uacpi_info i;
    acpi_uacpi_info(&i);
    u->mine = 0;
    if (i.state == ACPI_UACPI_READY) return 1;
    if (i.state == ACPI_UACPI_LOADING) { *why = "kacpid is still loading"; return 0; }
    if (i.state == ACPI_UACPI_FAILED) return -1;
    if (!under_hypervisor()) { *why = "acpi=uacpi is off, and this is real firmware"; return 0; }
    u->mine = acpi_uacpi_bring_up_inline();
    if (u->mine) return 1;
    acpi_uacpi_tear_down_inline();
    return -1;
}

#define USE_BEGIN(u) do {                                           \
        const char *why_ = 0;                                       \
        int got_ = use_begin(u, &why_);                             \
        if (got_ == 0) KTEST_SKIP(why_);                            \
        KTEST_ASSERT(got_ == 1);                                    \
    } while (0)

static void use_end(struct uacpi_use *u) {
    if (u->mine) acpi_uacpi_tear_down_inline();
}

// aml.c keeps only the LAST segment of a declaration's name, so
// `Scope (\_SB) { Device (PCI0.PRES) }` -- QEMU's CPU-hotplug device --
// is recorded as \_SB.PRES where it really lives at \_SB.PCI0.PRES.
// So a path not found exactly is looked for as a Device of that name
// BENEATH the recorded scope, and `relocated` counts those.
struct by_name {
    uacpi_object_name want;
    uacpi_namespace_node *found;
};

static uacpi_iteration_decision match_name(void *user, uacpi_namespace_node *node, uacpi_u32 depth) {
    (void)depth;
    struct by_name *b = user;
    if (uacpi_namespace_node_name(node).id != b->want.id) return UACPI_ITERATION_DECISION_CONTINUE;
    b->found = node;
    return UACPI_ITERATION_DECISION_BREAK;
}

static uacpi_namespace_node *find_device(int n, const char *path, int *relocated) {
    uacpi_namespace_node *node = 0;
    if (uacpi_namespace_node_find(UACPI_NULL, path, &node) == UACPI_STATUS_OK && node) return node;
    const struct aml_node *a = aml_node_at(n);
    char scope[128];
    uacpi_namespace_node *parent = uacpi_namespace_root();
    if (a->parent != AML_ROOT) {
        if (!aml_path(a->parent, scope, sizeof scope) ||
            uacpi_namespace_node_find(UACPI_NULL, scope, &parent) != UACPI_STATUS_OK)
            return 0;
    }
    struct by_name b = { .found = 0 };
    k_memcpy(b.want.text, a->name, 4);
    uacpi_namespace_for_each_child(parent, match_name, UACPI_NULL, UACPI_OBJECT_DEVICE_BIT,
                                   UACPI_MAX_DEPTH_ANY, &b);
    if (b.found) (*relocated)++;
    return b.found;
}

static int s5_element(uacpi_object_array *a, uacpi_size i, uint64_t *out) {
    return i < a->count && uacpi_object_get_integer(a->objects[i], out) == UACPI_STATUS_OK;
}

KTEST("uacpi", "\\_S5 evaluates to the sleep types the byte scan read") {
    const struct acpi_state *s = acpi_get_state();
    if (!(s->flags & ACPI_F_TABLES)) KTEST_SKIP("no ACPI tables");
    if (!(s->flags & ACPI_F_S5)) KTEST_SKIP("the byte scan found no _S5_ to compare with");
    struct uacpi_use u;
    USE_BEGIN(&u);

    uacpi_object *obj = 0;
    uacpi_status st = uacpi_eval_typed(UACPI_NULL, "\\_S5", UACPI_NULL,
                                       UACPI_OBJECT_PACKAGE_BIT, &obj);
    uacpi_object_array a = { 0 };
    uint64_t ta = ~0ull, tb = ~0ull;
    int ok = st == UACPI_STATUS_OK && uacpi_object_get_package(obj, &a) == UACPI_STATUS_OK &&
             s5_element(&a, 0, &ta);
    // A one-element package carries both types (ACPICA reads it so too).
    if (ok && !s5_element(&a, 1, &tb)) tb = ta;
    if (obj) uacpi_object_unref(obj);
    use_end(&u);

    KTEST_ASSERT(ok);
    KTEST_ASSERT_EQ(ta & 7, (uint64_t)s->slp_typ_a);
    KTEST_ASSERT_EQ(tb & 7, (uint64_t)s->slp_typ_b);
}

KTEST("uacpi", "every Device the declaration walk found is a Device in uACPI's namespace") {
    if (!(acpi_get_state()->flags & ACPI_F_TABLES)) KTEST_SKIP("no ACPI tables");
    struct uacpi_use u;
    USE_BEGIN(&u);
    aml_build();

    int devices = 0, missing = 0, wrong = 0, relocated = 0;
    char path[128];
    for (int n = 0; n < aml_node_count(); n++) {
        const struct aml_node *a = aml_node_at(n);
        if (!a || a->kind != AML_KIND_DEVICE || !aml_path(n, path, sizeof path)) continue;
        devices++;
        uacpi_object_type t = UACPI_OBJECT_UNINITIALIZED;
        uacpi_namespace_node *node = find_device(n, path, &relocated);
        if (!node) {
            if (!missing++) klog_printf("uacpi test: %s is not in uACPI's namespace\n", path);
        } else if (uacpi_namespace_node_type(node, &t) != UACPI_STATUS_OK || t != UACPI_OBJECT_DEVICE) {
            if (!wrong++) klog_printf("uacpi test: %s is type %d there\n", path, (int)t);
        }
    }
    use_end(&u);
    if (relocated) klog_printf("uacpi test: %d device(s) found beneath their recorded scope\n", relocated);

    KTEST_ASSERT(devices > 0);
    KTEST_ASSERT_EQ(missing, 0);
    KTEST_ASSERT_EQ(wrong, 0);
}

struct sta_run {
    int evaluated, failed;
};

static uacpi_iteration_decision run_sta(void *user, uacpi_namespace_node *node, uacpi_u32 depth) {
    (void)depth;
    struct sta_run *r = user;
    uint64_t v = 0;
    uacpi_status st = uacpi_eval_simple_integer(node, "_STA", &v);
    if (st == UACPI_STATUS_NOT_FOUND) return UACPI_ITERATION_DECISION_CONTINUE;
    r->evaluated++;
    if (st != UACPI_STATUS_OK && !r->failed++)
        klog_printf("uacpi test: a _STA failed: %s\n", uacpi_status_to_string(st));
    return UACPI_ITERATION_DECISION_CONTINUE;
}

// Every Device's and Processor's _STA in uACPI's OWN namespace, so the ones the
// declaration walk cannot see (QEMU's hotplug CPUs, which read a port
// through \_SB.PCI0.PRES) run too.
KTEST("uacpi", "every Device's and Processor's _STA runs and returns an integer") {
    if (!(acpi_get_state()->flags & ACPI_F_TABLES)) KTEST_SKIP("no ACPI tables");
    struct uacpi_use u;
    USE_BEGIN(&u);
    struct sta_run r = { 0, 0 };
    uacpi_namespace_for_each_child(uacpi_namespace_root(), run_sta, UACPI_NULL,
                                   UACPI_OBJECT_DEVICE_BIT | UACPI_OBJECT_PROCESSOR_BIT,
                                   UACPI_MAX_DEPTH_ANY, &r);
    use_end(&u);

    if (r.evaluated == 0) KTEST_SKIP("this firmware declares no _STA");
    KTEST_ASSERT_EQ(r.failed, 0);
}
