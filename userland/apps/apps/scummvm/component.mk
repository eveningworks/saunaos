# apps/apps/scummvm/ -> /bin/wm/apps/scummvm: ScummVM's front, which
# downloads the free games (ui/uui_datacard.h) and hands over to
# /usr/bin/scummvm (make scummvm). Seeded only with ScummVM; hence libhttp.
ULIBS = $(LIBHTTP_SO) $(LIBSSL_SO) $(BUILD)/lib/libhash.so
