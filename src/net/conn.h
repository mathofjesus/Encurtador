/* One client connection, from the reactor's point of view.
 *
 * A connection owns two buffers and nothing else. Everything about it — the
 * request bytes read so far, the response bytes not yet written — is bytes in
 * a buffer, which is what makes 500 of them cheap: no per-connection thread, no
 * per-connection stack, and a connection that has said nothing costs two
 * unallocated pointers.
 */
#ifndef NET_CONN_H
#define NET_CONN_H

#include <stddef.h>

#include "util/buf.h"

/* How much is read from the socket in one call. Large enough that a typical
 * request arrives in one read, small enough that one connection cannot make the
 * others wait for a large transfer. */
#define CONN_READ_CHUNK 16384

/* Beyond this the peer is either not speaking HTTP or is trying to exhaust
 * memory, and the connection is closed instead. A request line plus headers is
 * a few hundred bytes; HTTP_MAX_BODY is 8192, so this leaves room and still
 * refuses anything unbounded. */
#define CONN_MAX_IN (64 * 1024)

typedef struct {
    int fd;

    /* Bytes read from the socket and not yet consumed by the handler. NULL until
     * the first byte arrives, which is what makes an idle connection free: it
     * holds no allocation at all, not an empty one. */
    buf_t *in;
    /* Response bytes waiting to go out, with `sent` counting how many have
     * already been written, because a partial write is normal and the unwritten
     * tail must stay queued until the socket is ready. */
    buf_t *out;
    size_t sent;

    /* Set by the handler to close after the pending output has drained.
     * Closing immediately would truncate the response the client is waiting
     * for, which is the classic way a "connection reset" appears in a log with
     * no cause. */
    int want_close;

    /* Whether EPOLLOUT is currently registered. Tracked so the reactor can be
     * told about the transition instead of re-arming on every event. */
    int want_write;

    /* Monotonic count of bytes read, for metrics and for the per-connection
     * figure Task 6 records. */
    unsigned long long bytes_in;
    unsigned long long bytes_out;
} conn_t;

void conn_init(conn_t *c, int fd);
void conn_reset(conn_t *c);

/* Queues len bytes for writing and tries to write them immediately. Returns 0
 * when everything is queued, or -1 on a fatal socket error. Partial writes are
 * normal and leave the remainder queued with c->want_write set. */
int conn_write(conn_t *c, const void *data, size_t len);

int conn_write_str(conn_t *c, const char *s);

/* Drops n bytes from the front of the input buffer. The handler says how much of
 * what it was given it has finished with, which is what lets several pipelined
 * requests share one connection: each consumes its own bytes and the next
 * request is then at the front of the buffer. */
void conn_consume(conn_t *c, size_t n);

/* The input buffer, allocated on first use. NULL if allocation failed. */
buf_t *conn_input(conn_t *c);

/* Bytes still queued for writing. */
size_t conn_pending(const conn_t *c);

/* Writes as much queued output as the socket will accept, leaving the rest
 * queued and the caller responsible for arming EPOLLOUT. Returns 0, or -1 when
 * the socket is dead and the connection must be closed. */
int conn_flush(conn_t *c);

/* True once everything queued has been written, which is when a connection
 * marked want_close may actually be closed. */
int conn_flushed(const conn_t *c);

/* Whether EPOLLOUT interest should be on: there is queued output and the socket
 * is not already interested. Returns non-zero when the caller must update
 * epoll. */
int conn_needs_write_interest(const conn_t *c);

/* True when the incoming buffer is at its cap and no more will be read. */
int conn_input_overflowed(const conn_t *c);

#endif /* NET_CONN_H */