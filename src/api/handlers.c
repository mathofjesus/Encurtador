#include "api/handlers.h"

#include <stdio.h>
#include <string.h>

#include "codegen.h"
#include "util/json.h"

/* One place that writes a response, so the metrics side effect cannot be
 * forgotten on one branch and remembered on another. 5xx counts separately from
 * 4xx because "the client sent something odd" and "we are broken" are different
 * alarms. */
static void send(const handler_ctx_t *ctx, buf_t *out, int status,
                 const char *content_type, const char *body,
                 const char *location, const char *extra_headers)
{
    if (ctx->metrics) {
        if (status >= 500)
            metrics_inc(ctx->metrics, METRIC_SERVER_ERROR_TOTAL);
        else if (status >= 400)
            metrics_inc(ctx->metrics, METRIC_CLIENT_ERROR_TOTAL);
    }

    http_response_write(out, status, content_type, body, location,
                        extra_headers, ctx->keep_alive, ctx->head_only);
}

void handle_not_found(const handler_ctx_t *ctx, buf_t *out)
{
    send(ctx, out, 404, "application/json", "{\"error\":\"not found\"}\n",
         NULL, NULL);
}

void handle_method_not_allowed(const handler_ctx_t *ctx, buf_t *out)
{
    /* A generic Allow is enough while every route accepts a subset of the same
     * three methods. A route that diverges should grow its own. */
    send(ctx, out, 405, "application/json",
         "{\"error\":\"method not allowed\"}\n", NULL,
         "Allow: GET, HEAD, POST\r\n");
}

void handle_rate_limited(const handler_ctx_t *ctx, buf_t *out,
                         const char *extra_headers)
{
    send(ctx, out, 429, "application/json", "{\"error\":\"rate limited\"}\n",
         NULL, extra_headers);
}

void handle_health(const handler_ctx_t *ctx, buf_t *out)
{
    /* Deliberately does not touch Postgres or Redis. A health check that fails
     * when storage is merely slow turns a slow database into a restart loop,
     * which makes the outage worse. */
    send(ctx, out, 200, "text/plain; charset=utf-8", "ok\n", NULL, NULL);
}

void handle_metrics(const handler_ctx_t *ctx, buf_t *out)
{
    buf_t *body = buf_new(512);
    if (!body) {
        send(ctx, out, 500, "application/json",
             "{\"error\":\"out of memory\"}\n", NULL, NULL);
        return;
    }

    metrics_render(ctx->metrics, body);
    send(ctx, out, 200, "text/plain; version=0.0.4; charset=utf-8",
         body->data ? body->data : "", NULL, NULL);

    buf_free(body);
}

void handle_redirect(const handler_ctx_t *ctx, const char *code, buf_t *out)
{
    char url[PG_MAX_URL + 1];
    time_t expires = 0;
    int rc = url_service_lookup(ctx->svc, code, url, sizeof(url), &expires);

    switch (rc) {
    case URL_OK:
        if (ctx->metrics)
            metrics_inc(ctx->metrics, METRIC_REDIRECT_TOTAL);
        /* 301, not 302: the mapping is permanent, so a client may cache it.
         * That caching is most of what a short link is for. */
        send(ctx, out, 301, NULL, NULL, url, NULL);
        return;

    case URL_INVALID:
    case URL_NOT_FOUND:
        if (ctx->metrics)
            metrics_inc(ctx->metrics, METRIC_REDIRECT_MISS_TOTAL);
        /* A path that could never be a code is a miss, not a client error.
         * /favicon.ico asks for something that is not there; answering 400
         * would call every browser automatic request a bad request. */
        send(ctx, out, 404, "application/json", "{\"error\":\"not found\"}\n",
             NULL, NULL);
        return;

    default:
        send(ctx, out, 503, "application/json",
             "{\"error\":\"storage unavailable\"}\n", NULL,
             "Retry-After: 1\r\n");
        return;
    }
}

void handle_shorten(const handler_ctx_t *ctx, const http_request_t *req, buf_t *out)
{
    json_obj_t *o = json_parse(req->body, req->body_len);
    if (!o) {
        send(ctx, out, 400, "application/json",
             "{\"error\":\"body is not a JSON object\"}\n", NULL, NULL);
        return;
    }

    const char *url = json_get_str(o, "url");
    if (!url || !*url) {
        json_free(o);
        send(ctx, out, 400, "application/json",
             "{\"error\":\"url is required\"}\n", NULL, NULL);
        return;
    }

    /* Absent "custom_code" and absent "ttl_days" are both normal: a generated
     * code with the configured default lifetime. */
    const char *custom = json_get_str(o, "custom_code");
    long ttl = 0;
    int ttl_days = (json_get_int(o, "ttl_days", &ttl) == 0) ? (int)ttl : 0;

    char code[CODE_MAX_LENGTH + 1];
    int rc = url_service_create(ctx->svc, url, custom, ttl_days, code,
                                sizeof(code));
    json_free(o);

    switch (rc) {
    case URL_OK: {
        if (ctx->metrics)
            metrics_inc(ctx->metrics, METRIC_SHORTEN_TOTAL);

        /* A trailing slash in base_url would produce "https://x//abc". */
        char base[sizeof(ctx->cfg->app.base_url)];
        snprintf(base, sizeof(base), "%s", ctx->cfg->app.base_url);
        size_t bl = strlen(base);
        while (bl > 0 && base[bl - 1] == '/')
            base[--bl] = '\0';

        char location[sizeof(base) + CODE_MAX_LENGTH + 2];
        snprintf(location, sizeof(location), "%s/%s", base, code);

        char body[sizeof(base) + CODE_MAX_LENGTH + 64];
        snprintf(body, sizeof(body), "{\"code\":\"%s\",\"short_url\":\"%s\"}\n",
                 code, location);

        /* Location names the resource just created; the body repeats it for
         * clients that do not follow it. */
        send(ctx, out, 201, "application/json", body, location, NULL);
        return;
    }

    case URL_CONFLICT:
        send(ctx, out, 409, "application/json",
             "{\"error\":\"custom code already in use\"}\n", NULL, NULL);
        return;

    case URL_INVALID:
        send(ctx, out, 400, "application/json",
             "{\"error\":\"invalid url or custom code\"}\n", NULL, NULL);
        return;

    default:
        send(ctx, out, 503, "application/json",
             "{\"error\":\"storage unavailable\"}\n", NULL,
             "Retry-After: 1\r\n");
        return;
    }
}
