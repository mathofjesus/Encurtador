#include "net/reactor.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

/* One entry per epoll event, sized for the largest burst epoll_wait is allowed
 * to return. Allocated once with the reactor rather than per call: on the hot
 * path a per-iteration malloc would be a measurable share of the work for a
 * loop whose whole purpose is to be cheap. */
#define REACTOR_MAX_EVENTS 256

/* How many connections are tracked before new ones are refused. A cap is what
 * stops one client from opening 10 million sockets and taking the process with
 * it; the number is a memory budget, not a tuning knob. */
#define REACTOR_MAX_CONNS 65536

typedef struct {
    conn_t conn;
    /* Non-zero while this slot holds a live connection registered with epoll.
     * A slot that is not watched must be ignored, not read: epoll can report an
     * fd that has since been closed and reused, and this is what tells the two
     * apart. It is also the only record that a slot is occupied, because fd is
     * left in conn.fd after conn_reset. */
    int watched;
} slot_t;

struct reactor {
    int listen_fd;
    int epoll_fd;
    int port;

    slot_t *slots;   /* indexed by fd */
    int slot_cap;
    int conn_count;

    handler_fn handler;
    void *user_data;

    int stop_requested;
};

/* Puts fd in non-blocking mode. Returns 0 or -1. Called for both the listening
 * socket and every accepted connection: a blocking accept in the loop would let
 * one client stall every other connection, which is the exact failure this
 * design exists to avoid. */
static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* Grows the fd-indexed table so that want_fd is addressable inside it.
 *
 * The parameter is the descriptor that must fit, not "a" descriptor. Guarding on
 * the listening socket instead meant the table stopped growing the moment it was
 * larger than the listener, and every connection accepted onto a higher fd was
 * closed on arrival — invisible at three connections and total at five hundred. */
static void ensure_slot_capacity(reactor_t *r, int want_fd)
{
    if (want_fd < r->slot_cap)
        return;

    int new_cap = r->slot_cap ? r->slot_cap * 2 : 64;
    while (new_cap <= want_fd) {
        if (new_cap > INT_MAX / 2)
            return; /* cannot grow further; the caller closes the fd */
        new_cap *= 2;
    }

    slot_t *slots = realloc(r->slots, (size_t)new_cap * sizeof(*slots));
    if (!slots) {
        /* Out of memory for the table. Refusing further connections is the only
         * safe response: continuing would index a NULL pointer. */
        return;
    }
    /* The new region must be zeroed or a connection could be "found" in it. */
    memset(slots + r->slot_cap, 0,
           (size_t)(new_cap - r->slot_cap) * sizeof(*slots));
    r->slots = slots;
    r->slot_cap = new_cap;
}

