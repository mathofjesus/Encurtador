/* End-to-end tests: a real client over a real socket against the real stack.
 *
 * These are the only tests that exercise the reactor, the parser, the router,
 * the handlers and storage in one path. Every unit test below them can pass
 * while the assembled server answers nothing, which is exactly the failure this
 * suite exists to catch.
 *
 * The reactor is stepped with reactor_poll_once rather than run on a thread. A
 * threaded server would make each assertion a race between accept and assert;
 * stepping it makes every check about what the server replied.
 */
#include <arpa/inet.h>
#include <criterion/criterion.h>
#include <libpq-fe.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "api/application.h"
#include "codegen.h"
#include "net/reactor.h"

static application_t *app;
static reactor_t *reactor;

static const char *test_dsn(void)
{
    const char *dsn = getenv("TEST_DATABASE_URL");
    if (!dsn || !*dsn)
        dsn = getenv("DATABASE_URL");
    return dsn;
}

static const char *test_redis_url(void)
{
    const char *url = getenv("TEST_REDIS_URL");
    if (!url || !*url)
        url = getenv("REDIS_URL");
    return url;
}

static void fill_config(config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    const char *dsn = test_dsn();
    cr_assert_not_null(dsn, "neither TEST_DATABASE_URL nor DATABASE_URL is set");
    snprintf(cfg->database.primary_dsn, sizeof(cfg->database.primary_dsn), "%s", dsn);
    cfg->database.pool_size = 2;

    const char *redis = test_redis_url();
    if (redis && *redis)
        snprintf(cfg->cache.redis_url, sizeof(cfg->cache.redis_url), "%s", redis);
    cfg->cache.redis_ttl_seconds = 3600;

    snprintf(cfg->app.base_url, sizeof(cfg->app.base_url), "https://short.ly");
    cfg->app.port = 0;
    cfg->app.workers = 1;

    cfg->rate_limit.per_minute = 60;
    cfg->rate_limit.burst = 10;
    cfg->retention.default_ttl_days = 30;
    cfg->retention.max_ttl_days = 3650;
    cfg->codegen.min_length = CODE_MIN_LENGTH;
    cfg->codegen.max_length = CODE_MAX_LENGTH;
}

/* Removes what this suite created. Generated codes are unique by construction,
 * so they are matched by their target URL rather than by code. */
static void wipe_rows(void)
{
    PGconn *c = PQconnectdb(test_dsn());
    cr_assert_eq(PQstatus(c), CONNECTION_OK, "connect failed: %s", PQerrorMessage(c));
    PGresult *r = PQexec(c, "DELETE FROM urls WHERE url LIKE 'https://example.com/e2e%'");
    cr_assert(PQresultStatus(r) == PGRES_COMMAND_OK, "%s", PQerrorMessage(c));
    PQclear(r);
    PQfinish(c);
}

static void e2e_setup(void)
{
    config_t cfg;
    fill_config(&cfg);

    app = application_new(&cfg, NULL);
    cr_assert_not_null(app, "application_new failed (is TEST_DATABASE_URL set?)");

    reactor = reactor_new(0, 64);
    cr_assert_not_null(reactor, "reactor_new failed");
    cr_assert_eq(reactor_set_handler(reactor, application_handle_conn, app), 0);
}

static void e2e_teardown(void)
{
    reactor_free(reactor);
    application_free(app);
    reactor = NULL;
    app = NULL;
    wipe_rows();
}

TestSuite(e2e, .init = e2e_setup, .fini = e2e_teardown);

/* --- client ------------------------------------------------------------- */

static int connect_client(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    cr_assert(fd >= 0, "socket() failed");

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)reactor_port(reactor));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    cr_assert_eq(connect(fd, (struct sockaddr *)&addr, sizeof(addr)), 0,
                 "connect to the reactor failed");
    return fd;
}

static int response_complete(const char *buf, size_t len, int expect_body)
{
    const char *end = strstr(buf, "\r\n\r\n");
    if (!end)
        return 0;
    if (!expect_body)
        return 1;

    size_t header_len = (size_t)(end - buf) + 4;
    const char *cl = strstr(buf, "Content-Length: ");
    if (!cl)
        return 0;
    long n = strtol(cl + strlen("Content-Length: "), NULL, 10);
    if (n < 0)
        return 0;
    return header_len + (size_t)n <= len;
}

/* Sends one request and returns the whole response. expect_body is 0 only for
 * HEAD, whose declared length is never followed by a body. */
static int do_request(int fd, const char *method, const char *path,
                      const char *body, int expect_body, char *resp, size_t cap)
{
    char req[2048];
    size_t body_len = body ? strlen(body) : 0;
    int n = snprintf(req, sizeof(req),
                     "%s %s HTTP/1.1\r\nHost: localhost\r\n"
                     "Content-Length: %zu\r\n\r\n%s",
                     method, path, body_len, body ? body : "");
    cr_assert_lt(n, (int)sizeof(req));

    size_t sent = 0;
    size_t reqlen = strlen(req);
    while (sent < reqlen) {
        ssize_t w = send(fd, req + sent, reqlen - sent, MSG_NOSIGNAL);
        cr_assert_gt(w, 0, "send failed");
        sent += (size_t)w;
    }

    size_t total = 0;
    resp[0] = '\0';
    for (int steps = 0; steps < 500; steps++) {
        (void)reactor_poll_once(reactor, 20);
        for (;;) {
            if (total + 1 >= cap)
                break;
            ssize_t got = recv(fd, resp + total, cap - 1 - total, MSG_DONTWAIT);
            if (got > 0) {
                total += (size_t)got;
                resp[total] = '\0';
                continue;
            }
            break;
        }
        if (response_complete(resp, total, expect_body))
            break;
    }
    return (int)total;
}

