/* The assembled service: storage, cache, limiter, router and logging wired
 * together, plus the reactor callback that turns bytes into requests.
 *
 * This is the seam between the event loop and the routing rules. main owns a
 * config and a log and hands them here; a test can do the same and then step a
 * reactor by hand, which is how the e2e suite exercises the real stack without a
 * subprocess.
 */
#ifndef API_APPLICATION_H
#define API_APPLICATION_H

#include "config.h"
#include "net/conn.h"
#include "observability/log.h"

typedef struct application application_t;

/* Builds the pool, applies the schema, connects the cache, and constructs the
 * limiter and router. cfg is copied, so the caller's copy may go away; log is
 * borrowed and may be NULL (logging is then off).
 *
 * A cache that will not connect is a warning, not a startup failure: the
 * service is defined to work with a NULL cache, degraded to every read hitting
 * the database. A pool that will not connect is fatal, because there is then
 * nothing to serve. Returns NULL on failure. */
application_t *application_new(const config_t *cfg, log_t *log);

/* Safe on NULL. Frees what application_new built, never cfg or log. */
void application_free(application_t *a);

/* reactor handler_fn. Parses one request from a->in and dispatches it through the
 * router, logging the outcome. Returns 0 to keep the connection open, -1 to
 * close it once the response drains. */
int application_handle_conn(conn_t *c, void *user_data);

#endif /* API_APPLICATION_H */
