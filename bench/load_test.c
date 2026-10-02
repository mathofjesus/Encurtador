/* bench/load_test.c — the load generator.
 *
 * Closed-loop by design: each thread sends one request, reads the whole response,
 * and only then sends the next. That measures the system the way a client
 * experiences it — latency at a fixed concurrency — rather than the way a queue
 * drains. The cost is that the reported req/s is what *this* generator could
 * sustain, so a generator that is itself starved reports a floor rather than the
 * server's ceiling. In the sandbox the generator shares the app container's
 * 0.35 vCPU, so every figure this prints is a floor. That is stated in the
 * output rather than left for the reader to work out.
 *
 * Read and write latencies are never pooled into one percentile. They are
 * different operations — a redirect is a cache lookup, a shorten is a sequence
 * plus an insert — and averaging the two hides exactly the difference the
 * exercise exists to show.
 *
 * Where the read codes come from decides whether a "cold" run is cold. Creating
 * them over HTTP does not work: create is write-through, so every code the
 * generator has just created is already cached and the first read of it is a
 * cache hit. A generator with no database access cannot flush Redis either, and
 * asking the harness to flush between two runs of the same process is not
 * expressible. So the codes for a read run come from --codes-file, which the
 * harness fills by inserting rows directly: the cache has never seen them, so
 * the first read of each is a genuine miss. --prime-passes then reads them all
 * once to turn the next run warm. Both are reachable with the same binary and no
 * hidden ordering between runs.
 *
 * It links neither libpq nor hiredis. It is a client, and must not share a code
 * path with the server it measures.
 */
/* MSG_NOSIGNAL needs it, and the Makefile already passes -D_GNU_SOURCE, so the
 * define is guarded rather than assumed. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define CODE_MAX 16    /* one byte over CODE_MAX_LENGTH */
#define RESP_MAX 32768 /* the server's largest possible body is 8192 */
#define STATUS_MAX 600
#define URL_MAX 256

typedef struct {
    char host[64];
    int port;
    int threads;
    int duration;     /* seconds; 0 means only the caps bound the run */
    int reads;        /* redirects per cycle, per thread */
    int writes;       /* shortens per cycle, per thread */
    long max_reads;   /* per-thread read cap; 0 means unlimited */
    long max_writes;  /* per-thread write cap; 0 means unlimited */
    long warmup;      /* urls created before the clock starts */
    long pool_max;    /* codes retained for reads */
    int prime_passes; /* read passes over the pool before the clock starts */
    const char *codes_file; /* codes to read, one per line; skips warmup */
} cfg_t;

static cfg_t cfg;

/* --- the pool of codes reads draw from ----------------------------------- */

/* Read samples are sequential, not random, and that is deliberate. With one pass
 * over a pool the same size as the read count, every read lands on a code it has
 * not read yet, which is what makes an unprimed run actually cold. A random draw
 * would re-read codes it had just warmed and quietly turn the cold measurement
 * into the warm one. */
static pthread_mutex_t pool_mu = PTHREAD_MUTEX_INITIALIZER;
static char (*pool)[CODE_MAX];
static long pool_cap;
static long pool_count;

static void pool_init(long capacity)
{
    pool = calloc((size_t)capacity, sizeof(*pool));
    pool_cap = pool ? capacity : 0;
    pool_count = 0;
}

static int pool_add(const char *code)
{
    pthread_mutex_lock(&pool_mu);
    int ok = 0;
    if (pool_count < pool_cap) {
        snprintf(pool[pool_count], CODE_MAX, "%s", code);
        pool_count++;
        ok = 1;
    }
    pthread_mutex_unlock(&pool_mu);
    return ok;
}

static int pool_take(long index, char *out)
{
    pthread_mutex_lock(&pool_mu);
    int ok = 0;
    if (pool_count > 0 && index >= 0) {
        snprintf(out, CODE_MAX, "%s", pool[index % pool_count]);
        ok = 1;
    }
    pthread_mutex_unlock(&pool_mu);
    return ok;
}

