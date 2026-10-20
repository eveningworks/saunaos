// ddeml16.c -- DDEML, Windows' DDE management library, as far as a
// program needs it when NO OTHER PROGRAM IS THERE: Hearts uses it for
// network play (its dealer offers a service, its players connect through
// NetDDE). It initialises, names its service and makes its strings and
// data; no conversation ever connects, so a player finds no dealer and
// the dealer plays the computer's hands. Every call is a stand-in, listed
// as such (task16.c's PARTLY).
#include "w16_internal.h"

#include <string.h>

#define DMLERR_NO_ERROR            0
#define DMLERR_NO_CONV_ESTABLISHED 0x400A

static uint32_t g_next_handle = 1;               // string and data handles: only told apart, never read
static uint16_t g_last_error = DMLERR_NO_ERROR;

static void set32(struct w16 *w, uint32_t v) {
    w->cpu.r[R_AX] = (uint16_t)v;
    w->cpu.r[R_DX] = (uint16_t)(v >> 16);
}

// DdeInitialize(lpidInst, pfnCallback, afCmd, ulRes) -> DMLERR_*: an instance id
static int api_DdeInitialize(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint32_t pid = w16_arg32(a);
    w16_arg32(a); w16_arg32(a); w16_arg32(a);
    uint8_t *p = cpu16_ptr(&w->cpu, (uint16_t)(pid >> 16), (uint16_t)pid, 4, 1);
    if (!p) return -1;
    uint32_t id = 1;
    memcpy(p, &id, 4);
    w->cpu.r[R_AX] = DMLERR_NO_ERROR;
    return 16;
}

// DdeUninitialize(idInst), DdeDisconnect(hConv) -> TRUE
static int api_DdeTrue4(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w16_arg32(a);
    w->cpu.r[R_AX] = 1;
    return 4;
}

// DdeConnect(idInst, hszService, hszTopic, pCC) -> no conversation: nobody offers it
static int api_DdeConnect(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w16_arg32(a); w16_arg32(a); w16_arg32(a); w16_arg32(a);
    g_last_error = DMLERR_NO_CONV_ESTABLISHED;
    set32(w, 0);
    return 16;
}

// DdeClientTransaction(pData, cbData, hConv, hszItem, wFmt, wType, dwTimeout, pdwResult) -> 0: no conversation
static int api_DdeClientTransaction(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w16_arg32(a); w16_arg32(a); w16_arg32(a); w16_arg32(a);
    w16_arg16(a); w16_arg16(a); w16_arg32(a); w16_arg32(a);
    g_last_error = DMLERR_NO_CONV_ESTABLISHED;
    set32(w, 0);
    return 28;
}

// DdePostAdvise(idInst, hszTopic, hszItem) -> TRUE: no one is advised
static int api_DdePostAdvise(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w16_arg32(a); w16_arg32(a); w16_arg32(a);
    w->cpu.r[R_AX] = 1;
    return 12;
}

// DdeCreateDataHandle(idInst, pSrc, cb, cbOff, hszItem, wFmt, afCmd) -> a handle
static int api_DdeCreateDataHandle(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w16_arg32(a); w16_arg32(a); w16_arg32(a); w16_arg32(a); w16_arg32(a);
    w16_arg16(a); w16_arg16(a);
    set32(w, g_next_handle++);
    return 24;
}

// DdeGetData(hData, pDst, cbMax, cbOff) -> 0 bytes: no data ever arrives
static int api_DdeGetData(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w16_arg32(a); w16_arg32(a); w16_arg32(a); w16_arg32(a);
    set32(w, 0);
    return 16;
}

// DdeGetLastError(idInst)
static int api_DdeGetLastError(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w16_arg32(a);
    w->cpu.r[R_AX] = g_last_error;
    g_last_error = DMLERR_NO_ERROR;
    return 4;
}

// DdeCreateStringHandle(idInst, psz, iCodePage) -> a handle
static int api_DdeCreateStringHandle(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w16_arg32(a); w16_arg32(a); w16_arg16(a);
    set32(w, g_next_handle++);
    return 10;
}

// DdeFreeStringHandle(idInst, hsz) -> TRUE
static int api_DdeFreeStringHandle(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w16_arg32(a); w16_arg32(a);
    w->cpu.r[R_AX] = 1;
    return 8;
}

// DdeNameService(idInst, hsz1, hszRes, afCmd) -> nonzero: registered, to no one
static int api_DdeNameService(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w16_arg32(a); w16_arg32(a); w16_arg32(a); w16_arg16(a);
    set32(w, 1);
    return 14;
}

const struct w16_api g_ddeml_api[] = {
    { 2, api_DdeInitialize },
    { 3, api_DdeTrue4 },
    { 7, api_DdeConnect },
    { 8, api_DdeTrue4 },
    { 11, api_DdeClientTransaction },
    { 13, api_DdePostAdvise },
    { 14, api_DdeCreateDataHandle },
    { 16, api_DdeGetData },
    { 20, api_DdeGetLastError },
    { 21, api_DdeCreateStringHandle },
    { 22, api_DdeFreeStringHandle },
    { 27, api_DdeNameService },
};
const unsigned g_ddeml_api_n = sizeof g_ddeml_api / sizeof g_ddeml_api[0];
