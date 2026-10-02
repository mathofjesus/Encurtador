/* Router tests.
 *
 * These drive router_dispatch directly with a hand-built request, so they are
 * about the routing rules and not about sockets. They still need Postgres and
 * Redis for the routes that resolve URLs.
 */
#include <criterion/criterion.h>
#include <libpq-fe.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api/router.h"
#include "codegen.h"
#include "db/pg.h"

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

typedef struct {
    pg_pool_t *pool;
    redis_client_t *cache;
    config_t cfg;
    url_service_t *svc;
    rl_t *rl;
    metrics_t *metrics;
    router_t *rt;
} fixture_t;

static void fill_config(config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->app.base_url, sizeof(cfg->app.base_url), "https://short.ly");
    cfg->cache.redis_ttl_seconds = 3600;
    cfg->retention.default_ttl_days = 30;
    cfg->retention.max_ttl_days = 3650;
    cfg->codegen.min_length = CODE_MIN_LENGTH;
    cfg->codegen.max_length = CODE_MAX_LENGTH;
    cfg->rate_limit.per_minute = 60;
    cfg->rate_limit.burst = 10;
}

/* with_limiter: 0 no limiter, 1 60/minute burst 1, 2 60/minute burst 10. */
static void fixture_up(fixture_t *f, int limiter_kind)
{
    const char *dsn = test_dsn();
    cr_assert_not_null(dsn, "neither TEST_DATABASE_URL nor DATABASE_URL is set");
    const char *rurl = test_redis_url();
    cr_assert_not_null(rurl, "neither TEST_REDIS_URL nor REDIS_URL is set");

    f->pool = pg_pool_new(dsn, 2);
    cr_assert_not_null(f->pool);
    cr_assert_eq(pg_run_schema(f->pool), PG_OK);

    f->cache = redis_init(rurl);
    cr_assert_not_null(f->cache);

    fill_config(&f->cfg);
    f->svc = url_service_new(f->pool, f->cache, &f->cfg);
    cr_assert_not_null(f->svc);

    if (limiter_kind == 1)
        f->rl = rl_new(60, 1, 64);
    else if (limiter_kind == 2)
        f->rl = rl_new(60, 10, 64);
    else
        f->rl = NULL;

    f->metrics = metrics_new();
    f->rt = router_new(f->svc, f->rl, &f->cfg, f->metrics);
    cr_assert_not_null(f->rt);
}

static void fixture_down(fixture_t *f)
{
    router_free(f->rt);
    metrics_free(f->metrics);
    rl_free(f->rl);
    url_service_free(f->svc);
    redis_free(f->cache);
    pg_pool_free(f->pool);
}

static void wipe_codes(void)
{
    PGconn *c = PQconnectdb(test_dsn());
    cr_assert_eq(PQstatus(c), CONNECTION_OK, "connect failed: %s", PQerrorMessage(c));
    PGresult *r = PQexec(c, "DELETE FROM urls WHERE code LIKE 'test%'");
    cr_assert(PQresultStatus(r) == PGRES_COMMAND_OK, "%s", PQerrorMessage(c));
    PQclear(r);
    PQfinish(c);
}

static http_request_t make_request(const char *method, const char *path,
                                   const char *body)
{
    http_request_t req;
    http_request_init(&req);
    snprintf(req.method, sizeof(req.method), "%s", method);
    snprintf(req.path, sizeof(req.path), "%s", path);
    if (body) {
        snprintf(req.body, sizeof(req.body), "%s", body);
        req.body_len = strlen(body);
    }
    req.keep_alive = 1;
    req.complete = 1;
    return req;
}

/* Dispatches and returns the buffer; caller frees. */
static buf_t *dispatch(fixture_t *f, http_request_t *req)
{
    buf_t *out = buf_new(1024);
    cr_assert_not_null(out);
    int rc = router_dispatch(f->rt, req, "10.0.0.1", out);
    cr_assert_eq(rc, 0, "a keep-alive request should keep the connection");
    return out;
}

