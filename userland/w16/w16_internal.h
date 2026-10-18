// w16_internal.h -- what w16's parts share (docs/w16-design.md). w16 is
// split by concern, as userland/wm/ is: ne.c reads the file, ne_info.c
// prints it; the CPU and the Win16 API join in later stages.
#ifndef W16_INTERNAL_H
#define W16_INTERNAL_H

#include "ne.h"
#include "w16_names.h"

void ne_print_info(const struct ne_module *m);

#endif
