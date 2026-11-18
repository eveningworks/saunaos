#ifndef ULIB_URACE_H
#define ULIB_URACE_H
// A RACE AMPLIFIER FOR GUI TESTS: `desktop.test_race_delay` milliseconds
// slept wherever background work hands its result over -- a worker's
// uapp_post(), a uworker job's finish, w16's program taking a message.
// A test that races such work then fails every time instead of once in
// six runs (Go's race detector and rr's chaos mode in spirit: make the
// rare interleaving the common one). 0, the default, is no delay.
//
// Read once per process, at its first use: a program started before the
// setting changed keeps the value it found. Callable from any thread.

// The delay in milliseconds, 0 when off.
int urace_delay_ms(void);
// Sleep it, if any, at most once a second per process. Call it ON the
// background thread, never the loop.
void urace_delay(void);

#endif // ULIB_URACE_H
