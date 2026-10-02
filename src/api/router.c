#include "api/router.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "api/handlers.h"
#include "codegen.h"

struct router {
    url_service_t *svc;
    rl_t *rl; /* may be NULL */
    const config_t *cfg;
    metrics_t *metrics; /* may be NULL */
};

router_t *router_new(url_service_t *svc, rl_t *rl, const config_t *cfg,
                     metrics_t *metrics)
{
    if (!svc || !cfg)
        return NULL;

    router_t *r = calloc(1, sizeof(*r));
    if (!r)
        return NULL;

    r->svc = svc;
    r->rl = rl;
    r->cfg = cfg;
    r->metrics = metrics;
    return r;
}

void router_free(router_t *r)
{
    free(r);
}

/* Monotonic, because a limiter fed by a clock that can jump backwards would
 * either refill a bucket early or drain it. */
static long long monotonic_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int method_is(const http_request_t *req, const char *method)
{
    return strcmp(req->method, method) == 0;
}

/* memcmp rather than strcmp because path is not NUL-terminated at path_len when
 * a query string follows it. */
static int path_is(const char *path, size_t path_len, const char *literal)
{
    size_t n = strlen(literal);
    return path_len == n && memcmp(path, literal, n) == 0;
}

int router_dispatch(router_t *r, const http_request_t *req, const char *client_ip,
                    buf_t *out)
{
    if (!r || !req || !out)
        return -1;

    const int keep_alive = req->keep_alive;
    const int head_only = method_is(req, "HEAD");
    handler_ctx_t ctx = {r->svc, r->cfg, r->metrics, keep_alive, head_only};
    const int rc = keep_alive ? 0 : -1;

    const char *path = req->path;
    const char *query = strchr(path, '?');
    size_t path_len = query ? (size_t)(query - path) : strlen(path);

    if (path_is(path, path_len, "/health")) {
        if (method_is(req, "GET") || head_only)
            handle_health(&ctx, out);
        else
            handle_method_not_allowed(&ctx, out);
        return rc;
    }

    if (path_is(path, path_len, "/metrics")) {
        if (method_is(req, "GET") || head_only)
            handle_metrics(&ctx, out);
        else
            handle_method_not_allowed(&ctx, out);
        return rc;
    }

    if (path_is(path, path_len, "/api/v1/shorten")) {
        if (!method_is(req, "POST")) {
            handle_method_not_allowed(&ctx, out);
            return rc;
        }

        /* Only writes are limited. A redirect is a cache hit most of the time
         * and limiting reads would penalise the very case the cache exists
         * for. */
        if (r->rl && client_ip && rl_allow(r->rl, client_ip, monotonic_ms()) == 0) {
            if (r->metrics)
                metrics_inc(r->metrics, METRIC_RATE_LIMITED_TOTAL);

            /* Seconds until the bucket can hold a token again. With the
             * configured 60/minute this is 1; the formula keeps the header
             * honest if the rate changes. */
            int per_minute = r->cfg->rate_limit.per_minute;
            int retry = per_minute > 0 ? (60 + per_minute - 1) / per_minute : 60;
            if (retry < 1)
                retry = 1;

            char extra[64];
            snprintf(extra, sizeof(extra), "Retry-After: %d\r\n", retry);
            handle_rate_limited(&ctx, out, extra);
            return rc;
        }

        handle_shorten(&ctx, req, out);
        return rc;
    }

    /* Anything else of the form /{code}: a single path segment. A longer or
     * multi-segment path is not a code and falls through to 404 rather than
     * being handed to the database. */
    size_t code_len = path_len >= 1 ? path_len - 1 : 0;
    if (path_len >= 2 && path[0] == '/' && code_len <= CODE_MAX_LENGTH &&
        memchr(path + 1, '/', code_len) == NULL) {
        char code[CODE_MAX_LENGTH + 1];
        memcpy(code, path + 1, code_len);
        code[code_len] = '\0';

        if (method_is(req, "GET") || head_only)
            handle_redirect(&ctx, code, out);
        else
            handle_method_not_allowed(&ctx, out);
        return rc;
    }

    handle_not_found(&ctx, out);
    return rc;
}
