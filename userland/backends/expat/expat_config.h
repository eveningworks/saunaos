// expat_config.h -- what expat's configure would have written for SaunaOS.
// Expat itself is vendored at userland/ports/expat/ (tools/netsurf_vendor.py),
// for libdom's XML binding under libsvgtiny.
//
// THE HASH SALT COMES FROM getentropy(), over SYS_GETRANDOM: without a
// source, expat salts its hash tables from the clock and the pid, and a
// hostile document can then flood one bucket (CVE-2012-0876).
#ifndef EXPAT_CONFIG_H
#define EXPAT_CONFIG_H 1

#define BYTEORDER 1234
#define HAVE_GETENTROPY 1
#define HAVE_FCNTL_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_STRINGS_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_UNISTD_H 1
#define STDC_HEADERS 1

#define PACKAGE "expat"
#define PACKAGE_NAME "expat"
#define PACKAGE_TARNAME "expat"
#define PACKAGE_URL ""

#define XML_CONTEXT_BYTES 1024
#define XML_DTD 1
#define XML_GE 1
#define XML_NS 1

#endif
