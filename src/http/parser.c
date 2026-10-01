#include "http/parser.h"

#include <stdlib.h>
#include <string.h>

/* Everything the parser needs to resume, derived from the accumulated buffer.
 * Recomputed on each call rather than stored, so there is no state that can
 * drift out of sync with the bytes. */
typedef struct {
    const char *data;
    size_t len;
    size_t pos;
} scan_t;

static int find_crlf(const char *data, size_t len, size_t from, size_t *end)
{
    for (size_t i = from; i + 1 < len; i++) {
        if (data[i] == '\r' && data[i + 1] == '\n') {
            *end = i;
            return 1;
        }
    }
    return 0;
}

static int line_equals(const char *line, size_t line_len, const char *lit)
{
    size_t n = strlen(lit);
    return line_len == n && strncasecmp(line, lit, n) == 0;
}

/* Reads a decimal number with no sign and no leading space. Returns 0 on
 * success. Rejects empty, non-numeric, and anything that would overflow, which
 * is what stops "Content-Length: -1" or a value near LLONG_MAX. */
static int parse_decimal(const char *s, size_t len, long *out)
{
    if (len == 0)
        return -1;

    long value = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9')
            return -1;
        int d = s[i] - '0';
        if (value > (2147483647L - d) / 10)
            return -1;
        value = value * 10 + d;
    }
    *out = value;
    return 0;
}

static void trim(const char **start, size_t *len)
{
    while (*len > 0 && (**start == ' ' || **start == '\t')) {
        (*start)++;
        (*len)--;
    }
    while (*len > 0) {
        char c = (*start)[*len - 1];
        if (c != ' ' && c != '\t')
            break;
        (*len)--;
    }
}

/* Copies a bounded slice into a fixed array, refusing to overflow it.
 * Truncating here would silently change the meaning of a path or header. */
static int copy_bounded(char *dst, size_t dst_size, const char *src, size_t len)
{
    if (len >= dst_size)
        return -1;
    memcpy(dst, src, len);
    dst[len] = '\0';
    return 0;
}

static int parse_request_line(http_request_t *r, const char *line, size_t len)
{
    const char *method = line;
    const char *method_end = memchr(line, ' ', len);
    if (!method_end)
        return -1;

    size_t method_len = (size_t)(method_end - line);
    if (method_len == 0 || method_len >= HTTP_MAX_METHOD)
        return -1;

    /* Only the verbs this service routes. Matching the whole method rather
     * than its first letter is what makes this a whitelist instead of a
     * prefix test, so an invented verb like "GEX" is refused. */
    static const char *const allowed[] = {"GET", "POST", "HEAD", "DELETE"};
    int recognised = 0;
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
        if (line_equals(method, method_len, allowed[i])) {
            recognised = 1;
            break;
        }
    }
    if (!recognised)
        return -1;

    if (copy_bounded(r->method, sizeof(r->method), method, method_len) != 0)
        return -1;

    const char *path = method_end + 1;
    const char *path_end = memchr(path, ' ', (size_t)(line + len - path));
    if (!path_end)
        return -1;

    size_t path_len = (size_t)(path_end - path);
    if (path_len == 0 || path_len >= HTTP_MAX_PATH)
        return -1;
    if (path[0] != '/')
        return -1; /* origin-form only; no absolute-form or authority-form */
    if (copy_bounded(r->path, sizeof(r->path), path, path_len) != 0)
        return -1;

    /* The version is mandatory. A request line without it is not HTTP/1.x. */
    const char *version = path_end + 1;
    size_t version_len = (size_t)(line + len - version);
    trim(&version, &version_len);
    if (!line_equals(version, version_len, "HTTP/1.1") &&
        !line_equals(version, version_len, "HTTP/1.0"))
        return -1;

    /* HTTP/1.1 defaults to persistent; HTTP/1.0 defaults to close. */
    r->keep_alive = line_equals(version, version_len, "HTTP/1.1") ? 1 : 0;
    return 0;
}

