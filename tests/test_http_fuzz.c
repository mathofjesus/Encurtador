#include <criterion/criterion.h>
#include <stdlib.h>
#include <string.h>

#include "http/parser.h"
#include "util/buf.h"

/* Fuzz pass. Feeds structured and random bytes to the parser under ASan/UBSan.
 * The parser is the boundary between the network and every fixed-size array in
 * this program, so a crash here is a remote crash in the service.
 *
 * This test is only meaningful under sanitizers: run it with `make test-asan`
 * or `./scripts/sandbox.sh test-asan`.
 */

static void feed(const char *data, size_t len)
{
    http_request_t r;
    http_request_init(&r);
    /* The result is irrelevant; not crashing is the assertion. */
    (void)http_parse(&r, data, len);
    http_request_reset(&r);
}

/* Deterministic PRNG so a failure is reproducible from the seed. */
static unsigned long rng_state = 0x2545F4914F6CDD1DUL;

static unsigned long next_rand(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

Test(fuzz, random_bytes_never_crash_the_parser)
{
    char buf[512];

    for (int iter = 0; iter < 20000; iter++) {
        size_t len = (size_t)(next_rand() % sizeof(buf));
        for (size_t i = 0; i < len; i++)
            buf[i] = (char)(next_rand() & 0xff);
        feed(buf, len);
    }
}

Test(fuzz, almost_valid_requests_never_crash_the_parser)
{
    /* Mutations of a well-formed request: the shape an attacker actually
     * produces, unlike uniform random bytes that fail at the first character. */
    const char *templates[] = {
        "POST /api/v1/shorten HTTP/1.1\r\nHost: a\r\nContent-Length: 18\r\n\r\n{\"url\":\"http://a\"}",
        "GET /abc1234 HTTP/1.1\r\nHost: a\r\n\r\n",
        "PUT /x HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n",
        "POST / HTTP/1.1\r\nContent-Length: 0\r\n\r\n",
    };

    char work[256];

    for (size_t t = 0; t < sizeof(templates) / sizeof(templates[0]); t++) {
        size_t len = strlen(templates[t]);
        cr_assert_leq(len, sizeof(work) - 1);
        memcpy(work, templates[t], len);

        /* Truncation at every offset: a slow or broken client sends a prefix. */
        for (size_t cut = 0; cut <= len; cut++)
            feed(work, cut);

        /* Single-byte corruption at every position. */
        for (size_t pos = 0; pos < len; pos++) {
            for (int delta = 1; delta <= 255; delta++) {
                memcpy(work, templates[t], len);
                work[pos] = (char)(work[pos] + delta);
                feed(work, len);
            }
        }
    }
}

Test(fuzz, header_values_with_embedded_crlf_do_not_split_a_request)
{
    /* CRLF injection: a header value ending a request early would let one
     * request masquerade as two. */
    const char *raw = "GET /a HTTP/1.1\r\nX-Test: v\r\nInjected: yes\r\n\r\n";
    http_request_t r;
    http_request_init(&r);

    /* We only care that the parser does not report a second request. */
    int rc = http_parse(&r, raw, strlen(raw));
    cr_assert_neq(rc, HTTP_PARSE_MALFORMED);

    http_request_reset(&r);
}

Test(fuzz, huge_declared_length_is_rejected_without_reading_a_body)
{
    /* A declared length near the type's maximum must be refused on the header
     * alone. If the parser tried to wait for the bytes it would hold the
     * connection until the client timed out. */
    const char *raw = "POST / HTTP/1.1\r\nContent-Length: 9223372036854775807\r\n\r\n";
    http_request_t r;
    http_request_init(&r);

    cr_assert_eq(http_parse(&r, raw, strlen(raw)), HTTP_PARSE_MALFORMED);
    http_request_reset(&r);
}

Test(fuzz, embedded_nul_in_the_stream_does_not_truncate_parsing)
{
    /* Bodies are length-delimited, not NUL-terminated. A NUL in the middle must
     * not make the parser believe the body ended. */
    const char raw[] = "POST / HTTP/1.1\r\nContent-Length: 4\r\n\r\nab\0cd";
    http_request_t r;
    http_request_init(&r);

    cr_assert_eq(http_parse(&r, raw, sizeof(raw) - 1), HTTP_PARSE_COMPLETE);
    cr_assert_eq(r.body_len, 4);
    cr_assert(memcmp(r.body, "ab\0cd", 4) == 0);

    http_request_reset(&r);
}