static void extract_code(const char *resp, char *out, size_t cap)
{
    const char *p = strstr(resp, "\"code\":\"");
    cr_assert_not_null(p, "no code in: %s", resp);
    p += strlen("\"code\":\"");
    const char *q = strchr(p, '"');
    cr_assert_not_null(q, "unterminated code in: %s", resp);

    size_t n = (size_t)(q - p);
    cr_assert_lt(n, cap);
    memcpy(out, p, n);
    out[n] = '\0';
}

/* --- tests -------------------------------------------------------------- */

Test(e2e, health_over_a_socket_is_200)
{
    int fd = connect_client();
    char resp[4096];
    cr_assert_gt(do_request(fd, "GET", "/health", NULL, 1, resp, sizeof(resp)), 0);
    cr_assert_not_null(strstr(resp, "200 OK"), "%s", resp);
    cr_assert_not_null(strstr(resp, "Content-Length: 3"), "%s", resp);
    cr_assert_not_null(strstr(resp, "ok"), "%s", resp);
    close(fd);
}

Test(e2e, shorten_then_redirect_round_trips)
{
    int fd = connect_client();
    char resp[8192];

    cr_assert_gt(do_request(fd, "POST", "/api/v1/shorten",
                            "{\"url\":\"https://example.com/e2e-roundtrip\"}", 1,
                            resp, sizeof(resp)),
                 0);
    cr_assert_not_null(strstr(resp, "201 Created"), "%s", resp);

    char code[CODE_MAX_LENGTH + 1];
    extract_code(resp, code, sizeof(code));
    cr_assert_eq(strlen(code), (size_t)CODE_MIN_LENGTH);

    char location[256];
    snprintf(location, sizeof(location),
             "Location: https://short.ly/%s", code);
    cr_assert_not_null(strstr(resp, location), "%s", resp);

    char path[64];
    snprintf(path, sizeof(path), "/%s", code);
    cr_assert_gt(do_request(fd, "GET", path, NULL, 1, resp, sizeof(resp)), 0);
    cr_assert_not_null(strstr(resp, "301 Moved Permanently"), "%s", resp);
    cr_assert_not_null(strstr(resp, "Location: https://example.com/e2e-roundtrip"),
                       "%s", resp);

    close(fd);
}

Test(e2e, two_requests_reuse_one_connection)
{
    int fd = connect_client();
    char resp[8192];

    cr_assert_gt(do_request(fd, "GET", "/health", NULL, 1, resp, sizeof(resp)), 0);
    cr_assert_not_null(strstr(resp, "200 OK"), "%s", resp);
    cr_assert_not_null(strstr(resp, "Connection: keep-alive"), "%s", resp);

    /* Same socket, second request: if the parser did not consume the first
     * request's bytes this would re-answer the first. */
    cr_assert_gt(do_request(fd, "GET", "/metrics", NULL, 1, resp, sizeof(resp)), 0);
    cr_assert_not_null(strstr(resp, "shortener_shorten_total"), "%s", resp);

    close(fd);
}

Test(e2e, an_unknown_code_is_404_over_the_wire)
{
    int fd = connect_client();
    char resp[4096];
    cr_assert_gt(do_request(fd, "GET", "/zzzzzzz", NULL, 1, resp, sizeof(resp)), 0);
    cr_assert_not_null(strstr(resp, "404 Not Found"), "%s", resp);
    close(fd);
}

Test(e2e, a_malformed_body_is_400)
{
    int fd = connect_client();
    char resp[4096];
    cr_assert_gt(do_request(fd, "POST", "/api/v1/shorten", "not json", 1, resp,
                            sizeof(resp)),
                 0);
    cr_assert_not_null(strstr(resp, "400 Bad Request"), "%s", resp);
    close(fd);
}

Test(e2e, metrics_counts_a_create)
{
    int fd = connect_client();
    char resp[8192];

    cr_assert_gt(do_request(fd, "POST", "/api/v1/shorten",
                            "{\"url\":\"https://example.com/e2e-metrics\"}", 1, resp,
                            sizeof(resp)),
                 0);
    cr_assert_not_null(strstr(resp, "201 Created"), "%s", resp);

    cr_assert_gt(do_request(fd, "GET", "/metrics", NULL, 1, resp, sizeof(resp)), 0);
    cr_assert_not_null(strstr(resp, "shortener_shorten_total 1"), "%s", resp);

    close(fd);
}

Test(e2e, head_carries_the_length_but_no_body)
{
    int fd = connect_client();
    char resp[4096];
    cr_assert_gt(do_request(fd, "HEAD", "/health", NULL, 0, resp, sizeof(resp)), 0);
    cr_assert_not_null(strstr(resp, "200 OK"), "%s", resp);
    cr_assert_not_null(strstr(resp, "Content-Length: 3"), "%s", resp);

    const char *body = strstr(resp, "\r\n\r\n");
    cr_assert_not_null(body, "%s", resp);
    cr_assert_eq(body[4], '\0', "a HEAD response carried a body: %s", resp);

    close(fd);
}
