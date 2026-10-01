/* epoll reactor: many connections, one thread.
 *
 * The alternative on this hardware is a thread per connection. At 500 idle
 * connections that is 500 stacks — about 4 MB of address space each, or 2 GB of
 * virtual memory the kernel has to map and the scheduler has to walk — to hold
 * 500 sockets that are waiting for a byte. Here a connection is two buffers and
 * an fd. That difference is why the architecture is an event loop at all, and it
 * is a consequence of having one core rather than a preference.
 *
 * The loop does three things and nothing else: accept, read, write. It never
 * blocks on a client and never calls into the database, which is what lets one
 * thread serve every connection. Task 7 hands slow work to worker threads for
 * exactly that reason.
 */
#ifndef NET_REACTOR_H
#define NET_REACTOR_H

#include "net/conn.h"

/* Return 0 to keep the connection open for another request, -1 to close it once
 * its queued output has drained. */
typedef int (*handler_fn)(conn_t *c, void *user_data);

typedef struct reactor reactor_t;

/* Binds and listens. Pass port 0 to let the kernel choose one, which is what the
 * tests do so they never collide with anything already listening; the port
 * actually bound is available from reactor_port. backlog is passed to listen().
 * Returns NULL on failure, and errno is left set. */
reactor_t *reactor_new(int port, int backlog);

/* Installs the callback for accepted connections. Must be set before
 * reactor_run. Returns 0, or -1 if NULL. */
int reactor_set_handler(reactor_t *r, handler_fn fn, void *user_data);

/* Runs until reactor_stop is called. Returns 0 on a clean stop, -1 on a fatal
 * accept or epoll error. Not reentrant: one reactor per thread. */
int reactor_run(reactor_t *r);

/* Asks the loop to return after the current iteration. Safe from a handler or
 * from another thread. */
void reactor_stop(reactor_t *r);

/* Closes every connection and releases the listening socket. Safe on NULL and
 * safe to call after reactor_run has returned. */
void reactor_free(reactor_t *r);

/* The port actually bound, which is how a caller using port 0 discovers it.
 * Returns -1 for NULL. */
int reactor_port(const reactor_t *r);

/* Connections currently open. The 500-connection measurement in Task 6 is this
 * number, so it is exposed rather than inferred. */
int reactor_conn_count(const reactor_t *r);

/* Runs one iteration of the loop, waiting at most timeout_ms. Returns the number
 * of events handled, 0 on timeout, or -1 on error. Exposed so a test can step
 * the reactor deterministically instead of racing a thread. */
int reactor_poll_once(reactor_t *r, int timeout_ms);

#endif /* NET_REACTOR_H */