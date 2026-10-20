#ifndef UUI_DOCDIALOG_H
#define UUI_DOCDIALOG_H
// uui_docdialog -- A MARKDOWN DOCUMENT IN A WINDOW OF ITS OWN: the text
// drawn by uui_markdown and scrolled, with Copy (the Markdown, to the
// clipboard) and Close. For an explanation too long for a uui_dialog's
// few lines -- why a Windows call is only partly there, in Retro
// Launcher -- that the reader may keep open beside what it explains: NOT
// modal, and ONE AT A TIME, so opening another puts the new text in the
// window already open. KDE's KMessageBox::detailedError details, Windows'
// Program Compatibility Assistant's "what is wrong" page.
#include "ui/uapp.h"

// `title` and `markdown` (`len` bytes) are copied.
void uui_docdialog_open(struct uapp *a, const char *title, const char *markdown, int len);
int  uui_docdialog_is_open(void);
void uui_docdialog_close(void);
#endif
