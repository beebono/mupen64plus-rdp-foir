#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include "foir.h"

static void (*g_cb)(void *ctx, int level, const char *msg);
static void *g_ctx;

void foir_log_set_callback(void (*cb)(void *, int, const char *), void *ctx)
{
    g_cb = cb;
    g_ctx = ctx;
}

void foir_log(int level, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    static FILE *lf;
    static int lf_tried;
    if (!lf_tried) {
        const char *p = getenv("FOIR_LOGFILE");
        if (p && *p) lf = fopen(p, "a");
        lf_tried = 1;
    }
    if (lf) { fprintf(lf, "%s\n", buf); fflush(lf); }
    if (g_cb) g_cb(g_ctx, level, buf);
    else fprintf(stderr, "foir: %s\n", buf);
}
