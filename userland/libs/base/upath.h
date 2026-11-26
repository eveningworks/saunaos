#ifndef UPATH_H
#define UPATH_H

#ifdef __cplusplus
extern "C" {
#endif

// Paths for a ring-3 program, SIZED TO FIT: joining a directory and a
// name, and the PATH lookup for a program that has to RUN another one.
//
// Every answer here is a heap string the caller free()s -- glibc's
// getcwd(NULL, 0) and GIO's g_build_filename() shape. A fixed budget
// below FS_PATH_MAX is a deep folder that silently names a different
// one (the 64-byte tosh/ls budgets did exactly that).
//
// `kernel/shell/shell_path.c` is the KERNEL shell's version of the
// search and cannot use this one (ring 0, PATH from /etc/saunaos.conf),
// which is why the search ORDER is stated in both places. The list is
// fixed here rather than read from the environment: an inherited,
// mangled PATH silently running the wrong binary is worse.

// `dir` + "/" + `name`, with no doubled slash at the root ("/" + "x" is
// "/x") and `name` alone when `dir` is "" or NULL. NULL only when out of
// memory (errno ENOMEM).
char *upath_join(const char *dir, const char *name);

// Finds the program `name` names and stores a heap copy of its path in
// `*out` (free() it); `*out` is NULL unless the answer is 1. A name
// containing '/' is a path and is used as given.
//
// **THREE ANSWERS, NOT TWO**: 1 if `*out` names a program, 0 if no PATH
// entry had it, and -1 if the search could not be COMPLETED (errno says
// why: ENOMEM, or EMFILE from a probe). The third is not a shade of "no"
// -- a machine with a full fd table once reported "not found" for a
// command sitting right there. See docs/errno-design.md.
int upath_find_program(const char *name, char **out);

// The search list, by index, for anything that has to ENUMERATE what a
// bare name could be rather than resolve one -- Tab completion's first
// word. The same shape kernel/shell/shell.h exposes for the kernel shell's PATH,
// so one engine can walk either (api/completion.h).
int upath_dir_count(void);
const char *upath_dir(int index);

#ifdef __cplusplus
}
#endif

#endif