reactor_t *reactor_new(int port, int backlog)
{
    /* A peer that resets the connection must produce EPIPE from write(), not a
     * SIGPIPE that kills the process mid-request. Ignored globally rather than
     * per-thread because there is one reactor thread by construction. */
    signal(SIGPIPE, SIG_IGN);

    reactor_t *r = calloc(1, sizeof(*r));
    if (!r)
        return NULL;

    r->listen_fd = -1;
    r->epoll_fd = -1;

    r->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (r->epoll_fd < 0)
        goto fail;

    r->listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (r->listen_fd < 0)
        goto fail;

    /* Without SO_REUSEADDR a restart within a minute of a crash fails with
     * EADDRINUSE, because the old socket is still in TIME_WAIT. */
    int one = 1;
    if (setsockopt(r->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0)
        goto fail;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);

    if (bind(r->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        goto fail;

    if (listen(r->listen_fd, backlog > 0 ? backlog : 512) < 0)
        goto fail;

    if (set_nonblocking(r->listen_fd) != 0)
        goto fail;

    /* Read back what was actually bound. Port 0 asks the kernel to choose, and
     * the tests rely on learning the real port rather than assuming one. */
    socklen_t len = sizeof(addr);
    if (getsockname(r->listen_fd, (struct sockaddr *)&addr, &len) == 0)
        r->port = ntohs(addr.sin_port);

    ensure_slot_capacity(r, r->listen_fd);
    if (!r->slots)
        goto fail;

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    /* data.fd is the listening socket, distinguished by being the only fd that
     * is never a slot index. */
    ev.data.fd = r->listen_fd;
    if (epoll_ctl(r->epoll_fd, EPOLL_CTL_ADD, r->listen_fd, &ev) < 0)
        goto fail;

    return r;

fail:
    if (r->listen_fd >= 0)
        close(r->listen_fd);
    if (r->epoll_fd >= 0)
        close(r->epoll_fd);
    free(r->slots);
    free(r);
    return NULL;
}

int reactor_set_handler(reactor_t *r, handler_fn fn, void *user_data)
{
    if (!r || !fn)
        return -1;
    r->handler = fn;
    r->user_data = user_data;
    return 0;
}

int reactor_port(const reactor_t *r)
{
    return r ? r->port : -1;
}

int reactor_conn_count(const reactor_t *r)
{
    return r ? r->conn_count : 0;
}

static void close_slot(reactor_t *r, int fd)
{
    if (fd < 0 || fd >= r->slot_cap || !r->slots[fd].watched)
        return;

    epoll_ctl(r->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
    conn_reset(&r->slots[fd].conn);
    close(fd);
    r->slots[fd].watched = 0;
    r->conn_count--;
}

/* Registers interest in a socket that epoll has never seen. EPOLL_CTL_MOD on a
 * descriptor that was never added fails with ENOENT, which is how every accepted
 * connection ended up being closed the instant it arrived. */
static int watch(reactor_t *r, int fd, uint32_t events)
{
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = events;
    ev.data.fd = fd;
    return epoll_ctl(r->epoll_fd, EPOLL_CTL_ADD, fd, &ev);
}

/* Changes the interest set of a socket already registered with watch(). */
static int arm(reactor_t *r, int fd, uint32_t events)
{
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = events;
    ev.data.fd = fd;
    return epoll_ctl(r->epoll_fd, EPOLL_CTL_MOD, fd, &ev);
}

/* Accepts everything pending. Draining in a loop rather than accepting once per
 * iteration matters under load: one accept per epoll_wait would cap throughput
 * at one connection per loop turn even with a hundred queued. */
static void accept_pending(reactor_t *r)
{
    for (;;) {
        int fd = accept4(r->listen_fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            if (errno == EINTR || errno == ECONNABORTED)
                continue;
            /* EMFILE or ENFILE: the process is out of descriptors. Returning is
             * the only option — looping would spin at 100% CPU on a condition
             * that will not clear on its own. */
            return;
        }

        if (r->conn_count >= REACTOR_MAX_CONNS || fd >= REACTOR_MAX_CONNS) {
            close(fd);
            continue;
        }

        ensure_slot_capacity(r, fd);
        if (fd >= r->slot_cap) {
            /* The table could not be grown — almost certainly out of memory.
             * Refusing this connection is better than indexing past the array. */
            close(fd);
            continue;
        }

        /* Nagle would hold a small response back waiting for more data to batch
         * with. On a machine with one core, delaying a reply to save a packet is
         * the wrong trade every time. */
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        conn_init(&r->slots[fd].conn, fd);
        if (watch(r, fd, EPOLLIN | EPOLLRDHUP) != 0) {
            close(fd);
            continue;
        }
        r->slots[fd].watched = 1;
        r->conn_count++;
    }
}

/* Reads until the socket is drained or the cap is reached. Returns 0, -1 on a
 * dead socket, or 1 when the input cap was reached and the caller should close. */
static int read_available(reactor_t *r, int fd)
{
    conn_t *c = &r->slots[fd].conn;
    char chunk[CONN_READ_CHUNK];

    for (;;) {
        buf_t *in = conn_input(c);
        if (!in)
            return -1;

        /* The cap is enforced against the bytes held, not against the current
         * allocation. Reading into a stack chunk and appending lets the buffer
         * grow on its own; an earlier version derived the read size from the
         * buffer's free space and closed the connection as soon as the first
         * allocation filled, which is well below the cap. */
        if (in->len >= CONN_MAX_IN)
            return 1;

        size_t want = CONN_MAX_IN - in->len;
        if (want > sizeof(chunk))
            want = sizeof(chunk);

        ssize_t n = recv(fd, chunk, want, 0);
        if (n > 0) {
            if (buf_append(in, chunk, (size_t)n) != 0)
                return -1;
            c->bytes_in += (unsigned long long)n;
            /* buf_terminate writes a NUL past len so a handler can treat the
             * bytes as a string; it must not count toward the length. */
            buf_terminate(in);
            continue;
        }

        if (n == 0) {
            /* Orderly close. The peer may still have a queued request, so the
             * handler runs before the connection goes. */
            return 2;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;

        if (errno == EINTR)
            continue;

        return -1;
    }
}

static int write_ready(reactor_t *r, int fd)
{
    conn_t *c = &r->slots[fd].conn;

    if (conn_flush(c) != 0)
        return -1;

    if (conn_flushed(c) && c->want_write) {
        c->want_write = 0;
        /* Drop EPOLLOUT, do not merely forget it. epoll is level-triggered, so a
         * writable socket that still has EPOLLOUT registered reports ready on
         * every single wait: the loop stops blocking entirely and burns a core
         * doing nothing. Forgetting the flag without re-arming produces exactly
         * that spin. */
        return arm(r, fd, EPOLLIN | EPOLLRDHUP);
    }
    return 0;
}

int reactor_poll_once(reactor_t *r, int timeout_ms)
{
    if (!r)
        return -1;

    struct epoll_event events[REACTOR_MAX_EVENTS];
    int n = epoll_wait(r->epoll_fd, events, REACTOR_MAX_EVENTS, timeout_ms);
    if (n < 0)
        return (errno == EINTR) ? 0 : -1;

    for (int i = 0; i < n; i++) {
        int fd = events[i].data.fd;
        uint32_t ev = events[i].events;

        if (fd == r->listen_fd) {
            accept_pending(r);
            continue;
        }

        if (fd < 0 || fd >= r->slot_cap || !r->slots[fd].watched)
            continue;

        conn_t *c = &r->slots[fd].conn;

        if (ev & (EPOLLERR | EPOLLHUP)) {
            /* Nothing can be done with this socket; EPOLLERR does not say what
             * went wrong, only that something did. */
            close_slot(r, fd);
            continue;
        }

        int read_rc = 0;
        if (ev & (EPOLLIN | EPOLLRDHUP)) {
            read_rc = read_available(r, fd);
            if (read_rc == -1 || read_rc == 1) {
                close_slot(r, fd);
                continue;
            }
        }

        if (ev & EPOLLOUT) {
            if (write_ready(r, fd) != 0) {
                close_slot(r, fd);
                continue;
            }
        }

        if (c->in && c->in->len > 0 && r->handler) {
            if (r->handler(c, r->user_data) < 0)
                c->want_close = 1;
        } else if (read_rc == 2) {
            /* Peer sent FIN and there is nothing left to answer. */
            c->want_close = 1;
        }

        /* Arm EPOLLOUT only while bytes are actually queued. */
        if (conn_pending(c) > 0 && !c->want_write) {
            c->want_write = 1;
            if (arm(r, fd, EPOLLIN | EPOLLOUT | EPOLLRDHUP) != 0) {
                close_slot(r, fd);
                continue;
            }
        }

        /* Close only after the response has drained. Closing while bytes are
         * still queued resets the connection and the client sees a reset instead
         * of the answer it was sent.
         *
         * want_close lives on the connection, not in a local of the iteration
         * that ran the handler. A handler that asks to close and produces more
         * output than one write can take returns on an iteration where the
         * response has not drained; if the request were only a local, this test
         * would never run again and the descriptor would leak. */
        if (c->want_close && conn_flushed(c)) {
            close_slot(r, fd);
            continue;
        }
    }

    return n;
}

int reactor_run(reactor_t *r)
{
    if (!r)
        return -1;

    while (!r->stop_requested) {
        /* A finite timeout rather than -1: the loop must notice stop_requested
         * and must not block forever in epoll_wait with a connection that will
         * never produce another event. */
        if (reactor_poll_once(r, 200) < 0)
            return -1;
    }
    return 0;
}

void reactor_stop(reactor_t *r)
{
    if (r)
        r->stop_requested = 1;
}

void reactor_free(reactor_t *r)
{
    if (!r)
        return;

    for (int fd = 0; fd < r->slot_cap; fd++)
        if (r->slots[fd].watched)
            close_slot(r, fd);

    if (r->listen_fd >= 0)
        close(r->listen_fd);
    if (r->epoll_fd >= 0)
        close(r->epoll_fd);

    free(r->slots);
    free(r);
}