/* Reactor tests.
 *
 * The reactor is stepped with reactor_poll_once rather than run on a thread.
 * A threaded test would be a race: the assertion could pass or fail depending on
 * whether the reactor happened to run before the client's connect returned.
 * Stepping it means every assertion is about what the loop did, not about when.
 */
#include <arpa/inet.h>
#include <criterion/criterion.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "net/reactor.h"

#define BACKLOG 512
#define SIMPLE_RESPONSE "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi"

/* Far larger than any socket buffer, so send() must return short many times over
 * and the remainder has to stay queued. */
#define BIG_BODY_SIZE 300000

static reactor_t *fresh_reactor(void)
{
    reactor_t *r = reactor_new(0, BACKLOG);
    cr_assert_not_null(r, "reactor_new failed: %s", strerror(errno));
    cr_assert_gt(reactor_port(r), 0,
                 "port 0 should have been replaced by a real bound port");
    return r;
}

/* Connects with an explicitly small receive buffer when rcvbuf > 0.
 *
 * The receive buffer matters to the tests that need a write to be partial. Left
 * alone, TCP autotunes the buffers into the megabytes and an 8 MB response can be
 * absorbed whole, so the queued-output path the test is about never runs. It was
 * exactly this that made an earlier version of these tests pass with the bugs
 * reintroduced. Pinning SO_RCVBUF keeps the window small and EAGAIN certain. */
static int connect_client_buf(int port, int rcvbuf)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    cr_assert_geq(fd, 0);

    if (rcvbuf > 0) {
        int v = rcvbuf;
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &v, sizeof(v));
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    cr_assert_eq(connect(fd, (struct sockaddr *)&addr, sizeof(addr)), 0,
                 "connect failed: %s", strerror(errno));
    return fd;
}

/* A blocking client socket. The connect succeeds from the kernel's backlog even
 * before the reactor accepts, which is what lets a test queue a connection
 * before stepping the loop. */
static int connect_client(int port)
{
    return connect_client_buf(port, 0);
}

/* Sends a whole request. The length is always strlen, never a hardcoded count:
 * a count one byte short silently truncates the request, and the test then waits
 * out its entire poll budget for an answer to a request that was never complete.
 * Two tests did exactly that, which is why the missing final "\n" was invisible.
 */
static void send_request(int fd, const char *request)
{
    size_t len = strlen(request);
    cr_assert_eq(send(fd, request, len, 0), (ssize_t)len,
                 "short send of %zu bytes", len);
}

/* Reads whatever is available without blocking. */
static size_t drain(int fd, buf_t *into, size_t budget)
{
    char tmp[16384];
    size_t total = 0;

    while (total < budget) {
        ssize_t n = recv(fd, tmp, sizeof(tmp), MSG_DONTWAIT);
        if (n > 0) {
            buf_append(into, tmp, (size_t)n);
            total += (size_t)n;
            continue;
        }
        break;
    }
    return total;
}

static int has_enough_conns(reactor_t *r, void *ud)
{
    int *target = ud;
    return reactor_conn_count(r) >= *target;
}

/* Steps until pred holds. Returns the iteration count, or -1 if it never did. */
static int step_until(reactor_t *r, int (*pred)(reactor_t *, void *), void *ud,
                      int max_iterations)
{
    for (int i = 0; i < max_iterations; i++) {
        if (pred(r, ud))
            return i;
        reactor_poll_once(r, 10);
    }
    return pred(r, ud) ? max_iterations : -1;
}

/* Steps the loop until the client has received something, draining between
 * polls. Draining is not optional: the predicate is about bytes the *client* has
 * read, and no amount of polling by the server moves bytes into the client's
 * buffer. An earlier version polled without draining and waited out its whole
 * budget on a connection that had already been answered. */
static int wait_for_response(reactor_t *r, int client_fd, buf_t *got,
                             int max_iterations)
{
    for (int i = 0; i < max_iterations; i++) {
        reactor_poll_once(r, 10);
        drain(client_fd, got, 1 << 16);
        if (buf_len(got) > 0)
            return i;
    }
    return -1;
}

