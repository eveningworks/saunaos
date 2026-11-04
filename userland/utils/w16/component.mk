# utils/w16/ -> /bin/w16. Every .c here is linked in;
# w16.c has main(). Fields: Makefile, "component manifests".
# The settings w16 runs by, shared with Retro Launcher, Install Windows 3.x
# and hlpbroker. missing16.c holds no code (tools/gen/gen_w16_calls.py
# reads its comments), so linking it adds nothing.
EXTRA = common/w16prefs
