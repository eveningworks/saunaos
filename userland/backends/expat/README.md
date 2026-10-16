# expat's toy-os side -- OURS, not vendored

`userland/ports/expat/` is upstream's, at its release tag
(`tools/netsurf_vendor.py`); it is the XML parser under libdom's binding,
for libsvgtiny.

| File | Why it exists |
|---|---|
| `expat_config.h` | What expat's configure would have written for toy-os: little-endian, the hash salt from `getentropy()`, DTD and namespaces on. |
