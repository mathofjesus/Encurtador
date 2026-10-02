#include "observability/log.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "util/buf.h"

struct log {
    int fd;
    log_level_t min_level;
};

static const char *const LEVEL_NAMES[LOG_LEVEL_COUNT] = {
    "debug", "info", "warn", "error",
};

log_t *log_new(int fd, log_level_t min_level)
{
    log_t *l = calloc(1, sizeof(*l));
    if (!l)
        return NULL;

    l->fd = fd;
    /* A level outside the enum would suppress everything or nothing by
     * accident; clamp it to the most verbose setting so a bad argument logs. */
    l->min_level = (min_level >= 0 && min_level < LOG_LEVEL_COUNT) ? min_level
                                                                   : LOG_DEBUG;
    return l;
}

void log_free(log_t *l)
{
    free(l);
}

void log_set_level(log_t *l, log_level_t min_level)
{
    if (l && min_level >= 0 && min_level < LOG_LEVEL_COUNT)
        l->min_level = min_level;
}

const char *log_level_name(log_level_t level)
{
    if (level < 0 || level >= LOG_LEVEL_COUNT)
        return "unknown";
    return LEVEL_NAMES[level];
}

log_level_t log_level_from_string(const char *s)
{
    if (!s)
        return LOG_INFO;

    for (int i = 0; i < LOG_LEVEL_COUNT; i++) {
        if (strcasecmp(s, LEVEL_NAMES[i]) == 0)
            return (log_level_t)i;
    }
    /* Unknown level names are a config typo, not a reason to emit nothing. INFO
     * is the level a typo was almost certainly trying to set. */
    return LOG_INFO;
}

/* Writes a JSON string, escaping the two structural characters and every control
 * byte. A path or client address comes from the request; without this a quote in
 * it would let a client forge additional fields in the record. */
static void append_json_string(buf_t *b, const char *s)
{
    buf_append(b, "\"", 1);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"':
            buf_append(b, "\\\"", 2);
            break;
        case '\\':
            buf_append(b, "\\\\", 2);
            break;
        case '\n':
            buf_append(b, "\\n", 2);
            break;
        case '\r':
            buf_append(b, "\\r", 2);
            break;
        case '\t':
            buf_append(b, "\\t", 2);
            break;
        default:
            if (*p < 0x20)
                buf_appendf(b, "\\u%04x", *p);
            else
                buf_append(b, p, 1);
        }
    }
    buf_append(b, "\"", 1);
}

static void append_field(buf_t *b, const char *key, const char *value)
{
    if (!value)
        return;
    buf_appendf(b, ",\"%s\":", key);
    append_json_string(b, value);
}

void log_write(log_t *l, const log_record_t *rec)
{
    if (!l || !rec || !rec->event)
        return;
    if (rec->level < l->min_level)
        return;

    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return;

    long long ms = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;

    buf_t *b = buf_new(256);
    if (!b)
        return;

    buf_appendf(b, "{\"ts\":%lld,\"level\":", ms);
    append_json_string(b, log_level_name(rec->level));
    buf_append_str(b, ",\"event\":");
    append_json_string(b, rec->event);

    append_field(b, "method", rec->method);
    append_field(b, "path", rec->path);
    if (rec->status > 0)
        buf_appendf(b, ",\"status\":%d", rec->status);
    if (rec->duration_us >= 0)
        buf_appendf(b, ",\"duration_us\":%lld", rec->duration_us);
    append_field(b, "client", rec->client);
    append_field(b, "error", rec->error);

    buf_append(b, "}\n", 2);

    size_t off = 0;
    while (off < b->len) {
        ssize_t n = write(l->fd, b->data + off, b->len - off);
        if (n <= 0) {
            if (n < 0 && errno == EINTR)
                continue;
            break; /* a dropped line beats a stalled request */
        }
        off += (size_t)n;
    }

    buf_free(b);
}
