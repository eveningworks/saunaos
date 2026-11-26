// Names past the fields' old 128 bytes (fixed-tables round 4, track B):
// the file view's rename box opens holding a 250-byte name WHOLE, takes a
// character typed at its end, and a field made for a path holds one past
// 255 bytes. At the default size the box cut the name, and Enter renamed
// the file to the cut one.
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "ui/uui_fileview.h"
#include "ui/uui_textbox.h"
#include "rt/sys.h"
#include "uitest.h"

#define ROOT "/home/lc"   // this case's own (uitest_host.c)

static struct uui_fileview fv;
static struct uui_textbox tb;
static char name[256], file[300], path[600];

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    uitest_begin("listcaps", "a rename box and a path field past the 128-byte default");

    sys_mkdir(ROOT);
    memset(name, 'n', 246);
    strcpy(name + 246, ".txt");   // 250 bytes: under FS_NAME_MAX
    snprintf(file, sizeof file, "%s/%s", ROOT, name);
    utest_check(uitest_file(file, "x"), "a file with a 250-byte name");

    uui_fileview_init(&fv, 0, 0, 600, 400);
    utest_check(uui_fileview_set_dir(&fv, ROOT), "its folder lists");
    utest_check(uui_fileview_select_name(&fv, name) && uui_fileview_begin_rename(&fv),
                "rename in place begins");
    const char *t = uui_textbox_text(&fv.rename_box);
    utest_checkf(!strcmp(t, name), "the rename box holds the whole name (%d of %d bytes)",
                 (int)strlen(t), (int)strlen(name));
    uui_textbox_insert(&fv.rename_box, "Z");
    utest_checkf(!strcmp(uui_textbox_text(&fv.rename_box), "Z.txt"),
                 "typing replaces the selected stem, the extension kept (\"%.12s\")",
                 uui_textbox_text(&fv.rename_box));
    // A second rename re-opens the same box: it starts whole again.
    uui_fileview_cancel_rename(&fv);
    utest_check(uui_fileview_begin_rename(&fv) &&
                !strcmp(uui_textbox_text(&fv.rename_box), name), "a second rename starts whole again");
    uui_fileview_cancel_rename(&fv);

    // A field for a path: past 255 bytes.
    memset(path, 'p', 400);
    path[0] = '/';
    path[400] = '\0';
    utest_check(uui_textbox_init_max(&tb, path, UUI_TEXTBOX_PATH_MAX) == 0, "a path field is made");
    utest_checkf(!strcmp(uui_textbox_text(&tb), path), "it holds a 400-byte path (%d)",
                 (int)strlen(uui_textbox_text(&tb)));
    uui_textbox_release(&tb);
    utest_check(uui_textbox_text(&tb)[0] == '\0', "released, it is empty");

    return uitest_end();
}
