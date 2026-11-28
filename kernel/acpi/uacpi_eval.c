// SYS_ACPI_EVAL: one AML object evaluated from ring 3 (abi/syscall_abi.h).
//
// A HOLE, CLOSED BY DEFAULT, as `faultinject` closes SYS_CRASHTEST: any
// method the firmware has can be run, with any arguments, so the call is
// refused unless the kernel was booted with `acpieval`, and its row is
// PROMISE_ADMIN. It exists for the QEMU fixture's battery and lid
// (tools/tests/fixtures/acpi/), which a test drives through methods the
// fixture defines, and for reading a real machine's firmware by hand.
//
// It runs in the caller's syscall, not on kacpid: AML may reach the
// embedded controller and park, which a syscall's context may do (as
// power-off's _PTS does). uACPI serialises the namespace itself.
#include <uacpi/uacpi.h>
#include <uacpi/namespace.h>
#include <uacpi/status.h>
#include <uacpi/types.h>

#include "acpi.h"
#include "errno.h"
#include "kfmt.h"
#include "klog.h"
#include "multiboot.h"
#include "scheduler.h"
#include "string.h"
#include "syscall_abi.h"
#include "syscalls.h"
#include "vmm.h"

static int64_t eval(struct acpi_eval_msg *m) {
    if (!multiboot_cmdline_has("acpieval")) return -EPERM;
    struct acpi_uacpi_info u;
    acpi_uacpi_info(&u);
    if (u.state != ACPI_UACPI_READY) return -ENODEV;
    m->path[sizeof m->path - 1] = 0;
    // A path without its leading backslash is the root's anyway (uACPI
    // looks a NULL parent's path up from the root): a shell eats the `\`.
    if (!m->path[0] || m->nargs > ACPI_EVAL_ARGS_MAX) return -EINVAL;

    uacpi_object *argv[ACPI_EVAL_ARGS_MAX];
    uint32_t made = 0;
    for (; made < m->nargs; made++) {
        argv[made] = uacpi_object_create_integer(m->args[made]);
        if (!argv[made]) break;
    }
    int64_t rc = 0;
    if (made < m->nargs) {
        rc = -ENOMEM;
    } else {
        uacpi_object_array args = { argv, made };
        uacpi_object *ret = UACPI_NULL;
        uacpi_status st = uacpi_eval(UACPI_NULL, m->path, made ? &args : UACPI_NULL, &ret);
        klog_printf("acpi: pid %d evaluated %s (%u argument(s)): %s\n", scheduler_current_pid(), m->path,
                    (unsigned)made, uacpi_status_to_string(st));
        if (st == UACPI_STATUS_NOT_FOUND) {
            rc = -ENOENT;
        } else if (st != UACPI_STATUS_OK) {
            rc = -EIO;
        } else {
            uacpi_u64 v = 0;
            m->type = !ret ? ACPI_EVAL_NONE
                    : uacpi_object_get_integer(ret, &v) == UACPI_STATUS_OK ? ACPI_EVAL_INTEGER : ACPI_EVAL_OTHER;
            m->value = v;
        }
        if (ret) uacpi_object_unref(ret);
    }
    for (uint32_t i = 0; i < made; i++) uacpi_object_unref(argv[i]);
    return rc;
}

int sys_acpi_eval(struct syscall_ctx *c) {
    struct acpi_eval_msg m;
    int64_t rc;
    if (!vmm_copy_from_user(c->pml4, &m, c->a0, sizeof m)) {
        rc = -EFAULT;
    } else {
        m.type = ACPI_EVAL_NONE;
        m.value = 0;
        rc = eval(&m);
        if (rc == 0 && !vmm_copy_to_user(c->pml4, c->a0, &m, sizeof m)) rc = -EFAULT;
    }
    c->regs[14] = (uint64_t)rc;
    return 0;
}
