# apps/apps/terminal/ -> /bin/wm/apps/terminal. Every .c here is linked in;
# terminal.c has main(). Fields: Makefile, "component manifests".
# Installed as uterm, so it never shared a name with the kernel-space
# Terminal while both existed.
INSTALL_AS = uterm
