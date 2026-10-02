/* Route handlers.
 *
 * Each handler turns one already-routed request into one response. They do not
 * decide which route applies and they do not rate limit; that is the router's
 * job. Keeping the split means a handler is a function of (context, input,
 * output) and can be tested without a socket.
 */
#ifndef API_HANDLERS_H
#define API_HANDLERS_H

#include "config.h"
#include "http/http.h"
#include "observability/metrics.h"
#include "services/url_service.h"
#include "util/buf.h"

typedef struct {
    url_service_t *svc;
    const config_t *cfg;
    metrics_t *metrics; /* may be NULL */
    int keep_alive;     /* copied from the request, drives the Connection header */
    int head_only;      /* HEAD: real Content-Length, no body */
} handler_ctx_t;

/* POST /api/v1/shorten */
void handle_shorten(const handler_ctx_t *ctx, const http_request_t *req, buf_t *out);

/* GET or HEAD /{code} */
void handle_redirect(const handler_ctx_t *ctx, const char *code, buf_t *out);

/* GET or HEAD /health */
void handle_health(const handler_ctx_t *ctx, buf_t *out);

/* GET or HEAD /metrics */
void handle_metrics(const handler_ctx_t *ctx, buf_t *out);

/* A known path reached with a method it does not accept. */
void handle_method_not_allowed(const handler_ctx_t *ctx, buf_t *out);

/* The rate limiter refused the request. extra_headers carries Retry-After. */
void handle_rate_limited(const handler_ctx_t *ctx, buf_t *out,
                         const char *extra_headers);

/* No route matched. */
void handle_not_found(const handler_ctx_t *ctx, buf_t *out);

#endif /* API_HANDLERS_H */
