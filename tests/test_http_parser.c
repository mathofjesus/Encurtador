#include <criterion/criterion.h>
#include <stdlib.h>
#include <string.h>

#include "http/http.h"
#include "http/parser.h"
#include "util/buf.h"

Test(http, parse_simple_get)
{
    http_request_t r;
    http_request_init(&r);
    const char *raw = "GET /abc123 HTTP/1.1\r\nHost: x\r\n\r\n";

    cr_assert_eq(http_parse(&r, raw, strlen(raw)), HTTP_PARSE_COMPLETE);
    cr_assert_str_eq(r.method, "GET");
    cr_assert_str_eq(r.path, "/abc123");
    cr_assert_eq(r.keep_alive, 1);
    http_request_reset(&r);
}

Test(http, parse_post_with_body)
{
    http_request_t r;
    http_request_init(&r);
    /* Content-Length is 9: the body is 9 bytes, not 8. Declaring the wrong
     * length is not a parser concern, so the test must state it correctly. */
    const char *raw = "POST /api/v1/shorten HTTP/1.1\r\nHost: x\r\n"
                      "Content-Length: 9\r\n\r\n{\"url\":1}";

    cr_assert_eq(http_parse(&r, raw, strlen(raw)), HTTP_PARSE_COMPLETE);
    cr_assert_str_eq(r.method, "POST");
    cr_assert_str_eq(r.path, "/api/v1/shorten");
    cr_assert_eq(r.body_len, 9);
    cr_assert_str_eq(r.body, "{\"url\":1}");
    http_request_reset(&r);
}

Test(http, incomplete_request_returns_need_more)
{
    http_request_t r;
    http_request_init(&r);
    const char *raw = "GET /abc HT";

    cr_assert_eq(http_parse(&r, raw, strlen(raw)), HTTP_PARSE_NEED_MORE);
    http_request_reset(&r);
}

Test(http, body_shorter_than_content_length_returns_need_more)
{
    http_request_t r;
    http_request_init(&r);
    const char *raw = "POST / HTTP/1.1\r\nContent-Length: 100\r\n\r\nshort";

    cr_assert_eq(http_parse(&r, raw, strlen(raw)), HTTP_PARSE_NEED_MORE);
    http_request_reset(&r);
}

Test(http, rejects_content_length_beyond_the_cap)
{
    /* The body cap is what stops a single request from exhausting the 1 GB of
     * memory this box has to share with Postgres and Redis. */
    http_request_t r;
    http_request_init(&r);
    const char *raw = "POST / HTTP/1.1\r\nContent-Length: 999999\r\n\r\n";

    cr_assert_eq(http_parse(&r, raw, strlen(raw)), HTTP_PARSE_MALFORMED);
    http_request_reset(&r);
}

Test(http, accepts_content_length_exactly_at_the_cap)
{
    http_request_t r;
    http_request_init(&r);

    buf_t *head = buf_new(64);
    buf_appendf(head, "POST /api/v1/shorten HTTP/1.1\r\nContent-Length: %d\r\n\r\n",
                HTTP_MAX_BODY);

    buf_t *whole = buf_new(64);
    buf_append(whole, head->data, head->len);
    for (int i = 0; i < HTTP_MAX_BODY; i++)
        buf_append(whole, "a", 1);

    cr_assert_eq(http_parse(&r, (const char *)whole->data, whole->len),
                 HTTP_PARSE_COMPLETE);
    cr_assert_eq(r.body_len, HTTP_MAX_BODY);

    buf_free(whole);
    buf_free(head);
    http_request_reset(&r);
}

Test(http, parses_one_byte_at_a_time)
{
    /* TCP gives no guarantee about how a message is split across reads. A
     * parser that assumes one read yields one request breaks on slow clients,
     * which is exactly what this test reproduces. */
    http_request_t r;
    http_request_init(&r);

    const char *raw = "POST /api/v1/shorten HTTP/1.1\r\n"
                      "Host: x\r\nContent-Length: 9\r\n\r\n{\"url\":1}";
    size_t total = strlen(raw);

    for (size_t i = 1; i <= total; i++) {
        int rc = http_parse(&r, raw, i);
        if (i < total)
            cr_assert_eq(rc, HTTP_PARSE_NEED_MORE, "byte %zu of %zu", i, total);
        else
            cr_assert_eq(rc, HTTP_PARSE_COMPLETE, "final byte %zu", i);
    }

    cr_assert_str_eq(r.method, "POST");
    cr_assert_str_eq(r.body, "{\"url\":1}");
    http_request_reset(&r);
}

Test(http, parses_a_request_with_no_headers)
{
    http_request_t r;
    http_request_init(&r);
    const char *raw = "GET /health HTTP/1.0\r\n\r\n";

    cr_assert_eq(http_parse(&r, raw, strlen(raw)), HTTP_PARSE_COMPLETE);
    cr_assert_eq(r.keep_alive, 0); /* HTTP/1.0 defaults to close */
    http_request_reset(&r);
}

