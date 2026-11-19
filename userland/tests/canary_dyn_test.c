// canary_dyn_test -- canary_probe.h in a DYNAMIC program, where ld-toy
// seeds the canary and lays out the TCB. Run by canary_test, which
// compares its line with the static program's; alone it prints the line.
#include "canary_probe.h"

int main(void) { return probe_print(); }