static const char *body_of(buf_t *out)
{
    return out->data ? out->data : "";
}

Test(router, health_is_200_without_touching_storage)
{
    fixture_t f;
    fixture_up(&f, 0);

    http_request_t req = make_request("GET", "/health", NULL);
    buf_t *out = dispatch(&f, &req);
    cr_assert_not_null(strstr(body_of(out), "200 OK"));
    cr_assert_not_null(strstr(body_of(out), "ok"));
    buf_free(out);

    fixture_down(&f);
}

Test(router, metrics_lists_the_counters)
{
    fixture_t f;
    fixture_up(&f, 0);

    /* Create one URL through the router so a counter is non-zero. */
    http_request_t post = make_request("POST", "/api/v1/shorten",
                                       "{\"url\":\"https://example.com/m\"}");
    buf_t *created = dispatch(&f, &post);
    cr_assert_not_null(strstr(body_of(created), "201 Created"));
    buf_free(created);

    http_request_t req = make_request("GET", "/metrics", NULL);
    buf_t *out = dispatch(&f, &req);
    cr_assert_not_null(strstr(body_of(out), "200 OK"));
    cr_assert_not_null(strstr(body_of(out), "shortener_shorten_total 1"));
    buf_free(out);

    wipe_codes();
    fixture_down(&f);
}

Test(router, shorten_returns_201_with_a_code_and_a_location)
{
    fixture_t f;
    fixture_up(&f, 0);

    http_request_t req = make_request("POST", "/api/v1/shorten",
                                      "{\"url\":\"https://example.com/created\"}");
    buf_t *out = dispatch(&f, &req);

    cr_assert_not_null(strstr(body_of(out), "201 Created"));
    cr_assert_not_null(strstr(body_of(out), "Content-Type: application/json"));

    const char *code_field = strstr(body_of(out), "\"code\":\"");
    cr_assert_not_null(code_field, "the body carries no code: %s", body_of(out));
    code_field += strlen("\"code\":\"");
    char code[CODE_MAX_LENGTH + 1];
    size_t n = 0;
    while (code_field[n] && code_field[n] != '"' && n < CODE_MAX_LENGTH)
        n++;
    memcpy(code, code_field, n);
    code[n] = '\0';
    cr_assert_eq(n, (size_t)CODE_MIN_LENGTH, "generated code width: %s", code);

    char want_location[128];
    snprintf(want_location, sizeof(want_location), "Location: https://short.ly/%s", code);
    cr_assert_not_null(strstr(body_of(out), want_location),
                       "missing or wrong Location, wanted %s", want_location);

    /* And it resolves. */
    char path[64];
    snprintf(path, sizeof(path), "/%s", code);
    http_request_t get = make_request("GET", path, NULL);
    buf_t *redir = dispatch(&f, &get);
    cr_assert_not_null(strstr(body_of(redir), "301 Moved Permanently"));
    cr_assert_not_null(strstr(body_of(redir), "Location: https://example.com/created"));
    buf_free(redir);
    buf_free(out);

    wipe_codes();
    fixture_down(&f);
}

Test(router, a_custom_code_is_echoed_back)
{
    fixture_t f;
    fixture_up(&f, 0);
    wipe_codes();

    http_request_t req = make_request("POST", "/api/v1/shorten",
        "{\"url\":\"https://example.com/c\",\"custom_code\":\"testrtr01\"}");
    buf_t *out = dispatch(&f, &req);

    cr_assert_not_null(strstr(body_of(out), "201 Created"));
    cr_assert_not_null(strstr(body_of(out), "\"code\":\"testrtr01\""),
                       "the chosen code was not echoed: %s", body_of(out));
    buf_free(out);

    wipe_codes();
    fixture_down(&f);
}