/* --- timing and samples -------------------------------------------------- */

static long long now_us(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

typedef struct {
    long *v;
    size_t n;
    size_t cap;
} samples_t;

/* Returns 0 when the sample was stored, -1 when it was dropped. A dropped sample
 * is counted rather than ignored: silently shrinking the sample set would move a
 * percentile without saying so. */
static int samples_push(samples_t *s, long us)
{
    if (s->n == s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 4096;
        long *v = realloc(s->v, cap * sizeof(*v));
        if (!v)
            return -1;
        s->v = v;
        s->cap = cap;
    }
    s->v[s->n++] = us;
    return 0;
}

static int cmp_long(const void *a, const void *b)
{
    long x = *(const long *)a, y = *(const long *)b;
    return (x > y) - (x < y);
}

/* --- one connection ------------------------------------------------------ */

typedef struct {
    int fd;
    char in[RESP_MAX];
    size_t in_len;
    size_t in_off; /* bytes belonging to responses already reported */
} conn_t;

enum { EX_OK = 0, EX_TRANSPORT = -1, EX_MALFORMED = -2, EX_CLOSED = -3 };

static int conn_open(conn_t *c)
{
    memset(c, 0, sizeof(*c));
    c->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (c->fd < 0)
        return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)cfg.port);
    if (inet_pton(AF_INET, cfg.host, &addr.sin_addr) != 1) {
        close(c->fd);
        c->fd = -1;
        return -1;
    }

    if (connect(c->fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(c->fd);
        c->fd = -1;
        return -1;
    }

    /* Without this the kernel delays small responses, and that delay would be
     * charged to the server in every percentile below. */
    int one = 1;
    setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return 0;
}

static void conn_close(conn_t *c)
{
    if (c->fd >= 0)
        close(c->fd);
    c->fd = -1;
}