/* Answers once the blank line ending the headers has arrived. */
static int echo_handler(conn_t *c, void *ud)
{
    int *answered = ud;

    if (!c->in || c->in->len == 0)
        return 0;

    /* The reactor NUL-terminates the input buffer, so it can be searched as a
     * string. An incomplete request is left for the next event. */
    if (strstr(c->in->data, "\r\n\r\n") == NULL)
        return 0;

    conn_write_str(c, SIMPLE_RESPONSE);
    conn_consume(c, c->in->len);
    if (answered)
        (*answered)++;
    return 0;
}

/* Answers, then asks to be closed. */
static int close_handler(conn_t *c, void *ud)
{
    int *calls = ud;

    if (!c->in || c->in->len == 0)
        return 0;
    if (strstr(c->in->data, "\r\n\r\n") == NULL)
        return 0;

    conn_write_str(c, "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n");
    conn_consume(c, c->in->len);
    if (calls)
        (*calls)++;
    return -1; /* close once the response has drained */
}

static char big_body[BIG_BODY_SIZE];
static int big_body_built;

static int big_handler(conn_t *c, void *ud)
{
    (void)ud;

    if (!c->in || c->in->len == 0)
        return 0;
    if (strstr(c->in->data, "\r\n\r\n") == NULL)
        return 0;

    if (!big_body_built) {
        for (size_t i = 0; i < sizeof(big_body); i++)
            big_body[i] = (char)('a' + (i % 26));
        big_body_built = 1;
    }

    char header[128];
    int n = snprintf(header, sizeof(header),
                     "HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n",
                     BIG_BODY_SIZE);
    conn_write(c, header, (size_t)n);
    conn_write(c, big_body, sizeof(big_body));
    conn_consume(c, c->in->len);
    return -1;
}

/* A body larger than the socket buffers can possibly hold, so one conn_write
 * cannot complete and the queued-output path is guaranteed to run. The sum of the
 * largest possible send and receive buffers is about 4 MB on this kernel
 * (tcp_wmem caps the send buffer at 4 MB, and the client pins SO_RCVBUF to
 * 32 KB); 8 MB therefore cannot be buffered whole.
 *
 * The handlers record whether the write really was partial. Without that, the
 * tests below silently become no-ops the day the buffers are large enough — which
 * is precisely what happened once already. */
#define PARTIAL_BODY_SIZE (8u * 1024 * 1024)
static char partial_body[PARTIAL_BODY_SIZE];
static int partial_body_built;

static void build_partial_body(void)
{
    if (partial_body_built)
        return;
    for (size_t i = 0; i < sizeof(partial_body); i++)
        partial_body[i] = (char)('a' + (i % 26));
    partial_body_built = 1;
}

static int request_is_complete(conn_t *c)
{
    return c->in && c->in->len > 0 && strstr(c->in->data, "\r\n\r\n") != NULL;
}

/* Returns 0 to stay open, or -1 to close once drained. */
static int write_partial_response(conn_t *c, int *saw_partial, int close_after)
{
    build_partial_body();

    char header[128];
    int n = snprintf(header, sizeof(header),
                     "HTTP/1.1 200 OK\r\nContent-Length: %u\r\n\r\n",
                     (unsigned)PARTIAL_BODY_SIZE);
    conn_write(c, header, (size_t)n);
    conn_write(c, partial_body, sizeof(partial_body));

    if (conn_pending(c) > 0)
        *saw_partial = 1;

    conn_consume(c, c->in->len);
    return close_after ? -1 : 0;
}

static int partial_keepalive_handler(conn_t *c, void *ud)
{
    if (!request_is_complete(c))
        return 0;
    return write_partial_response(c, ud, 0);
}

static int partial_closing_handler(conn_t *c, void *ud)
{
    if (!request_is_complete(c))
        return 0;
    return write_partial_response(c, ud, 1);
}

