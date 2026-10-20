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

// DdeInitialize(lpidInst, pfnCallback, afCmd, ulRes) -> UINT
// Windows: Registers the program with the DDE management library, which lets programs talk to each
// other: one offers a service, another connects to it and asks for data or sends commands. The
// program's callback hears what arrives.
// w16: Succeeds and gives the program an instance id, so it can go on to name services and make
// string and data handles. Nothing it does reaches another program, and the callback is never
// called.
// Why: There is no other program to talk to: every program runs in its own w16, and there is no DDE
// between them, nor NetDDE between machines.
// Notice: Nothing at this call. Hearts uses DDE for network play: the dealer's game starts, but a
// player finds no dealer to join.
// Whole: DDE between w16 processes, and NetDDE over the network for Hearts. On the roadmap: "w16:
// Hearts over the network".
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

// DdeUninitialize(idInst) -> BOOL
// DdeDisconnect(hConv) -> BOOL
// Windows: DdeUninitialize ends a program's use of DDE, closing its conversations; DdeDisconnect
// ends one conversation.
// w16: Both succeed: there are no conversations to close (DdeInitialize says why).
static int api_DdeTrue4(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w16_arg32(a);
    w->cpu.r[R_AX] = 1;
    return 4;
}

// DdeConnect(idInst, hszService, hszTopic, pCC) -> HCONV
// Windows: Opens a conversation with the program that offers a service and topic, on this machine
// or -- through NetDDE -- on another.
// w16: Finds no one: returns no conversation, and DdeGetLastError says DMLERR_NO_CONV_ESTABLISHED,
// exactly as when nobody offers the service.
// Why: No other program is there to answer (DdeInitialize says why).
// Notice: A Hearts player cannot join a dealer; the program says it could not connect.
// Whole: As DdeInitialize: on the roadmap, "w16: Hearts over the network".
static int api_DdeConnect(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w16_arg32(a); w16_arg32(a); w16_arg32(a); w16_arg32(a);
    g_last_error = DMLERR_NO_CONV_ESTABLISHED;
    set32(w, 0);
    return 16;
}

// DdeClientTransaction(pData, cbData, hConv, hszItem, wFmt, wType, dwTimeout, pdwResult) -> HDDEDATA
// Windows: Sends a request, a poke, an advise or a command over an open conversation, and returns
// the data or the result.
// w16: Fails with DMLERR_NO_CONV_ESTABLISHED: there is never a conversation to send in.
// Why: DdeConnect never opens one (DdeInitialize says why).
// Notice: Nothing on its own: a program only gets here after a connection it never got.
// Whole: As DdeInitialize: on the roadmap, "w16: Hearts over the network".
static int api_DdeClientTransaction(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w16_arg32(a); w16_arg32(a); w16_arg32(a); w16_arg32(a);
    w16_arg16(a); w16_arg16(a); w16_arg32(a); w16_arg32(a);
    g_last_error = DMLERR_NO_CONV_ESTABLISHED;
    set32(w, 0);
    return 28;
}

// DdePostAdvise(idInst, hszTopic, hszItem) -> BOOL
// Windows: Tells every program advised on an item that its data changed, so each asks for it again.
// w16: Succeeds, advising no one: no other program is ever connected.
static int api_DdePostAdvise(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w16_arg32(a); w16_arg32(a); w16_arg32(a);
    w->cpu.r[R_AX] = 1;
    return 12;
}

// DdeCreateDataHandle(idInst, pSrc, cb, cbOff, hszItem, wFmt, afCmd) -> HDDEDATA
// Windows: Copies data into a DDE data handle, to send or to answer a request with.
// w16: Returns a new handle, told apart from the others; its bytes are not kept, as nothing could
// ever read them.
static int api_DdeCreateDataHandle(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w16_arg32(a); w16_arg32(a); w16_arg32(a); w16_arg32(a); w16_arg32(a);
    w16_arg16(a); w16_arg16(a);
    set32(w, g_next_handle++);
    return 24;
}

// DdeGetData(hData, pDst, cbMax, cbOff) -> DWORD
// Windows: Copies the bytes of a DDE data handle -- what another program sent -- into the program's
// memory.
// w16: Copies nothing and answers 0 bytes.
// Why: No data ever arrives from another program (DdeInitialize says why).
// Notice: Nothing: a program asks only for data it was sent.
// Whole: As DdeInitialize: on the roadmap, "w16: Hearts over the network".
static int api_DdeGetData(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w16_arg32(a); w16_arg32(a); w16_arg32(a); w16_arg32(a);
    set32(w, 0);
    return 16;
}

// DdeGetLastError(idInst) -> UINT
// Windows: The error of the last DDE call that failed, cleared as it is read.
// w16: The same: DMLERR_NO_CONV_ESTABLISHED after DdeConnect or DdeClientTransaction, else
// DMLERR_NO_ERROR.
static int api_DdeGetLastError(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w16_arg32(a);
    w->cpu.r[R_AX] = g_last_error;
    g_last_error = DMLERR_NO_ERROR;
    return 4;
}

// DdeCreateStringHandle(idInst, psz, iCodePage) -> HSZ
// Windows: Makes a handle for a string -- a service, topic or item name -- shared by every program
// using DDE.
// w16: Returns a new handle, told apart from the others; the string is not kept, as no other
// program could look it up.
static int api_DdeCreateStringHandle(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w16_arg32(a); w16_arg32(a); w16_arg16(a);
    set32(w, g_next_handle++);
    return 10;
}

// DdeFreeStringHandle(idInst, hsz) -> BOOL
// Windows: Releases a string handle.
// w16: Succeeds.
static int api_DdeFreeStringHandle(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w16_arg32(a); w16_arg32(a);
    w->cpu.r[R_AX] = 1;
    return 8;
}

// DdeNameService(idInst, hsz1, hszRes, afCmd) -> HDDEDATA
// Windows: Announces a service by name, so other programs can connect to it -- a Hearts dealer
// offering its game.
// w16: Succeeds. The name is recorded nowhere, because no other program could look for it.
// Why: No other program is there to look (DdeInitialize says why).
// Notice: A Hearts dealer waits for players who cannot reach it; start the game and the computer
// plays the other hands.
// Whole: As DdeInitialize: on the roadmap, "w16: Hearts over the network".
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
