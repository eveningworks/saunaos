# apps/system/sysupdate/ -> /bin/wm/system/sysupdate. Every .c here is linked in;
# sysupdate.c has main(). Fields: Makefile, "component manifests".
# One update engine behind this window and /bin/update.
EXTRA = update/upd
ULIBS = $(LIBHTTP_SO) $(LIBSSL_SO) $(BUILD)/lib/libhash.so
