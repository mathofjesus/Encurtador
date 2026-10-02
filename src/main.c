/* Application entry point.
 *
 * Two modes. Started with no arguments it loads config.yaml, brings up storage
 * and the router, and runs the reactor until a signal stops it. Started with
 * --self-test it binds an ephemeral port, drives one client through the real
 * routes over a real socket, and exits non-zero if a check fails. The self-test
 * is what `make test-e2e` runs inside the sandbox; without it that target would
 * launch the skeleton and report success without exercising anything.
 *
 * The client in --self-test steps the reactor with reactor_poll_once rather than
 * running it on a thread, the same way the reactor tests do. A threaded
 * self-test would pass or fail depending on scheduling; stepping it makes each
 * check about what the server replied, not about when it replied.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "api/application.h"
#include "config.h"
#include "net/reactor.h"
#include "observability/log.h"

#define LISTEN_BACKLOG 512

/* Set while the reactor runs so the signal handler has something to stop. A
 * plain pointer, read on a signal: reactor_stop only sets a flag, so this is as
 * much as a handler can safely do. */
static reactor_t *g_reactor;

static void on_signal(int signo)
{
    (void)signo;
    if (g_reactor)
        reactor_stop(g_reactor);
}

/* --- self-test client --------------------------------------------------- */

static int connect_client(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    /* The listener is already bound and listening, so the kernel completes this
     * from the backlog even though no accept has run yet. */
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* True once a whole response is present. Responses are never chunked, so
 * headers plus the declared Content-Length is the whole message. */
static int response_complete(const char *buf, size_t len)
{
    const char *end = strstr(buf, "\r\n\r\n");
    if (!end)
        return 0;

    size_t header_len = (size_t)(end - buf) + 4;
    const char *cl = strstr(buf, "Content-Length: ");
    if (!cl)
        return 0;

    long n = strtol(cl + strlen("Content-Length: "), NULL, 10);
    if (n < 0)
        return 0;
    return header_len + (size_t)n <= len;
}

/* Writes request, then alternately steps the reactor and drains the socket until
 * a full response arrives. Returns the number of response bytes, or -1. */
static int exchange(reactor_t *r, int fd, const char *request, char *resp,
                    size_t cap)
{
    size_t reqlen = strlen(request);
    size_t sent = 0;
    while (sent < reqlen) {
        ssize_t n = send(fd, request + sent, reqlen - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        sent += (size_t)n;
    }

    size_t total = 0;
    resp[0] = '\0';
    for (int steps = 0; steps < 2000; steps++) {
        (void)reactor_poll_once(r, 20);
        for (;;) {
            if (total + 1 >= cap)
                break;
            ssize_t n = recv(fd, resp + total, cap - 1 - total, MSG_DONTWAIT);
            if (n > 0) {
                total += (size_t)n;
                resp[total] = '\0';
                continue;
            }
            break; /* EAGAIN, or a close we do not need to distinguish here */
        }
        if (response_complete(resp, total))
            break;
    }
    return (int)total;
}

static int do_request(reactor_t *r, int fd, const char *method, const char *path,
                      const char *body, char *resp, size_t cap)
{
    char req[2048];
    size_t body_len = body ? strlen(body) : 0;
    int n = snprintf(req, sizeof(req),
                     "%s %s HTTP/1.1\r\nHost: localhost\r\n"
                     "Content-Length: %zu\r\n\r\n%s",
                     method, path, body_len, body ? body : "");
    if (n < 0 || (size_t)n >= sizeof(req))
        return -1;
    return exchange(r, fd, req, resp, cap);
}

static int extract_code(const char *resp, char *out, size_t cap)
{
    const char *p = strstr(resp, "\"code\":\"");
    if (!p)
        return -1;
    p += strlen("\"code\":\"");
    const char *q = strchr(p, '"');
    if (!q)
        return -1;

    size_t n = (size_t)(q - p);
    if (n == 0 || n >= cap)
        return -1;
    memcpy(out, p, n);
    out[n] = '\0';
    return 0;
}

static int self_test_run(reactor_t *r, log_t *log)
{
    (void)log;

    int fd = connect_client(reactor_port(r));
    if (fd < 0) {
        fprintf(stderr, "self-test: connect failed: %s\n", strerror(errno));
        return 1;
    }

    static char resp[16384];
    int checks = 0;
    int failures = 0;
    char code[64] = {0};

#define CHECK(cond, ...)                                                        \
    do {                                                                        \
        checks++;                                                               \
        if (!(cond)) {                                                          \
            failures++;                                                         \
            fprintf(stderr, "self-test FAIL (check %d): ", checks);             \
            fprintf(stderr, __VA_ARGS__);                                       \
            fprintf(stderr, "\n");                                              \
        }                                                                       \
    } while (0)

    if (do_request(r, fd, "GET", "/health", NULL, resp, sizeof(resp)) < 0)
        CHECK(0, "GET /health produced no response");

    CHECK(strstr(resp, "200 OK") != NULL, "GET /health: %.200s", resp);

    if (do_request(r, fd, "POST", "/api/v1/shorten",
                   "{\"url\":\"https://example.com/e2e\"}", resp,
                   sizeof(resp)) < 0)
        CHECK(0, "POST /api/v1/shorten produced no response");

    CHECK(strstr(resp, "201 Created") != NULL, "POST /shorten: %.300s", resp);
    CHECK(extract_code(resp, code, sizeof(code)) == 0,
          "POST /shorten carried no code: %.300s", resp);
    CHECK(strlen(code) == 7, "generated code width was %zu, not 7", strlen(code));

    char path[128];
    snprintf(path, sizeof(path), "/%s", code);
    if (do_request(r, fd, "GET", path, NULL, resp, sizeof(resp)) < 0)
        CHECK(0, "GET /%s produced no response", code);

    CHECK(strstr(resp, "301 Moved Permanently") != NULL, "GET /%s: %.200s", code,
          resp);
    CHECK(strstr(resp, "Location: https://example.com/e2e") != NULL,
          "GET /%s pointed somewhere else: %.300s", code, resp);

    if (do_request(r, fd, "GET", "/zzzzzzz", NULL, resp, sizeof(resp)) < 0)
        CHECK(0, "GET /zzzzzzz produced no response");
    CHECK(strstr(resp, "404 Not Found") != NULL, "unknown code: %.200s", resp);

    if (do_request(r, fd, "POST", "/api/v1/shorten", "not json", resp,
                   sizeof(resp)) < 0)
        CHECK(0, "malformed POST produced no response");
    CHECK(strstr(resp, "400 Bad Request") != NULL, "malformed body: %.300s", resp);

    if (do_request(r, fd, "GET", "/metrics", NULL, resp, sizeof(resp)) < 0)
        CHECK(0, "GET /metrics produced no response");
    CHECK(strstr(resp, "200 OK") != NULL, "GET /metrics: %.200s", resp);
    CHECK(strstr(resp, "shortener_shorten_total 1") != NULL,
          "/metrics did not count the one successful create: %.400s", resp);

    close(fd);

    printf("self-test: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

/* --- wiring ------------------------------------------------------------- */

int main(int argc, char **argv)
{
    int self_test = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--self-test") == 0) {
            self_test = 1;
        } else {
            fprintf(stderr, "usage: %s [--self-test]\n", argv[0]);
            return 2;
        }
    }

    const char *path = getenv("CONFIG_PATH");
    if (!path || !*path)
        path = "config.yaml";

    config_t *cfg = config_load(path);
    if (!cfg) {
        fprintf(stderr, "shortener: cannot load config from %s\n", path);
        return 1;
    }

    log_t *log = log_new(STDERR_FILENO, log_level_from_string(getenv("LOG_LEVEL")));

    application_t *app = application_new(cfg, log);
    if (!app) {
        fprintf(stderr, "shortener: storage is not reachable; refusing to start\n");
        log_free(log);
        config_free(cfg);
        return 1;
    }

    /* The self-test binds an ephemeral port so it can never collide with a real
     * server or a parallel run; the server binds what the config says. */
    reactor_t *reactor = reactor_new(self_test ? 0 : cfg->app.port, LISTEN_BACKLOG);
    if (!reactor) {
        fprintf(stderr, "shortener: cannot listen on port %d: %s\n",
                self_test ? 0 : cfg->app.port, strerror(errno));
        application_free(app);
        log_free(log);
        config_free(cfg);
        return 1;
    }

    if (reactor_set_handler(reactor, application_handle_conn, app) != 0) {
        fprintf(stderr, "shortener: cannot install the request handler\n");
        reactor_free(reactor);
        application_free(app);
        log_free(log);
        config_free(cfg);
        return 1;
    }

    int rc;
    if (self_test) {
        rc = self_test_run(reactor, log);
    } else {
        g_reactor = reactor;
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_signal;
        sigaction(SIGINT, &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);

        if (log) {
            log_record_t rec = {0};
            rec.level = LOG_INFO;
            rec.event = "listening";
            rec.duration_us = -1;
            log_write(log, &rec);
        }

        rc = reactor_run(reactor) == 0 ? 0 : 1;
        g_reactor = NULL;
    }

    reactor_free(reactor);
    application_free(app);
    log_free(log);
    config_free(cfg);
    return rc;
}