Test(http, honours_connection_close)
{
    http_request_t r;
    http_request_init(&r);
    const char *raw = "GET /a HTTP/1.1\r\nConnection: close\r\n\r\n";

    cr_assert_eq(http_parse(&r, raw, strlen(raw)), HTTP_PARSE_COMPLETE);
    cr_assert_eq(r.keep_alive, 0);
    http_request_reset(&r);
}

Test(http, rejects_a_non_http_request_line)
{
    http_request_t r;
    http_request_init(&r);
    const char *raw = "GET\r\n\r\n";

    cr_assert_eq(http_parse(&r, raw, strlen(raw)), HTTP_PARSE_MALFORMED);
    http_request_reset(&r);
}

Test(http, rejects_chunked_transfer_encoding)
{
    /* v1 does not implement chunked. Silently mis-parsing it would corrupt the
     * body, so it is refused and the connection answers 400. */
    http_request_t r;
    http_request_init(&r);
    const char *raw = "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n";

    cr_assert_eq(http_parse(&r, raw, strlen(raw)), HTTP_PARSE_MALFORMED);
    http_request_reset(&r);
}

Test(http, rejects_negative_content_length)
{
    http_request_t r;
    http_request_init(&r);
    const char *raw = "POST / HTTP/1.1\r\nContent-Length: -1\r\n\r\n";

    cr_assert_eq(http_parse(&r, raw, strlen(raw)), HTTP_PARSE_MALFORMED);
    http_request_reset(&r);
}

Test(http, rejects_non_numeric_content_length)
{
    http_request_t r;
    http_request_init(&r);
    const char *raw = "POST / HTTP/1.1\r\nContent-Length: abc\r\n\r\n";

    cr_assert_eq(http_parse(&r, raw, strlen(raw)), HTTP_PARSE_MALFORMED);
    http_request_reset(&r);
}

Test(http, rejects_a_path_longer_than_the_buffer)
{
    http_request_t r;
    http_request_init(&r);

    buf_t *b = buf_new(64);
    buf_append_str(b, "GET /");
    for (int i = 0; i < HTTP_MAX_PATH + 16; i++)
        buf_append_str(b, "a");
    buf_append_str(b, " HTTP/1.1\r\n\r\n");

    cr_assert_eq(http_parse(&r, (const char *)b->data, b->len), HTTP_PARSE_MALFORMED);
    buf_free(b);
    http_request_reset(&r);
}

Test(http, request_line_without_version_is_malformed)
{
    http_request_t r;
    http_request_init(&r);
    const char *raw = "GET /path\r\n\r\n";

    cr_assert_eq(http_parse(&r, raw, strlen(raw)), HTTP_PARSE_MALFORMED);
    http_request_reset(&r);
}

Test(http, response_301_carries_location)
{
    buf_t *out;
    buf_new_into(&out, 64);
    cr_assert_not_null(out);

    /* Signature is (out, status, content_type, body, location, keep_alive):
     * a 301 has no body, so the URL goes in the location slot. */
    http_response_write(out, 301, NULL, NULL, "https://example.com/x", 1);

    char *s = (char *)out->data;
    cr_assert_not_null(strstr(s, "301 Moved Permanently"));
    cr_assert_not_null(strstr(s, "Location: https://example.com/x"));
    /* A 301 with no Content-Length makes clients wait for a body that never
     * comes, so the zero length must still be declared. */
    cr_assert_not_null(strstr(s, "Content-Length: 0"));

    buf_free(out);
}

Test(http, response_201_is_json_with_body)
{
    buf_t *out;
    buf_new_into(&out, 64);
    cr_assert_not_null(out);

    http_response_write(out, 201, "application/json", "{\"code\":\"abc1234\"}", NULL, 1);

    char *s = (char *)out->data;
    cr_assert_not_null(strstr(s, "HTTP/1.1 201 Created"));
    cr_assert_not_null(strstr(s, "Content-Type: application/json"));
    cr_assert_not_null(strstr(s, "Content-Length: 18"));
    cr_assert_not_null(strstr(s, "Connection: keep-alive"));

    buf_free(out);
}

Test(http, response_without_keep_alive_says_close)
{
    buf_t *out;
    buf_new_into(&out, 64);
    cr_assert_not_null(out);

    http_response_write(out, 200, "text/plain", "ok", NULL, 0);
    cr_assert_not_null(strstr((char *)out->data, "Connection: close"));

    buf_free(out);
}

Test(http, status_text_is_defined_for_the_codes_we_use)
{
    cr_assert_str_eq(http_status_text(200), "OK");
    cr_assert_str_eq(http_status_text(301), "Moved Permanently");
    cr_assert_str_eq(http_status_text(400), "Bad Request");
    cr_assert_str_eq(http_status_text(404), "Not Found");
    cr_assert_str_eq(http_status_text(410), "Gone");
    cr_assert_str_eq(http_status_text(429), "Too Many Requests");
    cr_assert_str_eq(http_status_text(503), "Service Unavailable");
    /* An unknown code must still yield a printable string, never NULL, because
     * the response builder puts this straight into the status line. */
    cr_assert_not_null(http_status_text(599));
    cr_assert_str_eq(http_status_text(599), "Unknown");
}