static int send_all(conn_t *c, const char *req, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(c->fd, req + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

/* Drops the bytes of finished responses. Called only between requests, never in
 * the middle of a message, so moving the tail cannot split anything. */
static void conn_compact(conn_t *c)
{
    if (c->in_off == 0)
        return;
    memmove(c->in, c->in + c->in_off, c->in_len - c->in_off);
    c->in_len -= c->in_off;
    c->in_off = 0;
}

static int conn_fill(conn_t *c)
{
    if (c->in_off > 0)
        conn_compact(c);
    if (c->in_len == sizeof(c->in))
        return EX_MALFORMED; /* larger than any response this server can emit */

    ssize_t n = recv(c->fd, c->in + c->in_len, sizeof(c->in) - c->in_len, 0);
    if (n == 0)
        return EX_CLOSED;
    if (n < 0) {
        if (errno == EINTR)
            return EX_OK; /* nothing read, and not a failure */
        return EX_TRANSPORT;
    }
    c->in_len += (size_t)n;
    return EX_OK;
}

/* Sends one request and reads its whole response. Responses are never chunked —
 * the server refuses Transfer-Encoding rather than guessing — so the headers
 * plus the declared Content-Length is the whole message. The body pointer aims
 * into the connection's buffer and stays valid until the next call. */
static int conn_exchange(conn_t *c, const char *req, size_t req_len,
                         int *out_status, const char **out_body,
                         size_t *out_body_len)
{
    if (send_all(c, req, req_len) != 0)
        return EX_TRANSPORT;

    for (;;) {
        /* Only the unread tail is examined. The head of the buffer belongs to a
         * response already reported, so scanning from index 0 would find the
         * previous response's headers again and answer this request with the last
         * one — a run that appears to work and issues no requests at all. */
        const char *buf = c->in + c->in_off;
        size_t avail = c->in_len - c->in_off;

        const char *head_end = NULL;
        for (size_t i = 0; i + 4 <= avail; i++) {
            if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' &&
                buf[i + 3] == '\n') {
                head_end = buf + i + 4;
                break;
            }
        }

        if (head_end) {
            int status = 0;
            if (sscanf(buf, "HTTP/1.1 %d", &status) != 1)
                return EX_MALFORMED;

            /* Bounded to the header region, so a body that happens to contain the
             * literal text cannot be mistaken for a header. */
            size_t head_len = (size_t)(head_end - buf);
            const char *cl = NULL;
            for (size_t i = 0; i + 16 <= head_len; i++) {
                if (memcmp(buf + i, "Content-Length: ", 16) == 0) {
                    cl = buf + i + 16;
                    break;
                }
            }
            if (!cl)
                return EX_MALFORMED;

            long declared = strtol(cl, NULL, 10);
            if (declared < 0 || declared > RESP_MAX)
                return EX_MALFORMED;

            size_t body_at = head_len;
            if (avail - body_at < (size_t)declared) {
                int rc = conn_fill(c);
                if (rc != EX_OK)
                    return rc;
                continue;
            }

            c->in_off += body_at + (size_t)declared;
            *out_status = status;
            *out_body = buf + body_at;
            *out_body_len = (size_t)declared;
            return EX_OK;
        }

        int rc = conn_fill(c);
        if (rc != EX_OK)
            return rc;
    }
}

/* --- request building ---------------------------------------------------- */

static size_t build_get(char *out, size_t cap, const char *code)
{
    /* No Content-Length, because no real client sends one on a GET. The e2e
     * suite cannot catch a parser that required one: those requests all declare
     * a length. */
    int n = snprintf(out, cap,
                     "GET /%s HTTP/1.1\r\n"
                     "Host: bench\r\n"
                     "Accept: */*\r\n\r\n",
                     code);
    return (n < 0 || (size_t)n >= cap) ? 0 : (size_t)n;
}

static size_t build_post(char *out, size_t cap, const char *url)
{
    char body[URL_MAX];
    int bn = snprintf(body, sizeof(body), "{\"url\":\"%s\"}", url);
    if (bn < 0 || (size_t)bn >= (int)sizeof(body))
        return 0;

    int n = snprintf(out, cap,
                     "POST /api/v1/shorten HTTP/1.1\r\n"
                     "Host: bench\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: %d\r\n\r\n%s",
                     bn, body);
    return (n < 0 || (size_t)n >= cap) ? 0 : (size_t)n;
}

/* Pulls "code":"xxxx" out of a 201 body. Returns 0 on success. */
static int extract_code(const char *body, size_t len, char *out)
{
    static const char needle[] = "\"code\":\"";
    const size_t nlen = sizeof(needle) - 1;

    for (size_t i = 0; i + nlen <= len; i++) {
        if (memcmp(body + i, needle, nlen) != 0)
            continue;

        size_t start = i + nlen;
        size_t j = start;
        while (j < len && body[j] != '"')
            j++;
        if (j >= len || j == start || j - start >= CODE_MAX)
            return -1;

        memcpy(out, body + start, j - start);
        out[j - start] = '\0';
        return 0;
    }
    return -1;
}

/* --- per-thread statistics ----------------------------------------------- */

typedef struct {
    long reads_ok;
    long reads_err;
    long writes_ok;
    long writes_err;

    long status[STATUS_MAX];
    long reconnects;
    long malformed;
    long read_404; /* the server said 404 */
    long no_code;  /* the generator had nothing to read: a setup mistake */
    long pool_full;
    long dropped_samples;

    /* Kept apart so each row's percentiles describe only that operation. */
    samples_t read_lat;
    samples_t write_lat;
} stats_t;

typedef struct {
    int index;
    stats_t *st;
} worker_t;

/* --- one request --------------------------------------------------------- */

/* Issues one request and records it. Returns an EX_* code; on EX_OK the request
 * was counted as a success or an error according to its status. */
static int step(stats_t *st, conn_t **c, int is_write, long seq)
{
    char req[1024];
    size_t len;

    if (is_write) {
        /* Distinct URLs, so the row sizes stay comparable and the run does not
         * quietly depend on Postgres compressing repeated text. */
        char url[URL_MAX];
        snprintf(url, sizeof(url), "https://example.com/bench/%d/%ld", (int)getpid(),
                 seq);
        len = build_post(req, sizeof(req), url);
    } else {
        char code[CODE_MAX];
        if (!pool_take(seq, code)) {
            /* No codes to read is a setup mistake, not a server answer, so it is
             * counted apart from a 404. A silent no-op would report a fast, fast
             * meaningless run. */
            st->no_code++;
            return EX_MALFORMED;
        }
        len = build_get(req, sizeof(req), code);
    }
    if (len == 0) {
        st->malformed++;
        return EX_MALFORMED;
    }

    if (*c == NULL) {
        conn_t *fresh = calloc(1, sizeof(*fresh));
        if (!fresh) {
            st->reconnects++;
            return EX_TRANSPORT;
        }
        if (conn_open(fresh) != 0) {
            free(fresh);
            st->reconnects++;
            return EX_TRANSPORT;
        }
        *c = fresh;
    }

    int status = 0;
    const char *body = NULL;
    size_t body_len = 0;

    long long t0 = now_us();
    int rc = conn_exchange(*c, req, len, &status, &body, &body_len);
    long us = (long)(now_us() - t0);

    if (rc == EX_CLOSED || rc == EX_TRANSPORT) {
        /* A dropped keep-alive is normal under load. Reconnecting keeps the run
         * going, but the reconnect is counted so a connection ceiling cannot
         * hide inside the average. */
        conn_close(*c);
        free(*c);
        *c = NULL;
        st->reconnects++;
        if (is_write)
            st->writes_err++;
        else
            st->reads_err++;
        return rc;
    }
    if (rc != EX_OK) {
        st->malformed++;
        if (is_write)
            st->writes_err++;
        else
            st->reads_err++;
        return rc;
    }

    if (status > 0 && status < STATUS_MAX)
        st->status[status]++;

    /* Percentiles describe the requests that succeeded. A 429 or a 404 still
     * cost a round trip, but averaging a rejection into the distribution would
     * report the policy's latency as if it were the server's. */
    if (is_write) {
        if (status == 201) {
            char code[CODE_MAX];
            if (extract_code(body, body_len, code) == 0) {
                /* Every new code joins the pool, so a mixed run reads codes it
                 * has just written instead of re-reading the warmup set forever. */
                if (!pool_add(code))
                    st->pool_full++;
            }
            st->writes_ok++;
            if (samples_push(&st->write_lat, us) != 0)
                st->dropped_samples++;
        } else {
            st->writes_err++;
        }
    } else if (status == 301 || status == 200) {
        st->reads_ok++;
        if (samples_push(&st->read_lat, us) != 0)
            st->dropped_samples++;
    } else {
        st->reads_err++;
        if (status == 404)
            st->read_404++;
    }
    return EX_OK;
}

/* --- worker -------------------------------------------------------------- */

static void *worker(void *arg)
{
    worker_t *w = arg;
    stats_t *st = w->st;
    conn_t *c = NULL;

    long long deadline = cfg.duration > 0
                             ? now_us() + (long long)cfg.duration * 1000000
                             : 0;
    long done_reads = 0;
    long done_writes = 0;

    /* Stagger the workers across the pool. Left in step, every thread would read
     * the same code first and the cold measurement would only be cold once. */
    long cursor = (long)w->index * 7919;
    long write_seq = (long)w->index * 1000000;

    for (;;) {
        if (deadline && now_us() >= deadline)
            break;
        if (cfg.max_reads > 0 && done_reads >= cfg.max_reads)
            break;
        if (cfg.max_writes > 0 && done_writes >= cfg.max_writes)
            break;

        for (int i = 0; i < cfg.reads; i++) {
            step(st, &c, 0, cursor++);
            done_reads++;
            if (cfg.max_reads > 0 && done_reads >= cfg.max_reads)
                break;
            if (deadline && now_us() >= deadline)
                break;
        }
        for (int i = 0; i < cfg.writes; i++) {
            step(st, &c, 1, write_seq++);
            done_writes++;
            if (cfg.max_writes > 0 && done_writes >= cfg.max_writes)
                break;
            if (deadline && now_us() >= deadline)
                break;
        }
    }

    if (c) {
        conn_close(c);
        free(c);
    }
    return NULL;
}

/* --- warmup and priming -------------------------------------------------- */

/* Fills the pool from a file of codes, one per line. Returns the count loaded, or
 * -1 if the file could not be read. Blank lines and '#' comments are skipped so a
 * harness can write the file with a header. */
static long load_codes_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "load_test: cannot read %s: %s\n", path, strerror(errno));
        return -1;
    }

    char line[256];
    long loaded = 0;
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        size_t n = strlen(p);
        while (n > 0 && (p[n - 1] == '\n' || p[n - 1] == '\r' || p[n - 1] == ' '))
            p[--n] = '\0';
        if (n == 0 || *p == '#')
            continue;
        if (pool_add(p))
            loaded++;
    }
    fclose(f);
    return loaded;
}

