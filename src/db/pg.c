#include "db/pg.h"

#include <errno.h>
#include <libpq-fe.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "codegen.h"

/* SQLSTATE for a unique violation. The plan's collision story depends on
 * recognising this one specifically rather than treating every failure as
 * "duplicate", because a full disk also makes an insert fail. */
#define SQLSTATE_UNIQUE_VIOLATION "23505"

/* How long to wait for a free connection before giving up. Bounded so a stuck
 * worker cannot park a caller forever. */
#define PG_CHECKOUT_TIMEOUT_SEC 5

typedef struct {
    PGconn *conn;
    int in_use;
} pg_slot_t;

struct pg_pool {
    pg_slot_t *slots;
    int size;
    char dsn[512];
    pthread_mutex_t mu;
    pthread_cond_t cv;
};

static void slot_release(pg_pool_t *pool, PGconn *conn)
{
    pthread_mutex_lock(&pool->mu);
    for (int i = 0; i < pool->size; i++) {
        if (pool->slots[i].conn == conn) {
            pool->slots[i].in_use = 0;
            break;
        }
    }
    pthread_cond_signal(&pool->cv);
    pthread_mutex_unlock(&pool->mu);
}

/* Reconnects a slot whose connection has died. Without this a single network
 * blip would leave the pool permanently one smaller. */
static PGconn *slot_reconnect(pg_pool_t *pool, int index)
{
    PGconn *fresh = PQconnectdb(pool->dsn);
    if (fresh && PQstatus(fresh) == CONNECTION_OK) {
        PQfinish(pool->slots[index].conn);
        pool->slots[index].conn = fresh;
        return fresh;
    }
    if (fresh)
        PQfinish(fresh);
    return NULL;
}

static PGconn *checkout(pg_pool_t *pool)
{
    pthread_mutex_lock(&pool->mu);

    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += PG_CHECKOUT_TIMEOUT_SEC;

    for (;;) {
        for (int i = 0; i < pool->size; i++) {
            if (!pool->slots[i].in_use) {
                PGconn *conn = pool->slots[i].conn;
                pool->slots[i].in_use = 1;
                pthread_mutex_unlock(&pool->mu);

                if (PQstatus(conn) != CONNECTION_OK) {
                    pthread_mutex_lock(&pool->mu);
                    pool->slots[i].in_use = 0;
                    PGconn *fresh = slot_reconnect(pool, i);
                    if (!fresh) {
                        pthread_mutex_unlock(&pool->mu);
                        return NULL;
                    }
                    pool->slots[i].in_use = 1;
                    conn = fresh;
                    pthread_mutex_unlock(&pool->mu);
                }
                return conn;
            }
        }

        /* Every connection is out. Wait rather than opening an unbounded
         * number: Postgres's max_connections is the real ceiling and blowing
         * past it would fail the whole database for every other client. */
        if (pthread_cond_timedwait(&pool->cv, &pool->mu, &deadline) == ETIMEDOUT) {
            pthread_mutex_unlock(&pool->mu);
            return NULL;
        }
    }
}

pg_pool_t *pg_pool_new(const char *dsn, int pool_size)
{
    /* A pool of zero would make every query fail at the first checkout, which
     * is a worse outcome than refusing to start. */
    if (!dsn || !*dsn || pool_size < 1)
        return NULL;

    pg_pool_t *pool = calloc(1, sizeof(*pool));
    if (!pool)
        return NULL;

    pool->slots = calloc((size_t)pool_size, sizeof(*pool->slots));
    if (!pool->slots) {
        free(pool);
        return NULL;
    }
    pool->size = pool_size;
    snprintf(pool->dsn, sizeof(pool->dsn), "%s", dsn);
    pthread_mutex_init(&pool->mu, NULL);
    pthread_cond_init(&pool->cv, NULL);

    /* All or nothing. A pool that came up with fewer connections than asked
     * for would look healthy and then fail under the first burst, so the
     * shortfall is reported at startup instead. */
    for (int i = 0; i < pool_size; i++) {
        pool->slots[i].conn = PQconnectdb(dsn);
        if (!pool->slots[i].conn ||
            PQstatus(pool->slots[i].conn) != CONNECTION_OK) {
            pg_pool_free(pool);
            return NULL;
        }
    }

    return pool;
}

void pg_pool_free(pg_pool_t *pool)
{
    if (!pool)
        return;

    for (int i = 0; i < pool->size; i++) {
        if (pool->slots[i].conn) {
            PQfinish(pool->slots[i].conn);
            pool->slots[i].conn = NULL;
        }
    }
    pthread_mutex_destroy(&pool->mu);
    pthread_cond_destroy(&pool->cv);
    free(pool->slots);
    free(pool);
}

int pg_pool_size(const pg_pool_t *pool)
{
    return pool ? pool->size : 0;
}

