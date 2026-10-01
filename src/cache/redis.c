#include "cache/redis.h"

#include <hiredis/hiredis.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

/* Bounded so a wedged cache cannot park a worker thread indefinitely. The
 * reactor has one core; a thread stuck in a read is a core not serving
 * requests. */
#define REDIS_CONNECT_TIMEOUT_MS 2000
#define REDIS_IO_TIMEOUT_S 2

struct redis_client {
    redisContext *ctx;
};

/* Parses redis://[host][:port][/db]. hiredis has no URL parser, and the DSN in
 * config.yaml is the only form this needs to accept. */
static int parse_url(const char *url, char *host, size_t host_len, int *port,
                     int *db)
{
    *port = 6379;
    *db = 0;

    const char *p = url;
    if (strncmp(p, "redis://", 8) == 0)
        p += 8;
    else if (strncmp(p, "rediss://", 9) == 0)
        return -1; /* TLS is not configured anywhere in this project. */

    if (*p == '\0')
        return -1;

    const char *slash = strchr(p, '/');
    size_t authority_len = slash ? (size_t)(slash - p) : strlen(p);
    if (authority_len == 0 || authority_len >= host_len)
        return -1;

    /* Credentials in the URL are not supported: no configuration uses them, and
     * silently ignoring them would be worse than refusing. */
    if (memchr(p, '@', authority_len) != NULL)
        return -1;

    const char *colon = memchr(p, ':', authority_len);
    if (colon) {
        size_t host_part = (size_t)(colon - p);
        if (host_part == 0 || host_part >= host_len)
            return -1;
        memcpy(host, p, host_part);
        host[host_part] = '\0';

        char port_buf[16];
        size_t port_len = authority_len - host_part - 1;
        if (port_len == 0 || port_len >= sizeof(port_buf))
            return -1;
        memcpy(port_buf, colon + 1, port_len);
        port_buf[port_len] = '\0';
        *port = atoi(port_buf);
        if (*port <= 0 || *port > 65535)
            return -1;
    } else {
        memcpy(host, p, authority_len);
        host[authority_len] = '\0';
    }

    if (slash && slash[1] != '\0') {
        int n = atoi(slash + 1);
        if (n < 0)
            return -1;
        *db = n;
    }

    return 0;
}

redis_client_t *redis_init(const char *url)
{
    char host[256];
    int port = 6379;
    int db = 0;

    if (!url || parse_url(url, host, sizeof(host), &port, &db) != 0)
        return NULL;

    struct timeval connect_timeout = {
        REDIS_CONNECT_TIMEOUT_MS / 1000,
        (REDIS_CONNECT_TIMEOUT_MS % 1000) * 1000
    };
    /* By value: hiredis declares this parameter as `const struct timeval`, not
     * a pointer to one. */
    redisContext *ctx = redisConnectWithTimeout(host, port, connect_timeout);
    if (!ctx)
        return NULL;
    if (ctx->err != 0) {
        redisFree(ctx);
        return NULL;
    }

    redisReply *reply = redisCommand(ctx, "PING");
    if (!reply || reply->type == REDIS_REPLY_ERROR ||
        strcasecmp(reply->str, "PONG") != 0) {
        /* A context that answers but is not usable is worse than no context:
         * the cache would look connected while every command failed. */
        if (reply)
            freeReplyObject(reply);
        redisFree(ctx);
        return NULL;
    }
    freeReplyObject(reply);

    if (db != 0) {
        reply = (redisReply *)redisCommand(ctx, "SELECT %d", db);
        int ok = reply && reply->type != REDIS_REPLY_ERROR;
        if (reply)
            freeReplyObject(reply);
        if (!ok) {
            redisFree(ctx);
            return NULL;
        }
    }

    struct timeval tv = {REDIS_IO_TIMEOUT_S, 0};
    if (redisSetTimeout(ctx, tv) != REDIS_OK) {
        redisFree(ctx);
        return NULL;
    }

    redis_client_t *client = malloc(sizeof(*client));
    if (!client) {
        redisFree(ctx);
        return NULL;
    }
    client->ctx = ctx;
    return client;
}

void redis_free(redis_client_t *client)
{
    if (!client)
        return;
    if (client->ctx)
        redisFree(client->ctx);
    free(client);
}

int redis_get(redis_client_t *client, const char *key, char *out, size_t out_len)
{
    if (!client || !key || !out || out_len == 0)
        return REDIS_ERROR;

    out[0] = '\0';

    redisReply *reply = redisCommand(client->ctx, "GET %b", key, strlen(key));
    if (!reply)
        return REDIS_ERROR;

    int rc;
    if (reply->type == REDIS_REPLY_NIL) {
        rc = REDIS_MISS;
    } else if (reply->type == REDIS_REPLY_ERROR) {
        rc = REDIS_ERROR;
    } else if (reply->type == REDIS_REPLY_STRING) {
        /* Refuse rather than truncate: a shortened URL redirects to a different
         * site, and that failure would be invisible. */
        if ((size_t)reply->len + 1 > out_len) {
            rc = REDIS_ERROR;
        } else {
            memcpy(out, reply->str, reply->len);
            out[reply->len] = '\0';
            rc = REDIS_OK;
        }
    } else {
        rc = REDIS_ERROR;
    }

    freeReplyObject(reply);
    return rc;
}

int redis_setex(redis_client_t *client, const char *key, const char *value,
                int ttl)
{
    if (!client || !key || !value)
        return REDIS_ERROR;
    /* A non-positive TTL would be SET without expiry. The URLs are permanent,
     * so a cached copy with no expiry is an entry nothing will ever collect. */
    if (ttl <= 0)
        return REDIS_ERROR;

    redisReply *reply = (redisReply *)redisCommand(client->ctx, "SETEX %b %d %b",
                                                  key, strlen(key), ttl,
                                                  value, strlen(value));
    if (!reply)
        return REDIS_ERROR;

    int rc = (reply->type == REDIS_REPLY_STATUS && reply->str &&
              strcmp(reply->str, "OK") == 0) ? REDIS_OK : REDIS_ERROR;
    freeReplyObject(reply);
    return rc;
}

int redis_invalidate(redis_client_t *client, const char *key)
{
    if (!client || !key)
        return REDIS_ERROR;

    redisReply *reply = (redisReply *)redisCommand(client->ctx, "DEL %b", key,
                                                  strlen(key));
    if (!reply)
        return REDIS_ERROR;

    /* Whether or not the key existed, the caller's intent is now satisfied. */
    int rc = (reply->type == REDIS_REPLY_INTEGER) ? REDIS_OK : REDIS_ERROR;
    freeReplyObject(reply);
    return rc;
}

int redis_ttl(redis_client_t *client, const char *key)
{
    if (!client || !key)
        return REDIS_ERROR;

    redisReply *reply = (redisReply *)redisCommand(client->ctx, "TTL %b", key,
                                                  strlen(key));
    if (!reply)
        return REDIS_ERROR;

    int rc = REDIS_ERROR;
    if (reply->type == REDIS_REPLY_INTEGER)
        rc = (int)reply->integer;
    freeReplyObject(reply);
    return rc;
}