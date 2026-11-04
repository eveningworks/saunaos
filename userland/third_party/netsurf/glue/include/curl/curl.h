// NOT libcurl, which is not vendored. Upstream's content/fetch.c includes
// content/fetchers/curl.h whether or not curl is built (its call to
// fetch_curl_register() is inside #ifdef WITH_CURL; the #include is
// not), and that header declares `extern CURLM *fetch_curl_multi`. The
// type's NAME is all it needs; nothing here defines or uses one.
#ifndef NS_SAUNAOS_CURL_H
#define NS_SAUNAOS_CURL_H
typedef struct ns_saunaos_no_curl CURLM;
#endif
