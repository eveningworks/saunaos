// uitest-app: tools/hostchecks/uitest/throwxx
//
// The uixx exception guard (docs/uixx-design.md): a button's on_click
// throws. The toolkit is built with no unwind tables, here as on the
// guest, so a throw that reached its frames would terminate the process
// and this case would never report. Instead the window's loop must end,
// and ui::run() rethrow into main(), which catches it and returns 7.
#include <string.h>

#include "uitest_app.h"

int main(void) {
    uitest_begin("throwxx_app", "an exception from a C++ handler ends the loop and reaches main()");
    char *argv[] = { "throwxx", NULL };
    if (!uitest_app_start(1, argv)) return uitest_end();
    size_t mark = uitest_log_mark();
    uitest_app_click("boom");
    char line[128];
    utest_check(uitest_log_find(mark, "throwxx: caught", line, sizeof line) &&
                strstr(line, "from on_click") != NULL,
                "main() catches the handler's exception, with its message");
    uitest_check_int("main() returned what its catch returns", uitest_app_close(), 7);
    return uitest_end();
}
