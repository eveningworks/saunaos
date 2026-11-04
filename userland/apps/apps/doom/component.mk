# apps/apps/doom/ -> /bin/wm/apps/doom. Every .c here is linked in;
# doom.c has main(). Fields: Makefile, "component manifests".
# The game itself: our platform layer, then doomgeneric (both vendored
# in userland/third_party/doom/). It downloads its game data, hence libhttp.
EXTRA = $(DOOM_BACKEND_OBJS) $(DOOM_PORT_OBJS)
ULIBS = $(LIBHTTP_SO) $(LIBSSL_SO) $(BUILD)/lib/libhash.so
NEEDS_PORT = doom