Test(reactor, serves_a_request_on_an_ephemeral_port)
{
    reactor_t *r = fresh_reactor();
    int answered = 0;
    cr_assert_eq(reactor_set_handler(r, echo_handler, &answered), 0);

    int client = connect_client(reactor_port(r));
    buf_t *got;
    buf_new_into(&got, 256);
    cr_assert_not_null(got);

    send_request(client, "GET / HTTP/1.1\r\nHost: x\r\n\r\n");

    cr_assert_geq(wait_for_response(r, client, got, 400), 0,
                  "the reactor never answered");

    buf_terminate(got);
    cr_assert_str_eq(got->data, SIMPLE_RESPONSE);
    cr_assert_eq(answered, 1, "the handler should have run exactly once");

    buf_free(got);
    close(client);
    reactor_free(r);
}

Test(reactor, a_request_delivered_one_byte_at_a_time_still_completes)
{
    /* The point of an incremental parser feeding an event loop: a client that
     * dribbles its request across many packets, with the loop idle in between,
     * must get the same answer as one that sends it in a single write. */
    reactor_t *r = fresh_reactor();
    int answered = 0;
    cr_assert_eq(reactor_set_handler(r, echo_handler, &answered), 0);

    int client = connect_client(reactor_port(r));
    buf_t *got;
    buf_new_into(&got, 256);
    cr_assert_not_null(got);

    const char *request = "GET /a HTTP/1.1\r\nHost: x\r\n\r\n";
    size_t request_len = strlen(request);

    for (size_t i = 0; i < request_len; i++) {
        cr_assert_eq(send(client, &request[i], 1, 0), 1, "failed at byte %zu", i);

        reactor_poll_once(r, 5);
        drain(client, got, 64);

        if (i + 1 < request_len) {
            /* Answering here would mean treating a partial request as a whole
             * one. */
            cr_assert_eq(buf_len(got), 0, "answered after %zu of %zu bytes",
                         i + 1, request_len);
            cr_assert_eq(answered, 0);
        }
    }

    cr_assert_geq(wait_for_response(r, client, got, 400), 0);
    cr_assert_eq(answered, 1);

    buf_terminate(got);
    cr_assert_str_eq(got->data, SIMPLE_RESPONSE);

    buf_free(got);
    close(client);
    reactor_free(r);
}

Test(reactor, two_clients_are_served_from_one_loop)
{
    /* One thread, two sockets, interleaved. The alternative model needs two
     * threads here, which is the whole argument for this one. */
    reactor_t *r = fresh_reactor();
    int answered = 0;
    cr_assert_eq(reactor_set_handler(r, echo_handler, &answered), 0);

    int a = connect_client(reactor_port(r));
    int b = connect_client(reactor_port(r));

    send_request(a, "GET /a HTTP/1.1\r\n\r\n");
    send_request(b, "GET /b HTTP/1.1\r\n\r\n");

    buf_t *got_a, *got_b;
    buf_new_into(&got_a, 256);
    buf_new_into(&got_b, 256);

    for (int i = 0; i < 600; i++) {
        reactor_poll_once(r, 10);
        drain(a, got_a, 256);
        drain(b, got_b, 256);
        if (buf_len(got_a) > 0 && buf_len(got_b) > 0)
            break;
    }

    cr_assert_gt(buf_len(got_a), 0, "the first client was never answered");
    cr_assert_gt(buf_len(got_b), 0, "the second client was never answered");
    cr_assert_eq(answered, 2);

    buf_free(got_a);
    buf_free(got_b);
    close(a);
    close(b);
    reactor_free(r);
}

