// SDL3's filesystem module for SaunaOS (SDL_FILESYSTEM_PRIVATE).
//
// The program's path is the kernel's record of it (base/uself.h), not
// /proc/self/exe, which SaunaOS has not got. The user's folders are the
// personal folders base/udirs.h names; SaunaOS has no accounts, so
// /home is the one user's home. Moving, listing and making files is
// SDL's POSIX fsops (SDL_FSOPS_POSIX), unchanged.
#include "SDL_internal.h"

#ifdef SDL_FILESYSTEM_PRIVATE

#include "filesystem/SDL_sysfilesystem.h"
#include "base/udirs.h"
#include "base/uself.h"
#include "query_abi.h"

static char *exe_path(void)
{
    char buf[QUERY_PROCPATH_MAX];
    if (uself_path(buf, sizeof buf) < 0 || buf[0] != '/') {
        SDL_SetError("The kernel has no path for this program");
        return NULL;
    }
    return SDL_strdup(buf);
}

char *SDL_SYS_GetBasePath(void)
{
    char *path = exe_path();
    if (path) {
        SDL_strrchr(path, '/')[1] = '\0';   // keep the trailing '/'
    }
    return path;
}

char *SDL_SYS_GetExeName(void)
{
    char *path = exe_path();
    if (path) {
        const char *name = SDL_strrchr(path, '/') + 1;
        SDL_memmove(path, name, SDL_strlen(name) + 1);
    }
    return path;
}

// Where a program keeps its own files is not decided yet
// (docs/sdl-design.md, stage 3).
char *SDL_SYS_GetPrefPath(const char *org, const char *app)
{
    (void)org;
    (void)app;
    SDL_Unsupported();
    return NULL;
}

char *SDL_SYS_GetUserFolder(SDL_Folder folder)
{
    const char *dir;
    switch (folder) {
    case SDL_FOLDER_HOME:        dir = "/home"; break;
    case SDL_FOLDER_DESKTOP:     dir = udir_path(UDIR_DESKTOP); break;
    case SDL_FOLDER_DOCUMENTS:   dir = udir_path(UDIR_DOCUMENTS); break;
    case SDL_FOLDER_DOWNLOADS:   dir = udir_path(UDIR_DOWNLOADS); break;
    case SDL_FOLDER_MUSIC:       dir = udir_path(UDIR_MUSIC); break;
    case SDL_FOLDER_PICTURES:    dir = udir_path(UDIR_PICTURES); break;
    case SDL_FOLDER_SCREENSHOTS: dir = udir_path(UDIR_SCREENSHOTS); break;
    case SDL_FOLDER_VIDEOS:      dir = udir_path(UDIR_VIDEOS); break;
    default:
        // Public, Saved Games and Templates: SaunaOS has no such folder.
        SDL_SetError("SaunaOS has no such folder");
        return NULL;
    }
    if (!dir || !*dir) {
        SDL_SetError("The folder is not set");
        return NULL;
    }
    char *result = NULL;
    if (SDL_asprintf(&result, "%s/", dir) < 0) {
        return NULL;
    }
    return result;
}

#endif // SDL_FILESYSTEM_PRIVATE