int pg_pool_in_use(const pg_pool_t *pool)
{
    if (!pool)
        return 0;

    /* The cast is deliberate: taking a lock is not a mutation of the pool, but
     * pthread_mutex_lock cannot express that. */
    pg_pool_t *mutable_pool = (pg_pool_t *)pool;
    pthread_mutex_lock(&mutable_pool->mu);
    int n = 0;
    for (int i = 0; i < pool->size; i++)
        if (pool->slots[i].in_use)
            n++;
    pthread_mutex_unlock(&mutable_pool->mu);
    return n;
}

/* Whether a result failed with a specific SQLSTATE.
 *
 * PQresultErrorField returns NULL when the result carries no such field, which
 * happens for real failures and not only exotic ones: a backend that dies
 * mid-statement, and every error raised before a transaction is established,
 * come back with no SQLSTATE at all. Passing that NULL to strcmp is undefined
 * behaviour, and in practice it segfaults — which is why the first full disk
 * killed the process instead of being reported as the error it was.
 *
 * tests/test_disk_full.c is the regression test for this: it is the only test
 * that reaches an insert failure whose result carries no SQLSTATE. */
static int sqlstate_is(PGresult *r, const char *want)
{
    const char *state = PQresultErrorField(r, PG_DIAG_SQLSTATE);
    return state != NULL && strcmp(state, want) == 0;
}

/* Runs one parameterised statement on a checked-out connection. */
static PGresult *exec_params(pg_pool_t *pool, PGconn *conn, const char *sql,
                             int nparams, const char *const *values)
{
    (void)pool;
    PGresult *r = PQexecParams(conn, sql, nparams, NULL, values, NULL, NULL, 0);
    if (!r) {
        /* A NULL result means PQexecParams itself failed, usually an allocation
         * failure; the connection is left in an unknown state and the caller
         * treats it as an error. */
        return NULL;
    }
    return r;
}

/* Reads a whole file. The schema lives on disk rather than in a C string so it
 * can be read, diffed and applied by hand during the load test. */
static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long size = ftell(f);
    if (size < 0 || size > (1 << 20)) {
        fclose(f);
        return NULL;
    }
    rewind(f);

    char *buf = malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[got] = '\0';
    return buf;
}

/* First day of the month `months` away from now, as a struct tm in UTC. */
static void month_start(time_t now, int months, struct tm *out)
{
    time_t t = now;
    gmtime_r(&t, out);
    /* Normalising the day to 1 first means month arithmetic cannot be thrown off
     * by the 31st of a month that only has 30. */
    out->tm_mday = 1;
    out->tm_hour = 0;
    out->tm_min = 0;
    out->tm_sec = 0;
    out->tm_mon += months;
    /* timegm normalises an out-of-range tm_mon itself, so December plus one
     * rolls the year over without a special case. */
    time_t normalised = timegm(out);
    gmtime_r(&normalised, out);
}

static int create_partition(PGconn *conn, int year, int month)
{
    char sql[256];
    snprintf(sql, sizeof(sql),
             "CREATE TABLE IF NOT EXISTS urls_%04d_%02d PARTITION OF urls "
             "FOR VALUES FROM ('%04d-%02d-01 00:00:00+00') "
             "TO ('%04d-%02d-01 00:00:00+00')",
             year, month, year, month, year, month + 1);

    PGresult *r = PQexec(conn, sql);
    if (!r)
        return PG_ERROR;

    ExecStatusType st = PQresultStatus(r);
    if (st != PGRES_COMMAND_OK)
        fprintf(stderr, "pg: partition urls_%04d_%02d failed: %s", year, month,
                PQresultErrorMessage(r));
    PQclear(r);
    return st == PGRES_COMMAND_OK ? PG_OK : PG_ERROR;
}

int pg_run_schema(pg_pool_t *pool)
{
    if (!pool)
        return PG_ERROR;

    const char *path = getenv("SCHEMA_PATH");
    if (!path || !*path)
        path = "src/db/schema.sql";

    char *sql = read_file(path);
    if (!sql)
        return PG_ERROR;

    PGconn *conn = checkout(pool);
    if (!conn) {
        free(sql);
        return PG_ERROR;
    }

    int rc = PG_OK;

    /* CREATE TABLE IF NOT EXISTS emits a NOTICE per existing relation, which is
     * pure noise once the schema is in place. */
    PGresult *quiet = PQexec(conn, "SET client_min_messages TO warning");
    PQclear(quiet);

    PGresult *r = PQexec(conn, sql);
    if (!r) {
        fprintf(stderr, "pg: schema at %s returned no result: %s\n", path,
                PQerrorMessage(conn));
        rc = PG_ERROR;
    } else if (PQresultStatus(r) != PGRES_COMMAND_OK) {
        /* A failure here is almost always the schema file not being found at
         * the path the binary expects, so the path is in the message. */
        fprintf(stderr, "pg: schema at %s failed: %s", path,
                PQresultErrorMessage(r));
        PQclear(r);
        rc = PG_ERROR;
    } else {
        PQclear(r);
    }
    free(sql);

    /* Previous, current and next. The previous month covers a row whose
     * created_at was set by a clock slightly behind this one; the next covers a
     * row stamped at the end of the month. Both are cheap and neither is a
     * substitute for a scheduled job that keeps creating them. */
    if (rc == PG_OK) {
        time_t now = time(NULL);
        for (int delta = -1; delta <= 1; delta++) {
            struct tm tm_month;
            month_start(now, delta, &tm_month);
            if (create_partition(conn, tm_month.tm_year + 1900,
                                  tm_month.tm_mon + 1) != PG_OK) {
                rc = PG_ERROR;
                break;
            }
        }
    }

    slot_release(pool, conn);
    return rc;
}

