# utils/webcontent/ -> /bin/webcontent. Every .c here is linked in;
# webcontent.c has main(). Fields: Makefile, "component manifests".
# NetSurf without monkey's front end: our glue, then the engine.
EXTRA = $(NS_BACKEND_OBJS) $(NS_CORE_OBJS)
# NetSurf's http/https fetcher. GPL-2.0: libssl is taken under mbedTLS's
# GPL-2.0-or-later option (LICENSE). webcontent fetches through webnet,
# but the fetcher's in-process half is linked, and certificates are decoded.
ULIBS = $(LIBHTTP_SO) $(LIBSSL_SO) $(BUILD)/lib/libhash.so
