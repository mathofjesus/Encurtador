#include "net/conn.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

void conn_init(conn_t *c, int fd)
{
    memset(c, 0, sizeof(*c));
    c->fd = fd;
    /* No buffer is allocated here. A connection that has been accepted but has
     * not spoken yet costs a struct and two null pointers, which is the entire
     * basis for holding hundreds of them on one core. */
}

void conn_reset(conn_t *c)
{
    int fd = c->fd;

    buf_free(c->in);
    buf_free(c->out);

    conn_init(c, fd);
}

size_t conn_pending(const conn_t *c)
{
    size_t total = c->out ? c->out->len : 0;
    return total > c->sent ? total - c->sent : 0;
}

int conn_flushed(const conn_t *c)
{
    return conn_pending(c) == 0;
}

int conn_needs_write_interest(const conn_t *c)
{
    return conn_pending(c) > 0 && !c->want_write;
}

int conn_input_overflowed(const conn_t *c)
{
    return c->in && c->in->len >= CONN_MAX_IN;
}

void conn_consume(conn_t *c, size_t n)
{
    if (!c->in)
        return;

    if (n >= c->in->len) {
        buf_reset(c->in);
        return;
    }

    /* memmove rather than an offset: the buffer is reused for the next request,
     * so compacting keeps one allocation for the life of the connection instead
     * of one per request. */
    memmove(c->in->data, c->in->data + n, c->in->len - n);
    c->in->len -= n;
}

/* Creates the input buffer if it does not exist yet. */
buf_t *conn_input(conn_t *c)
{
    if (!c->in)
        buf_new_into(&c->in, CONN_READ_CHUNK);
    return c->in;
}

int conn_flush(conn_t *c)
{
    while (conn_pending(c) > 0) {
        ssize_t n = send(c->fd, c->out->data + c->sent, conn_pending(c),
                         MSG_NOSIGNAL);
        if (n > 0) {
            c->sent += (size_t)n;
            c->bytes_out += (unsigned long long)n;
            continue;
        }

        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            /* The socket buffer is full. The rest stays queued and EPOLLOUT is
             * armed, which is the whole point of a non-blocking write. */
            return 0;
        }

        if (n < 0 && errno == EINTR)
            continue;

        /* EPIPE, ECONNRESET and anything else: the peer is gone or the socket is
         * broken, so there is nothing useful left to write to it. */
        return -1;
    }

    /* Everything went out, so the queue is dead weight. Releasing it here rather
     * than on the next request keeps a connection that served one small response
     * from holding that response's memory for its whole life.
     *
     * buf_free cannot clear the caller's pointer — it takes a buf_t *, not a
     * buf_t ** — so the assignment is not optional. Leaving it dangling makes
     * conn_pending read freed memory on the very next poll. */
    if (c->out) {
        buf_free(c->out);
        c->out = NULL;
        c->sent = 0;
    }
    return 0;
}

int conn_write(conn_t *c, const void *data, size_t len)
{
    if (len == 0)
        return 0;

    /* Append before flushing, so a partial write leaves the unwritten tail in
     * the queue rather than dropping it. */
    if (!c->out)
        buf_new_into(&c->out, CONN_READ_CHUNK);
    if (!c->out)
        return -1;

    if (buf_append(c->out, data, len) != 0)
        return -1;

    return conn_flush(c);
}

int conn_write_str(conn_t *c, const char *s)
{
    return conn_write(c, s, strlen(s));
}