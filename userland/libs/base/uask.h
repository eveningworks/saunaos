#ifndef ULIB_UASK_H
#define ULIB_UASK_H

// A YES/NO QUESTION on the terminal, apt's and dnf's "[Y/n]": the
// default is the capital letter and is what Enter answers; y, yes, n and
// no in any case answer; anything else asks again.
//
// NOBODY TO ASK IS NOT A "NO": with stdin or stdout not a terminal (a
// script, `remote.py exec` into a file) it returns UASK_NO_TERMINAL and
// prints nothing, so the caller can name the flag that answers instead
// (-y), as dnf does. End of input is a "no".

#ifdef __cplusplus
extern "C" {
#endif

#define UASK_NO_TERMINAL (-1)

// 1 yes, 0 no, UASK_NO_TERMINAL. `dflt` is 1 for [Y/n], 0 for [y/N].
int uask_yes_no(const char *question, int dflt);

#ifdef __cplusplus
}
#endif

#endif
