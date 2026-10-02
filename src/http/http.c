#include "http/http.h"

#include <stdlib.h>
#include <string.h>

void http_request_init(http_request_t *r)
{
    if (!r)
        return;
    memset(r, 0, sizeof(*r));
    r->keep_alive = 1;
    r->complete = 0;
}

void http_request_reset(http_request_t *r)
{
    if (r)
        http_request_init(r);
}

const char *http_status_text(int status)
{
    switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 410: return "Gone";
    case 413: return "Payload Too Large";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default:  return "Unknown";
    }
}

void http_response_write(buf_t *out, int status, const char *content_type,
                         const char *body, const char *location,
                         const char *extra_headers, int keep_alive,
                         int head_only)
{
    if (!out)
        return;

    const char *body_text = body ? body : "";
    size_t body_len = body ? strlen(body) : 0;

    buf_appendf(out, "HTTP/1.1 %d %s\r\n", status, http_status_text(status));

    if (location)
        buf_appendf(out, "Location: %s\r\n", location);

    /* Content-Type is omitted when absent rather than sent as an empty value,
     * which some clients treat as a malformed header. */
    if (content_type)
        buf_appendf(out, "Content-Type: %s\r\n", content_type);

    if (extra_headers)
        buf_append_str(out, extra_headers);

    /* Always declared, even when zero: without it a client cannot tell an empty
     * 301 from a truncated one and will wait. */
    buf_appendf(out, "Content-Length: %zu\r\n", body_len);

    buf_appendf(out, "Connection: %s\r\n\r\n", keep_alive ? "keep-alive" : "close");

    /* The length above is the length the equivalent GET would return; HEAD
     * simply stops before sending it. */
    if (body_len > 0 && !head_only)
        buf_append(out, body_text, body_len);

    /* buf_append does not write a terminator. The reactor writes by length, but
     * a caller reading the buffer as a string (the tests do) must not walk off
     * the end. One spare byte is always reserved for exactly this. */
    buf_terminate(out);
}