/* Single-threaded, before the clock starts: create the codes reads draw from,
 * then optionally read them all so the cache is warm for what follows. Kept out
 * of the workers because its cost — a cold insert, or a cold first read per code
 * — would otherwise be averaged into the reported percentiles. */
static int prepare(void)
{
    if (cfg.codes_file) {
        /* The codes already exist and the cache has never seen them, so this is
         * the cold case. Creating them here instead would cache them all and
         * report a warm run under a cold label. */
        long loaded = load_codes_file(cfg.codes_file);
        if (loaded < 0)
            return -1;
        fprintf(stderr, "load_test: loaded %ld codes from %s\n", loaded,
                cfg.codes_file);
        if (pool_count == 0) {
            fprintf(stderr, "load_test: %s held no codes\n", cfg.codes_file);
            return -1;
        }
    }

    conn_t c;
    if (conn_open(&c) != 0) {
        fprintf(stderr, "load_test: cannot connect to %s:%d: %s\n", cfg.host,
                cfg.port, strerror(errno));
        return -1;
    }

    if (cfg.warmup > 0) {
        fprintf(stderr, "load_test: creating %ld urls\n", cfg.warmup);
        for (long i = 0; i < cfg.warmup; i++) {
            char url[URL_MAX];
            snprintf(url, sizeof(url), "https://example.com/warm/%ld", i);

            char req[1024];
            size_t len = build_post(req, sizeof(req), url);
            const char *body = NULL;
            size_t body_len = 0;
            int status = 0;
            if (len == 0 || conn_exchange(&c, req, len, &status, &body, &body_len) != EX_OK) {
                fprintf(stderr, "load_test: warmup lost the connection at %ld\n", i);
                conn_close(&c);
                return -1;
            }
            if (status != 201) {
                fprintf(stderr, "load_test: warmup got HTTP %d at %ld\n", status, i);
                conn_close(&c);
                return -1;
            }

            char code[CODE_MAX];
            if (extract_code(body, body_len, code) != 0) {
                fprintf(stderr, "load_test: no code in the reply at %ld\n", i);
                conn_close(&c);
                return -1;
            }
            (void)pool_add(code);
            if ((i + 1) % 1000 == 0)
                fprintf(stderr, "load_test: %ld/%ld\n", i + 1, cfg.warmup);
        }
    }

    for (int pass = 0; pass < cfg.prime_passes; pass++) {
        fprintf(stderr, "load_test: priming the cache, pass %d of %d\n", pass + 1,
                cfg.prime_passes);
        long hits = 0;
        for (long i = 0; i < pool_count; i++) {
            char code[CODE_MAX];
            if (!pool_take(i, code))
                break;

            char req[1024];
            size_t len = build_get(req, sizeof(req), code);
            const char *body = NULL;
            size_t body_len = 0;
            int status = 0;
            if (len == 0 || conn_exchange(&c, req, len, &status, &body, &body_len) != EX_OK)
                break;
            if (status == 301)
                hits++;
        }
        fprintf(stderr, "load_test: primed %ld entries\n", hits);
    }

    conn_close(&c);
    return 0;
}