Test(reactor, a_large_response_arrives_whole_despite_partial_writes)
{
    /* Byte-for-byte integrity of a response larger than one read: a bug here
     * loses or duplicates bytes, and a truncated body is worse than an error.
     * Whether the write is genuinely partial at this size is not guaranteed —
     * the two tests below pin the queued-output path with a body the buffers
     * cannot hold. */
    reactor_t *r = fresh_reactor();
    cr_assert_eq(reactor_set_handler(r, big_handler, NULL), 0);

    int client = connect_client(reactor_port(r));
    send_request(client, "GET /big HTTP/1.1\r\n\r\n");

    buf_t *got;
    buf_new_into(&got, 1 << 16);
    cr_assert_not_null(got);

    for (int i = 0; i < 4000; i++) {
        drain(client, got, 1 << 16);
        if (buf_len(got) >= (size_t)BIG_BODY_SIZE + 64)
            break;
        reactor_poll_once(r, 5);
    }

    buf_terminate(got);
    cr_assert_gt(buf_len(got), (size_t)BIG_BODY_SIZE,
                 "only %zu of %d body bytes arrived", buf_len(got), BIG_BODY_SIZE);

    const char *split = strstr(got->data, "\r\n\r\n");
    cr_assert_not_null(split, "no header terminator in the response");
    size_t header_len = (size_t)(split - got->data) + 4;
    size_t body_len = buf_len(got) - header_len;

    cr_assert_eq(body_len, (size_t)BIG_BODY_SIZE,
                 "body is %zu bytes, Content-Length said %d", body_len,
                 BIG_BODY_SIZE);
    cr_assert_eq(memcmp(got->data + header_len, big_body, BIG_BODY_SIZE), 0,
                 "the body was corrupted in transit");

    buf_free(got);
    close(client);
    reactor_free(r);
}

Test(reactor, a_flushed_keep_alive_connection_lets_the_loop_block)
{
    /* A stale EPOLLOUT does not corrupt anything: the bytes still arrive, so a
     * functional assertion cannot see it. It shows up only as a loop that never
     * blocks. So this waits for a genuinely partial response to finish and then
     * requires a wait to time out. */
    int saw_partial = 0;
    reactor_t *r = fresh_reactor();
    cr_assert_eq(reactor_set_handler(r, partial_keepalive_handler, &saw_partial), 0);

    int client = connect_client_buf(reactor_port(r), 32768);
    send_request(client, "GET /big HTTP/1.1\r\n\r\n");

    buf_t *got;
    buf_new_into(&got, 1 << 16);
    cr_assert_not_null(got);

    for (int i = 0; i < 6000 && buf_len(got) <= (size_t)PARTIAL_BODY_SIZE; i++) {
        reactor_poll_once(r, 5);
        drain(client, got, 1 << 16);
    }
    cr_assert_eq(saw_partial, 1,
                 "the response was not written partially, so this test proves "
                 "nothing about the queued-output path");
    cr_assert_gt(buf_len(got), (size_t)PARTIAL_BODY_SIZE);

    int quiet = 0;
    for (int i = 0; i < 200 && !quiet; i++)
        quiet = reactor_poll_once(r, 10) == 0;

    cr_assert_eq(reactor_conn_count(r), 1,
                 "a keep-alive connection should not have been closed");
    cr_assert_eq(quiet, 1,
                 "the loop never blocked: EPOLLOUT is still armed on a socket "
                 "with nothing left to write");

    buf_free(got);
    close(client);
    reactor_free(r);
}

Test(reactor, a_partially_written_response_is_closed_after_it_drains)
{
    /* The handler asks to close before the 8 MB has gone out. If that request is
     * remembered only for the iteration that ran the handler, nothing ever runs
     * the check again and the descriptor leaks — invisible until the process runs
     * out of them. */
    int saw_partial = 0;
    reactor_t *r = fresh_reactor();
    cr_assert_eq(reactor_set_handler(r, partial_closing_handler, &saw_partial), 0);

    int client = connect_client_buf(reactor_port(r), 32768);
    send_request(client, "GET /big HTTP/1.1\r\n\r\n");

    buf_t *got;
    buf_new_into(&got, 1 << 16);
    cr_assert_not_null(got);

    for (int i = 0; i < 6000 && buf_len(got) <= (size_t)PARTIAL_BODY_SIZE; i++) {
        reactor_poll_once(r, 5);
        drain(client, got, 1 << 16);
    }
    cr_assert_eq(saw_partial, 1,
                 "the response was not written partially, so this test proves "
                 "nothing about the queued-output path");
    cr_assert_gt(buf_len(got), (size_t)PARTIAL_BODY_SIZE);

    for (int i = 0; i < 100 && reactor_conn_count(r) > 0; i++)
        reactor_poll_once(r, 10);

    cr_assert_eq(reactor_conn_count(r), 0,
                 "a connection told to close never closed after its response drained");
    cr_assert_eq(reactor_poll_once(r, 50), 0,
                 "the loop is still waking on a descriptor that is gone");

    buf_free(got);
    close(client);
    reactor_free(r);
}

