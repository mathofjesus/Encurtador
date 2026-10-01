#include "util/buf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Keeps the struct itself separate from the bytes so buf_detach can hand back
 * a plain malloc'd string without the caller caring about our bookkeeping. */
typedef struct {
    buf_t pub;
    size_t alloc; /* bytes owned by the malloc block backing data */
} buf_impl_t;

static int buf_reserve(buf_impl_t *bi, size_t extra)
{
    if (extra > (size_t)-1 - bi->pub.len)
        return -1; /* len + extra would wrap; refuse rather than corrupt */

    size_t needed = bi->pub.len + extra;
    if (needed <= bi->alloc)
        return 0;

    size_t want = bi->alloc ? bi->alloc : 64;
    while (want < needed) {
        if (want > (size_t)-1 / 2)
            return -1;
        want *= 2;
    }

    /* One extra byte so buf_terminate can NUL-terminate without counting it. */
    char *p = realloc(bi->pub.data, want + 1);
    if (!p)
        return -1; /* buffer left untouched and still valid */

    bi->pub.data = p;
    bi->alloc = want;
    return 0;
}

buf_t *buf_new(size_t cap)
{
    /* cap must leave room for the terminator byte without wrapping. */
    if (cap > (size_t)-1 - 1)
        return NULL;

    buf_impl_t *bi = calloc(1, sizeof(*bi));
    if (!bi)
        return NULL;

    if (cap > 0) {
        char *p = malloc(cap + 1);
        if (!p) {
            free(bi);
            return NULL;
        }
        p[0] = '\0';
        bi->pub.data = p;
        bi->alloc = cap;
    }

    bi->pub.len = 0;
    return &bi->pub;
}

void buf_free(buf_t *b)
{
    buf_impl_t *bi = (buf_impl_t *)b;
    if (!bi)
        return;
    free(bi->pub.data);
    free(bi);
}

int buf_append(buf_t *b, const void *data, size_t len)
{
    if (!b || (!data && len > 0))
        return -1;
    if (len == 0)
        return 0;

    buf_impl_t *bi = (buf_impl_t *)b;
    if (buf_reserve(bi, len) != 0)
        return -1;

    memcpy(b->data + b->len, data, len);
    b->len += len;
    return 0;
}

int buf_append_str(buf_t *b, const char *s)
{
    if (!s)
        return -1;
    return buf_append(b, s, strlen(s));
}

int buf_appendf(buf_t *b, const char *fmt, ...)
{
    if (!b || !fmt)
        return -1;

    buf_impl_t *bi = (buf_impl_t *)b;

    /* Try the existing space first: vsnprintf reports what it *would* have
     * written, so one call is usually enough and avoids a second copy. */
    va_list ap;
    va_start(ap, fmt);
    size_t avail = bi->alloc > bi->pub.len ? bi->alloc - bi->pub.len : 0;
    int need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);

    if (need < 0)
        return -1;

    size_t total = bi->pub.len + (size_t)need;
    if (buf_reserve(bi, (size_t)need) != 0)
        return -1;

    /* Recompute after the reserve: using the pre-grow figure would let vsnprintf
     * truncate to the old capacity while len claimed the full length. */
    avail = buf_avail(b);

    size_t before = b->len;
    va_start(ap, fmt);
    int written = vsnprintf(b->data + before, avail + 1, fmt, ap);
    va_end(ap);

    if (written < 0) {
        b->len = before; /* nothing usable was produced */
        return -1;
    }

    b->len = total;
    return written;
}

size_t buf_len(const buf_t *b)
{
    return b ? b->len : 0;
}

size_t buf_avail(const buf_t *b)
{
    if (!b)
        return 0;
    const buf_impl_t *bi = (const buf_impl_t *)b;
    return bi->alloc > b->len ? bi->alloc - b->len : 0;
}

char *buf_detach(buf_t *b)
{
    if (!b)
        return NULL;

    if (b->len == 0) {
        /* An empty buffer has nothing to hand back. The caller gets NULL rather
         * than an empty string it might mistake for content. The struct stays
         * alive and still needs buf_free. */
        return NULL;
    }

    buf_terminate(b);
    char *out = b->data;

    /* Only the bytes change hands. Freeing the struct here would make
     * buf_detach() followed by buf_free() a double free, which is the obvious
     * thing for a caller to write. */
    b->data = NULL;
    b->len = 0;
    ((buf_impl_t *)b)->alloc = 0;
    return out;
}

void buf_reset(buf_t *b)
{
    if (b)
        b->len = 0;
}

void buf_terminate(buf_t *b)
{
    buf_impl_t *bi = (buf_impl_t *)b;
    if (!b)
        return;
    /* buf_reserve always keeps one spare byte, so this cannot fail. */
    if (bi->alloc >= b->len)
        b->data[b->len] = '\0';
}

void buf_new_into(buf_t **out, size_t cap)
{
    if (!out)
        return;
    *out = buf_new(cap);
}
