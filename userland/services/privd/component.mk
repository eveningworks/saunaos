# services/privd/ -> /bin/privd, the root broker (docs/users-design.md,
# stage B). Every .c here is linked in; privd.c has main(). Fields:
# Makefile, "component manifests".
# The password hash: privd is the one program that links it (pass.c).
EXTRA = third_party/yescrypt/upstream/yescrypt-opt third_party/yescrypt/upstream/yescrypt-common \
        third_party/yescrypt/upstream/sha256 third_party/yescrypt/upstream/insecure_memzero