Test(router, a_reused_custom_code_is_409)
{
    fixture_t f;
    fixture_up(&f, 0);
    wipe_codes();

    http_request_t first = make_request("POST", "/api/v1/shorten",
        "{\"url\":\"https://example.com/1\",\"custom_code\":\"testrtr02\"}");
    buf_t *a = dispatch(&f, &first);
    cr_assert_not_null(strstr(body_of(a), "201 Created"));
    buf_free(a);

    http_request_t second = make_request("POST", "/api/v1/shorten",
        "{\"url\":\"https://example.com/2\",\"custom_code\":\"testrtr02\"}");
    buf_t *b = dispatch(&f, &second);
    cr_assert_not_null(strstr(body_of(b), "409 Conflict"));
    buf_free(b);

    wipe_codes();
    fixture_down(&f);
}

Test(router, a_body_that_is_not_json_is_400)
{
    fixture_t f;
    fixture_up(&f, 0);

    http_request_t req = make_request("POST", "/api/v1/shorten", "not json");
    buf_t *out = dispatch(&f, &req);
    cr_assert_not_null(strstr(body_of(out), "400 Bad Request"));
    buf_free(out);

    fixture_down(&f);
}

Test(router, a_body_without_a_url_is_400)
{
    fixture_t f;
    fixture_up(&f, 0);

    http_request_t req = make_request("POST", "/api/v1/shorten", "{\"ttl_days\":7}");
    buf_t *out = dispatch(&f, &req);
    cr_assert_not_null(strstr(body_of(out), "400 Bad Request"));
    buf_free(out);

    fixture_down(&f);
}

Test(router, an_unknown_code_is_404)
{
    fixture_t f;
    fixture_up(&f, 0);

    http_request_t req = make_request("GET", "/testnope9", NULL);
    buf_t *out = dispatch(&f, &req);
    cr_assert_not_null(strstr(body_of(out), "404 Not Found"));
    buf_free(out);

    fixture_down(&f);
}

Test(router, a_path_that_could_never_be_a_code_is_404_not_400)
{
    fixture_t f;
    fixture_up(&f, 0);

    /* A dot is not in the alphabet. Browsers ask for this unprompted, and a 400
     * would call every such request a client error. */
    http_request_t req = make_request("GET", "/favicon.ico", NULL);
    buf_t *out = dispatch(&f, &req);
    cr_assert_not_null(strstr(body_of(out), "404 Not Found"));
    buf_free(out);

    /* More than one segment is not a code either. */
    http_request_t deep = make_request("GET", "/a/b", NULL);
    buf_t *out2 = dispatch(&f, &deep);
    cr_assert_not_null(strstr(body_of(out2), "404 Not Found"));
    buf_free(out2);

    fixture_down(&f);
}

Test(router, a_write_past_the_burst_is_429_with_retry_after)
{
    fixture_t f;
    fixture_up(&f, 1); /* burst of one */

    http_request_t first = make_request("POST", "/api/v1/shorten",
        "{\"url\":\"https://example.com/rate1\"}");
    buf_t *a = dispatch(&f, &first);
    cr_assert_not_null(strstr(body_of(a), "201 Created"));
    buf_free(a);

    http_request_t second = make_request("POST", "/api/v1/shorten",
        "{\"url\":\"https://example.com/rate2\"}");
    buf_t *b = dispatch(&f, &second);
    cr_assert_not_null(strstr(body_of(b), "429 Too Many Requests"),
                       "the second write was not throttled: %s", body_of(b));
    cr_assert_not_null(strstr(body_of(b), "Retry-After: 1"),
                       "a 429 must say when to retry: %s", body_of(b));
    cr_assert_eq(metrics_get(f.metrics, METRIC_RATE_LIMITED_TOTAL), 1);
    buf_free(b);

    wipe_codes();
    fixture_down(&f);
}