/* --- reporting ----------------------------------------------------------- */

enum { GATHER_READ = 0, GATHER_WRITE = 1, GATHER_BOTH = 2 };

static long *gather(const stats_t *st, int nthreads, int mode, size_t *out_n)
{
    size_t total = 0;
    for (int t = 0; t < nthreads; t++) {
        if (mode != GATHER_WRITE)
            total += st[t].read_lat.n;
        if (mode != GATHER_READ)
            total += st[t].write_lat.n;
    }

    *out_n = 0;
    if (total == 0)
        return NULL;

    long *v = malloc(total * sizeof(*v));
    if (!v)
        return NULL;

    size_t at = 0;
    for (int t = 0; t < nthreads; t++) {
        if (mode != GATHER_WRITE)
            for (size_t i = 0; i < st[t].read_lat.n; i++)
                v[at++] = st[t].read_lat.v[i];
        if (mode != GATHER_READ)
            for (size_t i = 0; i < st[t].write_lat.n; i++)
                v[at++] = st[t].write_lat.v[i];
    }
    qsort(v, at, sizeof(*v), cmp_long);
    *out_n = at;
    return v;
}

static long percentile(const long *sorted, size_t n, int pct)
{
    if (n == 0)
        return 0;
    size_t idx = (size_t)((double)n * pct / 100.0);
    if (idx >= n)
        idx = n - 1;
    return sorted[idx];
}