Test(reactor, a_handler_returning_minus_one_closes_the_connection_after_draining)
{
    reactor_t *r = fresh_reactor();
    int calls = 0;
    cr_assert_eq(reactor_set_handler(r, close_handler, &calls), 0);

    int client = connect_client(reactor_port(r));
    send_request(client, "GET / HTTP/1.1\r\n\r\n");

    buf_t *got;
    buf_new_into(&got, 256);
    cr_assert_not_null(got);

    for (int i = 0; i < 600; i++) {
        reactor_poll_once(r, 10);
        drain(client, got, 256);
        if (reactor_conn_count(r) == 0 && buf_len(got) > 0)
            break;
    }

    cr_assert_eq(calls, 1);
    cr_assert_eq(reactor_conn_count(r), 0, "the connection should have been closed");
    /* Closed after the response went out, not before: a close that beat the
     * write would show up on the client as a reset. */
    cr_assert_gt(buf_len(got), 0, "the client got nothing before the close");

    buf_free(got);
    close(client);
    reactor_free(r);
}

/* Resident set size in bytes, from /proc. Returns 0 if it cannot be read, which
 * the caller treats as "not measured" rather than as zero usage. */
static size_t rss_bytes(void)
{
    FILE *f = fopen("/proc/self/statm", "r");
    if (!f)
        return 0;

    long total_pages = 0, resident_pages = 0;
    int got = fscanf(f, "%ld %ld", &total_pages, &resident_pages);
    fclose(f);
    if (got != 2 || resident_pages <= 0)
        return 0;

    return (size_t)resident_pages * (size_t)sysconf(_SC_PAGESIZE);
}

Test(reactor, five_hundred_idle_connections_cost_a_few_megabytes)
{
    /* Task 6's central measurement: 500 connections that have said nothing must
     * be accepted and held. A thread-per-connection design would need 500 stacks
     * at roughly 8 MB of address space each — about 4 GB of virtual memory for
     * sockets that are waiting for a byte. Here a connection is two pointers
     * that stay NULL until a byte arrives.
     *
     * RSS is measured, not computed from a struct size. The struct is only part
     * of the answer, and a calculation would be the thing being asserted rather
     * than the thing being observed. */
    const int count = 500;
    int target = count;
    reactor_t *r = fresh_reactor();
    cr_assert_eq(reactor_set_handler(r, echo_handler, NULL), 0);

    int *clients = malloc(sizeof(int) * (size_t)count);
    cr_assert_not_null(clients);

    size_t rss_before = rss_bytes();
    cr_assert_gt(rss_before, 0, "/proc/self/statm is unreadable");

    for (int i = 0; i < count; i++)
        clients[i] = connect_client(reactor_port(r));

    cr_assert_geq(step_until(r, has_enough_conns, &target, 1500), 0,
                  "only %d of %d connections were accepted", reactor_conn_count(r),
                  count);
    cr_assert_eq(reactor_conn_count(r), count);

    size_t rss_after = rss_bytes();
    double per_conn = rss_after > rss_before
                          ? (double)(rss_after - rss_before) / (double)count
                          : 0.0;

    fprintf(stderr,
            "  %d idle connections: sizeof(conn_t)=%zu, table=%zu bytes, "
            "RSS delta=%zu bytes (%.0f bytes/conn)\n",
            count, sizeof(conn_t), (size_t)count * sizeof(conn_t),
            rss_after > rss_before ? rss_after - rss_before : 0, per_conn);

    /* A thread per connection would need a stack each; even a very small 64 KB
     * stack would be 32 MB for 500. The bound is deliberately loose — this
     * asserts the architecture, not a tuning target. */
    cr_assert_lt(rss_after - rss_before, (size_t)8 * 1024 * 1024,
                 "500 idle connections grew RSS by %zu bytes, which is far more "
                 "than the connection table",
                 rss_after > rss_before ? rss_after - rss_before : 0);

    for (int i = 0; i < count; i++)
        send_request(clients[i], "GET / HTTP/1.1\r\n\r\n");

    for (int i = 0; i < count; i++) {
        buf_t *got;
        buf_new_into(&got, 256);
        for (int spin = 0; spin < 300 && buf_len(got) == 0; spin++) {
            reactor_poll_once(r, 5);
            drain(clients[i], got, 256);
        }
        cr_assert_gt(buf_len(got), 0, "connection %d was never answered", i);
        buf_free(got);
    }

    for (int i = 0; i < count; i++)
        close(clients[i]);
    free(clients);
    reactor_free(r);
}

