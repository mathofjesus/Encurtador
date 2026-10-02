/* HTTP/1.1 request and response types shared by the parser and the reactor.
 *
 * Every field here is a fixed-size array because these are the buffers that
 * untrusted bytes land in. The sizes are enforced by the parser: anything
 * larger is rejected rather than truncated, because a truncated path is a
 * request for the wrong resource.
 */
#ifndef HTTP_HTTP_H
#define HTTP_HTTP_H

#include <stddef.h>

#include "util/buf.h"

/* Caps chosen so one connection cannot exhaust the 1 GB this box shares with
 * Postgres and Redis. HTTP_MAX_BODY is the most important: it is the amount of
 * memory a single client can ask for by announcing a Content-Length. */
#define HTTP_MAX_PATH 2048
#define HTTP_MAX_BODY 8192
#define HTTP_MAX_METHOD 8
#define HTTP_MAX_HEADERS 32

typedef enum {
    HTTP_PARSE_NEED_MORE = 0, /* header or body still arriving */
    HTTP_PARSE_COMPLETE = 1,  /* a whole request is available */
    HTTP_PARSE_MALFORMED = -1 /* refuse: bad request line, or over a cap */
} http_parse_result_t;

typedef struct {
    char name[64];
    char value[512];
} http_header_t;

typedef struct {
    char method[HTTP_MAX_METHOD];
    char path[HTTP_MAX_PATH];

    http_header_t headers[HTTP_MAX_HEADERS];
    int header_count;

    /* One byte more than the cap: the parser NUL-terminates the body, and a
     * request declaring exactly HTTP_MAX_BODY would otherwise write one byte
     * past the end. The extra byte is for the terminator, not for content. */
    char body[HTTP_MAX_BODY + 1];
    size_t body_len;
    size_t content_length;
    int has_content_length;

    int keep_alive;
    int complete;

    /* Bytes consumed by the request that just completed. The reactor uses this
     * to find the start of the next request when a client pipelines them. */
    size_t consumed;
} http_request_t;

/* Prepares a request for parsing. Must be called before http_parse. */
void http_request_init(http_request_t *r);

/* Releases anything the request owns and returns it to the init state. */
void http_request_reset(http_request_t *r);

/* Serialises a response into out.
 *
 * location is written as a Location header and ignored when NULL; body may be
 * NULL for a zero-length response.
 *
 * extra_headers is NULL or one or more complete CRLF-terminated header lines,
 * appended after the standard ones. It exists for headers whose presence
 * depends on the outcome, such as Retry-After on a 429. It is built by this
 * program from values it controls and is never copied from the request.
 *
 * head_only declares the true Content-Length but writes no body, which is what
 * a response to HEAD must do: the client learns the length it would have
 * received without the bytes being sent. */
void http_response_write(buf_t *out, int status, const char *content_type,
                         const char *body, const char *location,
                         const char *extra_headers, int keep_alive,
                         int head_only);

/* Reason phrase for a status code. Never returns NULL: an unknown code yields
 * "Unknown", because this string goes straight into the status line. */
const char *http_status_text(int status);

#endif /* HTTP_HTTP_H */