static void print_row(const char *label, long ok, long err, double seconds,
                      const long *sorted, size_t n)
{
    printf("  %-6s %9ld ok %8ld err %9.1f req/s", label, ok, err,
           seconds > 0 ? (double)ok / seconds : 0.0);
    if (n > 0)
        printf("   p50 %7ldus  p95 %7ldus  p99 %7ldus", percentile(sorted, n, 50),
               percentile(sorted, n, 95), percentile(sorted, n, 99));
    else
        printf("   (no samples)");
    printf("\n");
}

static void report(const stats_t *st, int nthreads, double seconds)
{
    long reads_ok = 0, reads_err = 0, writes_ok = 0, writes_err = 0;
    long status[STATUS_MAX];
    long reconnects = 0, malformed = 0, read_404 = 0, dropped = 0, pool_full = 0;
    long no_code = 0;
    memset(status, 0, sizeof(status));

    for (int t = 0; t < nthreads; t++) {
        reads_ok += st[t].reads_ok;
        reads_err += st[t].reads_err;
        writes_ok += st[t].writes_ok;
        writes_err += st[t].writes_err;
        reconnects += st[t].reconnects;
        malformed += st[t].malformed;
        read_404 += st[t].read_404;
        no_code += st[t].no_code;
        dropped += st[t].dropped_samples;
        pool_full += st[t].pool_full;
        for (int i = 0; i < STATUS_MAX; i++)
            status[i] += st[t].status[i];
    }

    size_t n_read = 0, n_write = 0, n_all = 0;
    long *s_read = gather(st, nthreads, GATHER_READ, &n_read);
    long *s_write = gather(st, nthreads, GATHER_WRITE, &n_write);
    long *s_all = gather(st, nthreads, GATHER_BOTH, &n_all);

    printf("scenario: %d thread(s), %d reads + %d writes per cycle, %d s, "
           "pool %ld, primed %d\n",
           nthreads, cfg.reads, cfg.writes, cfg.duration, pool_count,
           cfg.prime_passes);
    printf("elapsed: %.3f s\n", seconds);

    print_row("reads", reads_ok, reads_err, seconds, s_read, n_read);
    print_row("writes", writes_ok, writes_err, seconds, s_write, n_write);
    print_row("all", reads_ok + writes_ok, reads_err + writes_err, seconds, s_all,
              n_all);

    printf("  statuses:");
    for (int i = 0; i < STATUS_MAX; i++)
        if (status[i])
            printf(" %d=%ld", i, status[i]);
    printf("\n");

    printf("  failures: reconnects %ld  malformed %ld  404-reads %ld  "
           "no-codes %ld  pool-full %ld  dropped-samples %ld\n",
           reconnects, malformed, read_404, no_code, pool_full, dropped);

    free(s_read);
    free(s_write);
    free(s_all);
}

