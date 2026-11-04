# services/smbfsd/ -> /bin/smbfsd. Every .c here is linked in;
# smbfsd.c has main(). Fields: Makefile, "component manifests".
# The SMB client library; its crypto is in libssl.
ULIBS = $(LIBSMB_SO) $(LIBSSL_SO)
