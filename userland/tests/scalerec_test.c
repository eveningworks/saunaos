// uui_scale_recommended(): the quarter step a screen is offered first,
// pinned on the two machines it was calibrated on (both 125%) and on the
// ends of its range, so a retuned factor shows up here as a number.
#include "ui/uui_scalesample.h"
#include "lib/utest.h"

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    utest_begin("scalerec_test", "uui_scale_recommended", UTEST_VERDICT_FILE);
    utest_check(uui_scale_recommended(1920, 294) == 125, "a 13.3\" 1920x1080 panel: 125%");
    utest_check(uui_scale_recommended(2560, 0) == 125, "2560 wide, size unknown: 125%");
    utest_check(uui_scale_recommended(1920, 0) == 100, "1920 wide, size unknown: 100%");
    utest_check(uui_scale_recommended(1920, 527) == 100, "a 24\" 1920x1080 monitor: 100%");
    utest_check(uui_scale_recommended(3840, 344) == 200, "a 15.6\" 4K panel: the 200% cap");
    utest_check(uui_scale_recommended(1280, 0) == 100, "a small screen never under 100%");
    utest_check(uui_scale_recommended(0, 0) == 100, "no screen: 100%");
    return utest_end();
}
