# services/init/ -> /bin/init. Every .c here is linked in;
# init.c has main(). Fields: Makefile, "component manifests".
# Linked STATIC by a rule of its own (the Makefile's "THREE EXCEPTIONS"):
# pid 1 must start with /lib broken. That rule links no parts, so init
# has none.
