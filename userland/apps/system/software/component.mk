# apps/system/software/ -> /bin/wm/system/software. Every .c here is linked in;
# software.c has main(). Fields: Makefile, "component manifests".
# Removing a component is libbase's (base/ucomponent.h); adding one back
# fetches at this machine's build, so it is the update engine's too.
EXTRA = common/upd
ULIBS = $(LIBHTTP_SO) $(LIBSSL_SO) $(BUILD)/lib/libhash.so
