#ifndef KERNEL_FSPERM_H
#define KERNEL_FSPERM_H
// FILE PERMISSIONS (users stage C, docs/users-design.md): may the
// CURRENT process do this to that path? Linux's inode_permission() and
// may_delete(), asked at the syscall boundary -- kernel/fs/fs_syscalls.c
// and the spawn path -- never inside the VFS, so the kernel's own reads
// and writes (an ELF's libraries, init's /run files) are not checked,
// as a kernel thread runs with root's credentials on Linux.
//
// Each answers 0, or -EACCES (a mode bit says no) / -EPERM (only the
// owner or root may). A path that is NOT THERE answers 0: the operation
// itself reports ENOENT, and a check must not turn that into EACCES.
// Every one also asks for SEARCH (x) on each directory above the path.
//
// ROOT (euid 0) passes everything but executing a file with no x bit
// at all. ENFORCED BY DEFAULT; `fileperm=permissive` logs what would
// have been refused and allows it (SELinux's permissive mode),
// `fileperm=off` asks nothing (docs/boot-flags.md), and a cred with
// CRED_PERM_ENFORCE is enforced whatever the boot says.
#include <stdint.h>

#define FSP_X 1u
#define FSP_W 2u
#define FSP_R 4u

int fsperm_may(const char *path, unsigned want);   // FSP_* on the path itself
int fsperm_may_search(const char *path);           // only the directories above it
int fsperm_may_create(const char *path);           // write + search on its parent
int fsperm_may_delete(const char *path);           // the same, plus the sticky bit's rule
int fsperm_may_own(const char *path);              // its owner, or root: chmod, utime
int fsperm_may_touch(const char *path);            // its owner or a writer: utime "now"
// chown: root sets anything; an owner may set the group to one of its
// own (FS_OWNER_KEEP leaves a half as it is).
int fsperm_may_chown(const char *path, uint32_t uid, uint32_t gid);

// The question a path syscall asks, as a parameter of
// resolve_user_path() (kernel/syscalls.h) beside the sandbox's, so none
// can forget to ask one.
enum fsp_ask {
    FSP_ASK_NONE,     // the syscall asks for itself (open), or is root's anyway
    FSP_ASK_SEARCH,   // stat, a watch: only the folders above it
    FSP_ASK_READ,     // a listing, an image to mount
    FSP_ASK_WRITE,    // truncate
    FSP_ASK_EXEC,     // spawn, exec, chdir (a folder's x is its search)
    FSP_ASK_CREATE,   // mkdir, a rename's or link's new name
    FSP_ASK_DELETE,   // unlink, a rename's old name
    FSP_ASK_OWN,      // chmod, utime to a given time
    FSP_ASK_TOUCH,    // utime to "now": the owner or a writer
};
int fsperm_ask(const char *path, enum fsp_ask ask);

#define FSPERM_OFF        0
#define FSPERM_PERMISSIVE 1
#define FSPERM_ENFORCE    2
int fsperm_mode(void);

#endif
