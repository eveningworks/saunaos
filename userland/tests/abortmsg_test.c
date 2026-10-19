// Dies of abort() after set_abort_message(), for tools/faulttest_run.py:
// the kernel's crash report must carry the message as `message:` lines,
// and logs its first line once the report is written. The \x01 must
// arrive as a space -- a control byte from a dying program's memory is
// cleaned before it reaches the report.
#include <stdlib.h>

int main(void) {
    set_abort_message("abortmsg_test says\x01goodbye\nline two\nline three");
    abort();
}