int pg_create_url(pg_pool_t *pool, const char *code, const char *url,
                  time_t expires_at)
{
    if (!pool || !code || !url)
        return PG_ERROR;
    /* Refused before the round trip: a URL longer than the column is meant to
     * hold is a client error, and letting the database reject it would surface
     * as a 500. */
    if (strlen(url) > PG_MAX_URL)
        return PG_ERROR;
    if (!code_is_valid_custom(code))
        return PG_ERROR;

    PGconn *conn = checkout(pool);
    if (!conn)
        return PG_ERROR;

    char expires_buf[32];
    const char *values[3];
    values[0] = code;
    values[1] = url;
    if (expires_at == 0) {
        /* A NULL parameter, not the string "0": to_timestamp(0) is 1970, which
         * would make a permanent URL read back as long expired. */
        values[2] = NULL;
    } else {
        snprintf(expires_buf, sizeof(expires_buf), "%lld", (long long)expires_at);
        values[2] = expires_buf;
    }

    /* The CASE is what lets one statement accept both a timestamp and NULL
     * without Postgres having to infer the parameter's type. */
    PGresult *r = exec_params(pool, conn,
        "INSERT INTO urls (code, url, expires_at) "
        "VALUES ($1::varchar, $2::text, "
        "        CASE WHEN $3::text IS NULL THEN NULL "
        "             ELSE to_timestamp($3::double precision) END)",
        3, values);

    int rc = PG_ERROR;
    if (r) {
        ExecStatusType st = PQresultStatus(r);
        if (st == PGRES_COMMAND_OK) {
            rc = PG_OK;
        } else if (sqlstate_is(r, SQLSTATE_UNIQUE_VIOLATION)) {
            rc = 1;
        }
        PQclear(r);
    }

    slot_release(pool, conn);
    return rc;
}

int pg_lookup_url(pg_pool_t *pool, const char *code, char *out_url,
                  size_t out_len, time_t *out_expires_at)
{
    if (!pool || !code || !out_url || out_len == 0)
        return PG_ERROR;

    out_url[0] = '\0';
    if (out_expires_at)
        *out_expires_at = 0;

    PGconn *conn = checkout(pool);
    if (!conn)
        return PG_ERROR;

    const char *values[1] = {code};
    /* The expiry test is in the WHERE clause rather than applied afterwards, so
     * an expired row is never read out of the database at all. */
    PGresult *r = exec_params(pool, conn,
        "SELECT url, COALESCE(EXTRACT(EPOCH FROM expires_at)::bigint, 0) "
        "  FROM urls "
        " WHERE code = $1::varchar "
        "   AND (expires_at IS NULL OR expires_at > now()) "
        " LIMIT 1",
        1, values);

    int rc = PG_ERROR;
    if (r) {
        if (PQresultStatus(r) != PGRES_TUPLES_OK) {
            rc = PG_ERROR;
        } else if (PQntuples(r) == 0) {
            rc = PG_NOT_FOUND;
        } else {
            const char *url = PQgetvalue(r, 0, 0);
            size_t url_len = PQgetlength(r, 0, 0);
            /* Never truncate. A shortened URL redirects somewhere else, which is
             * worse than reporting the failure. */
            if (url_len + 1 > out_len) {
                rc = PG_ERROR;
            } else {
                memcpy(out_url, url, url_len);
                out_url[url_len] = '\0';
                if (out_expires_at)
                    *out_expires_at = (time_t)atoll(PQgetvalue(r, 0, 1));
                rc = PG_OK;
            }
        }
        PQclear(r);
    }

    slot_release(pool, conn);
    return rc;
}

int pg_cleanup_expired(pg_pool_t *pool, time_t now, long *out_deleted)
{
    if (!pool)
        return PG_ERROR;

    PGconn *conn = checkout(pool);
    if (!conn)
        return PG_ERROR;

    char now_buf[32];
    snprintf(now_buf, sizeof(now_buf), "%lld", (long long)now);
    const char *values[1] = {now_buf};

    /* expires_at IS NOT NULL matters: a URL with no expiry is not an expired
     * URL, and without this guard every permanent row would be collected on the
     * first sweep. */
    PGresult *r = exec_params(pool, conn,
        "DELETE FROM urls "
        " WHERE expires_at IS NOT NULL "
        "   AND expires_at <= to_timestamp($1::double precision)",
        1, values);

    int rc = PG_ERROR;
    if (r) {
        if (PQresultStatus(r) == PGRES_COMMAND_OK) {
            if (out_deleted)
                *out_deleted = atol(PQcmdTuples(r));
            rc = PG_OK;
        }
        PQclear(r);
    }

    slot_release(pool, conn);
    return rc;
}