static int parse_header_line(http_request_t *r, const char *line, size_t len)
{
    const char *colon = memchr(line, ':', len);
    if (!colon)
        return -1;

    const char *name = line;
    size_t name_len = (size_t)(colon - name);
    trim(&name, &name_len);
    if (name_len == 0)
        return -1;

    const char *value = colon + 1;
    size_t value_len = (size_t)(line + len - value);
    trim(&value, &value_len);

    if (r->header_count >= HTTP_MAX_HEADERS)
        return -1;

    /* Connection: close downgrades keep-alive. Checked here rather than at
     * request time so the reactor sees the final answer. */
    if (line_equals(name, name_len, "Connection")) {
        if (line_equals(value, value_len, "close"))
            r->keep_alive = 0;
        else if (line_equals(value, value_len, "keep-alive"))
            r->keep_alive = 1;
        return 0;
    }

    if (line_equals(name, name_len, "Content-Length")) {
        long declared;
        if (parse_decimal(value, value_len, &declared) != 0)
            return -1;
        if (declared < 0 || (size_t)declared > HTTP_MAX_BODY)
            return -1; /* refuse on the header, before waiting for the body */
        if (r->has_content_length && r->content_length != (size_t)declared)
            return -1; /* two different lengths is a smuggling attempt */
        r->has_content_length = 1;
        r->content_length = (size_t)declared;
        return 0;
    }

    if (line_equals(name, name_len, "Transfer-Encoding")) {
        /* v1 does not implement chunked. Refusing is safer than guessing:
         * mis-parsing a chunked body would desynchronise the connection. */
        return -1;
    }

    http_header_t *h = &r->headers[r->header_count];
    if (copy_bounded(h->name, sizeof(h->name), name, name_len) != 0)
        return -1;
    if (copy_bounded(h->value, sizeof(h->value), value, value_len) != 0)
        return -1;
    r->header_count++;
    return 0;
}

/* Returns the offset just past the blank line ending the header block. */
static int parse_headers(http_request_t *r, const char *data, size_t len, size_t *body_start)
{
    size_t pos = 0;
    int first = 1;

    while (1) {
        size_t line_end;
        if (!find_crlf(data, len, pos, &line_end))
            return HTTP_PARSE_NEED_MORE;

        size_t line_len = line_end - pos;

        /* An empty line closes the header block. */
        if (line_len == 0) {
            *body_start = line_end + 2;
            return HTTP_PARSE_COMPLETE;
        }

        const char *line = data + pos;
        if (first) {
            if (parse_request_line(r, line, line_len) != 0)
                return HTTP_PARSE_MALFORMED;
            first = 0;
        } else {
            if (parse_header_line(r, line, line_len) != 0)
                return HTTP_PARSE_MALFORMED;
        }

        pos = line_end + 2;
    }
}

int http_parse(http_request_t *r, const char *data, size_t len)
{
    if (!r || !data)
        return HTTP_PARSE_MALFORMED;

    size_t body_start;
    int rc = parse_headers(r, data, len, &body_start);
    if (rc != HTTP_PARSE_COMPLETE)
        return rc;

    size_t available = len - body_start;
    if (available < r->content_length)
        return HTTP_PARSE_NEED_MORE;

    /* The body is length-delimited and may contain NUL, so it is copied by
     * count and never treated as a C string. */
    if (r->content_length > 0)
        memcpy(r->body, data + body_start, r->content_length);
    r->body[r->content_length] = '\0';
    r->body_len = r->content_length;

    r->consumed = body_start + r->content_length;
    r->complete = 1;
    return HTTP_PARSE_COMPLETE;
}

const char *http_get_header(const http_request_t *r, const char *name)
{
    if (!r || !name)
        return NULL;
    for (int i = 0; i < r->header_count; i++) {
        if (strcasecmp(r->headers[i].name, name) == 0)
            return r->headers[i].value;
    }
    return NULL;
}
