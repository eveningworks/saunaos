// JavaScript, decided per page -- docs/browser-design.md, stage 6.
//
// NetSurf reads one global option when it creates an HTML content
// (html.c: `enable_scripting = nsoption_bool(enable_javascript)`), so the
// decision is made just before that, for THAT content's own address: a
// wrapper around content_factory_create_content() (--wrap, as the other
// link-time hooks) sets the option from the site's exception, else the
// global setting. A page's iframes are decided by their own sites,
// NoScript's rule.
//
// The exceptions are the APP's, pushed whole (WV_PERMS) -- this process
// reads no file of the browser's -- and the global switch is
// browser.javascript, read at each content so a change in System
// Settings applies to the next page loaded.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dom/dom.h>
#include "utils/errors.h"
#include "utils/nsoption.h"
#include "utils/nsurl.h"
#include "content/content_factory.h"
#include "content/llcache.h"
#include "netsurf/browser_window.h"
#include "netsurf/content.h"
#include "html/html_save.h"
#include "webcontent/wc.h"
#include "lib/usetting.h"

static char *g_perms;   // "host\tallow|block\n" lines, or NULL

void wc_js_set_perms(const char *list, size_t len) {
    free(g_perms);
    g_perms = malloc(len + 1);
    if (!g_perms) return;
    memcpy(g_perms, list, len);
    g_perms[len] = '\0';
}

// The exception for `host`: 1 allow, 0 block, -1 none.
static int exception(const char *host) {
    size_t n = strlen(host);
    for (const char *p = g_perms; p && *p;) {
        const char *tab = strchr(p, '\t'), *nl = strchr(p, '\n');
        if (!nl) nl = p + strlen(p);
        if (tab && tab < nl && (size_t)(tab - p) == n && !strncmp(p, host, n))
            return !strncmp(tab + 1, "allow", 5);
        p = *nl ? nl + 1 : nl;
    }
    return -1;
}

static int global_on(void) {
    char v[8];
    return usetting_get("browser.javascript", v, sizeof v) < 0 || strcmp(v, "off") != 0;
}

int wc_js_allowed(const char *url) {
    char host[256] = "";
    struct nsurl *u = NULL;
    if (url && nsurl_create(url, &u) == NSERROR_OK) {
        lwc_string *h = nsurl_get_component(u, NSURL_HOST);
        if (h) {
            snprintf(host, sizeof host, "%.*s", (int)lwc_string_length(h), lwc_string_data(h));
            lwc_string_unref(h);
        }
        nsurl_unref(u);
    }
    int e = host[0] ? exception(host) : -1;
    return e >= 0 ? e : global_on();
}

struct content *__real_content_factory_create_content(struct llcache_handle *llcache,
        const char *fallback_charset, bool quirks, lwc_string *effective_type);
struct content *__wrap_content_factory_create_content(struct llcache_handle *llcache,
        const char *fallback_charset, bool quirks, lwc_string *effective_type) {
    nsurl *u = llcache_handle_get_url(llcache);
    nsoption_set_bool(enable_javascript, wc_js_allowed(u ? nsurl_access(u) : NULL));
    return __real_content_factory_create_content(llcache, fallback_charset, quirks,
                                                 effective_type);
}

// The page's <script> elements: what a page whose scripts were blocked
// would have run. 0 for anything but an HTML page.
int wc_js_count_scripts(struct browser_window *bw) {
    struct hlcache_handle *h = bw ? browser_window_get_content(bw) : NULL;
    if (!h || content_get_type(h) != CONTENT_HTML) return 0;
    dom_document *doc = html_get_document(h);
    dom_string *tag = NULL;
    dom_nodelist *list = NULL;
    uint32_t n = 0;
    if (!doc || dom_string_create((const uint8_t *)"script", 6, &tag) != DOM_NO_ERR) return 0;
    if (dom_document_get_elements_by_tag_name(doc, tag, &list) == DOM_NO_ERR && list) {
        dom_nodelist_get_length(list, &n);
        dom_nodelist_unref(list);
    }
    dom_string_unref(tag);
    return (int)n;
}
