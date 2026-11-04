# services/remoted/ -> /bin/remoted. Every .c here is linked in;
# remoted.c has main(). Fields: Makefile, "component manifests".
# VeNCrypt: the remote desktop server's TLS.
ULIBS = $(LIBSSL_SO)
