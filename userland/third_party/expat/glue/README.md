# expat's SaunaOS side -- OURS, not vendored

`userland/third_party/expat/upstream/` is upstream's, at its release tag
(`tools/fetch/netsurf_vendor.py`); it is the XML parser under libdom's binding,
for libsvgtiny.

| File | Why it exists |
|---|---|
| `expat_config.h` | What expat's configure would have written for SaunaOS: little-endian, the hash salt from `getentropy()`, DTD and namespaces on. |