Test(reactor, connections_that_vanish_are_reaped)
{
    /* A connection that is accepted and then reset must not leak. Half-open
     * connections are how file-descriptor exhaustion works: open, vanish,
     * repeat, until accept starts failing. */
    reactor_t *r = fresh_reactor();
    cr_assert_eq(reactor_set_handler(r, echo_handler, NULL), 0);

    for (int round = 0; round < 200; round++) {
        int client = connect_client(reactor_port(r));
        struct linger lin = {1, 0};
        setsockopt(client, SOL_SOCKET, SO_LINGER, &lin, sizeof(lin));
        close(client); /* RST rather than FIN */
        for (int i = 0; i < 4; i++)
            reactor_poll_once(r, 0);
    }

    for (int i = 0; i < 500 && reactor_conn_count(r) > 0; i++)
        reactor_poll_once(r, 5);

    cr_assert_eq(reactor_conn_count(r), 0, "%d connections were never reaped",
                 reactor_conn_count(r));

    reactor_free(r);
}

Test(reactor, a_client_that_never_finishes_a_request_is_closed_at_the_cap)
{
    /* Without a cap, a peer that sends an endless header and never a blank line
     * grows the process until it is killed. The cap is a refusal, not a
     * truncation: the connection is dropped rather than half-parsed. */
    reactor_t *r = fresh_reactor();
    int answered = 0;
    cr_assert_eq(reactor_set_handler(r, echo_handler, &answered), 0);

    int client = connect_client(reactor_port(r));

    char blob[4096];
    memset(blob, 'x', sizeof(blob));
    for (int i = 0; i < 40; i++) {
        if (send(client, blob, sizeof(blob), 0) < 0)
            break; /* the reactor closed it, which is the point */
        for (int spin = 0; spin < 4; spin++)
            reactor_poll_once(r, 5);
        if (reactor_conn_count(r) == 0)
            break;
    }

    for (int i = 0; i < 200 && reactor_conn_count(r) > 0; i++)
        reactor_poll_once(r, 5);

    cr_assert_eq(answered, 0, "an unterminated request must never be answered");
    cr_assert_eq(reactor_conn_count(r), 0,
                 "the connection should have been closed at the cap");

    close(client);
    reactor_free(r);
}

Test(reactor, null_and_invalid_arguments_are_safe)
{
    reactor_free(NULL);
    cr_assert_eq(reactor_port(NULL), -1);
    cr_assert_eq(reactor_conn_count(NULL), 0);
    cr_assert_eq(reactor_poll_once(NULL, 0), -1);
    cr_assert_eq(reactor_set_handler(NULL, echo_handler, NULL), -1);

    reactor_t *r = fresh_reactor();
    cr_assert_eq(reactor_set_handler(r, NULL, NULL), -1);
    reactor_free(r);

    /* backlog 0 is a legal listen() argument on Linux and means "use the
     * default", so it must not be refused the way a NULL handler is. */
    reactor_t *r2 = reactor_new(0, 0);
    cr_assert_not_null(r2, "backlog 0 should fall back to the default");
    reactor_free(r2);
}