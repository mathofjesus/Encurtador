/* Request routing.
 *
 * The router decides which handler runs, whether a write is within the client's
 * rate budget, and whether the connection stays open. It owns no storage and no
 * sockets, so it can be driven directly from a test with a hand-built request.
 *
 * It is synchronous. A database-backed route blocks the calling thread for the
 * duration of its query, which on one core means a slow query delays every
 * other connection. Threading the storage calls off the reactor is a wiring
 * concern for main; these rules are unchanged by where they run.
 */
#ifndef API_ROUTER_H
#define API_ROUTER_H

#include "config.h"
#include "http/http.h"
#include "observability/metrics.h"
#include "services/rate_limit.h"
#include "services/url_service.h"
#include "util/buf.h"

typedef struct router router_t;

/* svc and cfg are required. rl may be NULL to disable rate limiting, and
 * metrics may be NULL to disable counting. None are owned by the router. */
router_t *router_new(url_service_t *svc, rl_t *rl, const config_t *cfg,
                     metrics_t *metrics);

/* Safe on NULL. */
void router_free(router_t *r);

/* Dispatches one complete request, appending a full HTTP/1.1 response to out.
 *
 * client_ip identifies the caller for rate limiting and may be NULL, in which
 * case the limiter is skipped: an unknown client is not one to spend a bucket
 * on.
 *
 * Returns 0 to keep the connection alive for another request, or -1 to close it
 * once the response has been written. This matches the reactor's handler
 * contract so main can install the router with no adapter. */
int router_dispatch(router_t *r, const http_request_t *req, const char *client_ip,
                    buf_t *out);

#endif /* API_ROUTER_H */