/* --- arguments ----------------------------------------------------------- */

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [options]\n"
            "  --host H           server host (default 127.0.0.1)\n"
            "  --port P           server port (default 8000)\n"
            "  --threads N        concurrent connections (default 1)\n"
            "  --duration S       seconds; 0 means only the caps bound the run\n"
            "  --reads N          redirects per cycle per thread (default 9)\n"
            "  --writes N         shortens per cycle per thread (default 0)\n"
            "  --max-reads N      stop a thread after N reads (default 0)\n"
            "  --max-writes N     stop a thread after N writes (default 0)\n"
            "  --warmup N         urls created before the clock starts\n"
            "  --codes-file PATH  read these codes, one per line; skips warmup\n"
            "  --prime-passes N   read passes over the pool before the clock\n"
            "  --pool-max N       codes retained for reads (default 200000)\n"
            "  --label TEXT       echoed in the scenario line\n",
            argv0);
}

static int arg_nonneg(const char *s, const char *what)
{
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (!end || *end != '\0' || v < 0 || v > 1000000000L) {
        fprintf(stderr, "load_test: %s: not a number: %s\n", what, s);
        return -1;
    }
    return (int)v;
}

int main(int argc, char **argv)
{
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.host, sizeof(cfg.host), "127.0.0.1");
    cfg.port = 8000;
    cfg.threads = 1;
    cfg.duration = 30;
    cfg.reads = 9;
    cfg.writes = 0;
    cfg.warmup = 100;
    cfg.pool_max = 200000;
    const char *label = "";

    /* A codes file already provides the read codes, so the default warmup of 100
     * would add 100 unrequested URLs to the measurement's database. */
    int warmup_given = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (strcmp(a, "--help") == 0) {
            usage(argv[0]);
            return 0;
        }
        if (strcmp(a, "--label") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "load_test: --label needs a value\n");
                return 2;
            }
            label = argv[i];
            continue;
        }
        if (i + 1 >= argc) {
            fprintf(stderr, "load_test: %s needs a value\n", a);
            usage(argv[0]);
            return 2;
        }

        const char *v = argv[++i];
        int n;

        if (strcmp(a, "--host") == 0) {
            snprintf(cfg.host, sizeof(cfg.host), "%s", v);
        } else if (strcmp(a, "--port") == 0) {
            if ((n = arg_nonneg(v, "--port")) < 0) return 2;
            cfg.port = n;
        } else if (strcmp(a, "--threads") == 0) {
            if ((n = arg_nonneg(v, "--threads")) < 0) return 2;
            if (n < 1 || n > 64) {
                fprintf(stderr, "load_test: --threads must be 1..64\n");
                return 2;
            }
            cfg.threads = n;
        } else if (strcmp(a, "--duration") == 0) {
            if ((n = arg_nonneg(v, "--duration")) < 0) return 2;
            cfg.duration = n;
        } else if (strcmp(a, "--reads") == 0) {
            if ((n = arg_nonneg(v, "--reads")) < 0) return 2;
            cfg.reads = n;
        } else if (strcmp(a, "--writes") == 0) {
            if ((n = arg_nonneg(v, "--writes")) < 0) return 2;
            cfg.writes = n;
        } else if (strcmp(a, "--max-reads") == 0) {
            if ((n = arg_nonneg(v, "--max-reads")) < 0) return 2;
            cfg.max_reads = n;
        } else if (strcmp(a, "--max-writes") == 0) {
            if ((n = arg_nonneg(v, "--max-writes")) < 0) return 2;
            cfg.max_writes = n;
        } else if (strcmp(a, "--warmup") == 0) {
            if ((n = arg_nonneg(v, "--warmup")) < 0) return 2;
            cfg.warmup = n;
            warmup_given = 1;
        } else if (strcmp(a, "--codes-file") == 0) {
            cfg.codes_file = v;
        } else if (strcmp(a, "--prime-passes") == 0) {
            if ((n = arg_nonneg(v, "--prime-passes")) < 0) return 2;
            cfg.prime_passes = n;
        } else if (strcmp(a, "--pool-max") == 0) {
            if ((n = arg_nonneg(v, "--pool-max")) < 0) return 2;
            cfg.pool_max = n;
        } else {
            fprintf(stderr, "load_test: unknown option: %s\n", a);
            usage(argv[0]);
            return 2;
        }
    }

    if (cfg.reads == 0 && cfg.writes == 0) {
        fprintf(stderr, "load_test: --reads and --writes are both 0: nothing to do\n");
        return 2;
    }
    if (cfg.codes_file && !warmup_given)
        cfg.warmup = 0;
    if (cfg.reads > 0 && cfg.codes_file == NULL && cfg.warmup == 0) {
        fprintf(stderr,
                "load_test: reads were requested but there are no codes to read: "
                "pass --codes-file or --warmup\n");
        return 2;
    }
    if (cfg.duration == 0 && cfg.max_reads == 0 && cfg.max_writes == 0) {
        fprintf(stderr, "load_test: --duration 0 with no --max-reads or "
                        "--max-writes would never stop\n");
        return 2;
    }
    if (cfg.pool_max < 1) {
        fprintf(stderr, "load_test: --pool-max must be at least 1\n");
        return 2;
    }

    pool_init(cfg.warmup > cfg.pool_max ? cfg.warmup : cfg.pool_max);

    /* Writing to a connection the server already closed must not kill the
     * process: a dropped keep-alive is data, not a reason to abort the run. */
    signal(SIGPIPE, SIG_IGN);

    if (prepare() != 0)
        return 1;

    if (label && *label)
        printf("label: %s\n", label);

    stats_t *st = calloc((size_t)cfg.threads, sizeof(*st));
    pthread_t *tid = calloc((size_t)cfg.threads, sizeof(*tid));
    if (!st || !tid) {
        fprintf(stderr, "load_test: out of memory\n");
        return 1;
    }

    int started = 0;
    /* One worker_t per thread, not a shared stack slot: the worker reads it for
     * as long as it runs, which is past the end of this loop iteration. */
    worker_t **w = calloc((size_t)cfg.threads, sizeof(*w));
    if (!w) {
        fprintf(stderr, "load_test: out of memory\n");
        return 1;
    }
    for (int i = 0; i < cfg.threads; i++) {
        w[i] = calloc(1, sizeof(*w[i]));
        if (!w[i])
            break;
        w[i]->index = i;
        w[i]->st = &st[i];
        if (pthread_create(&tid[i], NULL, worker, w[i]) != 0)
            break;
        started++;
    }
    if (started == 0) {
        fprintf(stderr, "load_test: could not start a worker\n");
        return 1;
    }

    long long t0 = now_us();
    for (int i = 0; i < started; i++)
        pthread_join(tid[i], NULL);
    double seconds = (double)(now_us() - t0) / 1e6;

    /* t0 is taken after the threads are created so their startup is not charged
     * to the measurement; the workers' own deadline is what bounds them. */

    report(st, started, seconds);

    for (int i = 0; i < started; i++) {
        free(st[i].read_lat.v);
        free(st[i].write_lat.v);
    }
    for (int i = 0; i < cfg.threads; i++)
        free(w[i]);
    free(w);
    free(st);
    free(tid);

    printf("note: the generator shared the app container's cgroup, so every "
           "figure above is a floor, not a ceiling.\n");
    return 0;
}