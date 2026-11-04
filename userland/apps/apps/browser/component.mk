# apps/apps/browser/ -> /bin/wm/apps/browser. Every .c here is linked in;
# browser.c has main(). Fields: Makefile, "component manifests".
# MIT, but every page is drawn by /bin/webcontent (NetSurf, GPL-2.0), so
# a NOGPL=1 build leaves it out.
NEEDS_PORT = netsurf
