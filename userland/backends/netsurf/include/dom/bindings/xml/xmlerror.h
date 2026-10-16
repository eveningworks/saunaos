// libdom installs its XML binding's headers under dom/bindings/xml/,
// which is where libsvgtiny includes them from; the vendored tree keeps
// them at bindings/xml/. This forwards one to the other rather than
// editing either.
#include "../../../../../../ports/libdom/bindings/xml/xmlerror.h"
