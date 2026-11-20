#!/usr/bin/env python3
"""tools/checks/check_libc_imports.py -- a shared library takes no NEW runtime call.

WHY THIS EXISTS
---------------
The sys_* functions (userland/libs/rt/) are linked into every PROGRAM,
and the shared libraries in /lib -- libc.so first -- find theirs in the
program that loaded them. So a library that starts calling a sys_*
function added since some program was built cannot be loaded beside that
program: ld-toy refuses, "undefined symbol", and the program exits 127
before main. e871e149 did exactly that with sys_prctl, and ScummVM --
an optional build made the day before, shipped as it was -- stopped
starting at all (scummvm_data_test, 2026-11-20).

WHAT IT CHECKS: every sys_* symbol a library in build/lib imports is in
the FROZEN set below: the ones every runtime still in use has had. A new
one fails, naming the library. The way out is the one prctl() took:
sys_call(SYS_X, ...), which every runtime there has been exports, with
the errno handled in the library. Putting a name in the set is a
decision to stop loading every program built before it.

Needs the build (build/lib); without one it says so and passes.
"""
import glob
import os
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
REPO = toolpath.REPO

# The runtime calls a library may import: the union of what /lib took
# when this was written, every one of which the 2026-11-19 ScummVM build
# exports. It may shrink freely.
FROZEN = {
    "sys_accept", "sys_bind", "sys_call", "sys_chdir", "sys_chmod",
    "sys_close", "sys_connect", "sys_console_size", "sys_dev_claim",
    "sys_dev_release", "sys_dup", "sys_dup2", "sys_dupfd", "sys_eject",
    "sys_eprint", "sys_errname", "sys_errno", "sys_execve", "sys_exit",
    "sys_fchmod", "sys_fcntl_lock", "sys_fd_cloexec", "sys_fork",
    "sys_fs_generation_of", "sys_fs_resize", "sys_fstat", "sys_fsync",
    "sys_ftruncate", "sys_futex_wait", "sys_futex_wake", "sys_futimens",
    "sys_getcwd", "sys_getdents", "sys_getpgid", "sys_getpid",
    "sys_getpriority", "sys_getrandom", "sys_getrlimit", "sys_getsid",
    "sys_gettid", "sys_gettime", "sys_install_boot", "sys_isatty", "sys_kill",
    "sys_link", "sys_listen", "sys_lseek", "sys_mkdir", "sys_mkfs",
    "sys_mkpart", "sys_mmap", "sys_monotonic_ns", "sys_mount",
    "sys_mount_image", "sys_mprotect", "sys_munmap", "sys_net_admin",
    "sys_net_arp_probe", "sys_net_config_replace", "sys_net_link",
    "sys_net_resolved", "sys_open", "sys_open_ctty", "sys_openpty",
    "sys_partedit", "sys_pci_info", "sys_pipe", "sys_pledge", "sys_poweroff",
    "sys_print", "sys_proc_info", "sys_query_record", "sys_read",
    "sys_recvfrom", "sys_rename", "sys_rename2", "sys_sbrk",
    "sys_sched_getscheduler", "sys_sched_setscheduler", "sys_sendto",
    "sys_set_abort_msg", "sys_set_nonblock", "sys_setpgid", "sys_setpriority",
    "sys_setrlimit", "sys_setsid", "sys_setting", "sys_shm_grant",
    "sys_shm_open", "sys_shm_open_from", "sys_shm_unlink", "sys_sigaction",
    "sys_signal", "sys_sigprocmask", "sys_sigsuspend", "sys_sleep_ms",
    "sys_snd_ctl", "sys_snd_format", "sys_snd_open", "sys_socket",
    "sys_spawn", "sys_spawn_argv", "sys_spawn_flags", "sys_spawn_opts",
    "sys_spawn_opts_init", "sys_stat", "sys_strerror", "sys_sync",
    "sys_tcgetattr", "sys_tcgetpgrp", "sys_tcgetsid", "sys_tcgetwinsz",
    "sys_tcsetattr", "sys_tcsetpgrp", "sys_tcsetwinsz", "sys_ticks",
    "sys_truncate", "sys_tty_raw", "sys_umount", "sys_unlink", "sys_unveil",
    "sys_usb_claim", "sys_usb_release", "sys_userfs", "sys_utime",
    "sys_utimens", "sys_waitpid", "sys_waitpid_check", "sys_waitpid_nohang",
    "sys_waitpid_untraced", "sys_wakeword", "sys_win_request", "sys_write",
    "sys_yield",
}


def imports(path):
    r = subprocess.run(["nm", "-D", "--undefined-only", path], capture_output=True, text=True)
    return {ln.split()[-1] for ln in r.stdout.splitlines() if ln.split() and ln.split()[-1].startswith("sys_")}


def main():
    libs = sorted(glob.glob(os.path.join(REPO, "build", "lib", "*.so")))
    if not libs:
        print("check_libc_imports: no build/lib -- nothing to check (run make all)")
        return 0
    problems = []
    for lib in libs:
        for sym in sorted(imports(lib) - FROZEN):
            problems.append(f"{os.path.basename(lib)} imports {sym}, which programs built before it "
                            f"do not have -- call sys_call(SYS_...) instead (see libc's prctl.c)")
    if problems:
        print(f"check_libc_imports: {len(problems)} problem(s)\n")
        for p in problems:
            print(f"  {p}")
        return 1
    print(f"check_libc_imports: ok -- {len(libs)} libraries import only runtime calls every "
          f"program has ({len(FROZEN)} frozen)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
