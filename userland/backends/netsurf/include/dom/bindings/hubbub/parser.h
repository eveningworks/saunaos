// libdom installs its HTML binding's headers under dom/bindings/hubbub/,
// which is where NetSurf includes them from; the vendored tree keeps
// them at bindings/hubbub/. This forwards one to the other rather than
// editing either.
#include "../../../../../../ports/libdom/bindings/hubbub/parser.h"