Test(router, a_read_is_not_rate_limited)
{
    fixture_t f;
    fixture_up(&f, 1);

    /* Consume the single token with a write. */
    http_request_t post = make_request("POST", "/api/v1/shorten",
        "{\"url\":\"https://example.com/read1\"}");
    buf_t *a = dispatch(&f, &post);
    buf_free(a);

    /* Reads still work: the limiter is for writes. */
    http_request_t get = make_request("GET", "/health", NULL);
    buf_t *out = dispatch(&f, &get);
    cr_assert_not_null(strstr(body_of(out), "200 OK"),
                       "a read was throttled: %s", body_of(out));
    buf_free(out);

    wipe_codes();
    fixture_down(&f);
}

Test(router, head_returns_the_length_but_no_body)
{
    fixture_t f;
    fixture_up(&f, 0);

    http_request_t req = make_request("HEAD", "/health", NULL);
    buf_t *out = dispatch(&f, &req);

    cr_assert_not_null(strstr(body_of(out), "200 OK"));
    cr_assert_not_null(strstr(body_of(out), "Content-Length: 3"),
                       "HEAD must declare the length the GET would return");
    char *end = strstr(body_of(out), "\r\n\r\n");
    cr_assert_not_null(end);
    cr_assert_eq(end[4], '\0', "a HEAD response carried a body");
    buf_free(out);

    fixture_down(&f);
}

Test(router, a_wrong_method_on_a_known_path_is_405)
{
    fixture_t f;
    fixture_up(&f, 0);

    http_request_t req = make_request("POST", "/health", NULL);
    buf_t *out = dispatch(&f, &req);
    cr_assert_not_null(strstr(body_of(out), "405 Method Not Allowed"));
    cr_assert_not_null(strstr(body_of(out), "Allow:"));
    buf_free(out);

    http_request_t get = make_request("GET", "/api/v1/shorten", NULL);
    buf_t *out2 = dispatch(&f, &get);
    cr_assert_not_null(strstr(body_of(out2), "405 Method Not Allowed"));
    buf_free(out2);

    fixture_down(&f);
}

Test(router, a_query_string_is_ignored_for_routing)
{
    fixture_t f;
    fixture_up(&f, 0);

    http_request_t req = make_request("GET", "/health?verbose=1", NULL);
    buf_t *out = dispatch(&f, &req);
    cr_assert_not_null(strstr(body_of(out), "200 OK"));
    buf_free(out);

    fixture_down(&f);
}

Test(router, a_request_that_asks_to_close_returns_minus_one)
{
    fixture_t f;
    fixture_up(&f, 0);

    http_request_t req = make_request("GET", "/health", NULL);
    req.keep_alive = 0;
    buf_t *out = buf_new(256);
    int rc = router_dispatch(f.rt, &req, "10.0.0.1", out);
    cr_assert_eq(rc, -1, "Connection: close must tell the reactor to close");
    cr_assert_not_null(strstr(body_of(out), "Connection: close"));
    buf_free(out);

    fixture_down(&f);
}

Test(router, null_arguments_are_safe)
{
    fixture_t f;
    fixture_up(&f, 0);

    buf_t *out = buf_new(64);
    http_request_t req = make_request("GET", "/health", NULL);

    cr_assert_eq(router_dispatch(NULL, &req, "10.0.0.1", out), -1);
    cr_assert_eq(router_dispatch(f.rt, NULL, "10.0.0.1", out), -1);
    cr_assert_eq(router_dispatch(f.rt, &req, "10.0.0.1", NULL), -1);

    /* client_ip NULL disables limiting rather than failing. */
    http_request_t post = make_request("POST", "/api/v1/shorten",
        "{\"url\":\"https://example.com/noip\"}");
    buf_t *out2 = buf_new(512);
    cr_assert_eq(router_dispatch(f.rt, &post, NULL, out2), 0);
    cr_assert_not_null(strstr(body_of(out2), "201 Created"));
    buf_free(out2);

    buf_free(out);
    wipe_codes();
    fixture_down(&f);
}

Test(router, a_router_without_a_service_or_config_is_refused)
{
    cr_assert_null(router_new(NULL, NULL, NULL, NULL));
}
